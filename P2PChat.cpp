#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "AudioEngine.hpp"
#include "CandidateGatherer.hpp"
#include "IceAgent.hpp"
#include "STUN.hpp"
#include "ScreenShareEngine.hpp"
#include "SignallingClient.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

constexpr uint8_t PACKET_TEXT = 0x01;
constexpr uint8_t PACKET_AUDIO = 0x02;
constexpr uint8_t PACKET_SCREEN = 0x03;
constexpr size_t SCREEN_CHUNK_PAYLOAD = 1100;
constexpr size_t MAX_SCREEN_CHUNKS = 512;

#pragma pack(push, 1)
struct ScreenChunkHeader
{
    uint16_t frameId;
    uint16_t chunkIndex;
    uint16_t chunkCount;
    uint32_t totalSize;
};
#pragma pack(pop)

struct ScreenRxAssembly
{
    uint16_t frameId = 0;
    uint16_t chunkCount = 0;
    uint32_t totalSize = 0;
    std::vector<std::vector<uint8_t>> chunks;
    std::vector<uint8_t> received;
    size_t receivedCount = 0;
    size_t receivedBytes = 0;
    std::chrono::steady_clock::time_point lastUpdate =
        std::chrono::steady_clock::now();
};

struct PeerState
{
    std::shared_ptr<IceAgent> agent;
    std::chrono::steady_clock::time_point lastPingSent;
    std::atomic<bool> reconnecting{false};
    std::atomic<bool> iceStarted{false};
    std::atomic<bool> negotiationReady{false};
    std::atomic<unsigned long long> audioTxPackets{0};
    std::atomic<unsigned long long> audioRxPackets{0};
    std::atomic<unsigned long long> audioTxDrops{0};
    std::atomic<unsigned long long> audioTxErrors{0};
};

struct ScheduledTask
{
    std::thread worker;
    std::shared_ptr<std::atomic<bool>> cancelled;
};

struct P2PClient
{
    std::map<std::string, std::weak_ptr<IceAgent>> addrToAgent;
    std::mutex addrMapMtx;
    std::string id;
    std::atomic<bool> running{true};

    SignallingClient signalling;
    CandidateGatherer gatherer;
    AudioEngine audio;
    ScreenShareEngine screenShare;

    std::map<std::string, std::shared_ptr<PeerState>> peers;
    std::mutex peersMtx;

    std::vector<Candidate> myLocalCandidates;
    std::mutex localCandidatesMtx;

    std::mutex screenShareMtx;
    std::string screenShareTarget = "*";
    std::atomic<uint16_t> nextScreenFrameId{1};
    std::mutex screenRxMtx;
    std::unordered_map<std::string, ScreenRxAssembly> screenRxBuffers;
    std::unordered_map<std::string, ScreenShareEngine::PreviewFrame>
        remoteScreenPreviews;
    std::atomic<uint64_t> remoteScreenPreviewSeq{1};

    HANDLE cp = nullptr;
    std::thread worker;
    std::thread pingWorker;
    std::atomic<bool> started{false};

    std::map<std::string, ScheduledTask> pendingTasks;
    std::mutex pendingICEMtx;

    std::shared_ptr<IceAgent> GetExistingAgent(const std::string &peerId)
    {
        std::lock_guard<std::mutex> lock(peersMtx);
        auto it = peers.find(peerId);
        if (it != peers.end() && it->second && it->second->agent)
        {
            return it->second->agent;
        }
        return nullptr;
    }

    std::shared_ptr<PeerState> GetExistingPeerState(const std::string &peerId)
    {
        std::lock_guard<std::mutex> lock(peersMtx);
        auto it = peers.find(peerId);
        if (it != peers.end())
        {
            return it->second;
        }
        return nullptr;
    }

    std::shared_ptr<IceAgent> GetOrCreateAgent(const std::string &peerId)
    {

        {
            std::lock_guard<std::mutex> lock(peersMtx);
            auto it = peers.find(peerId);
            if (it != peers.end() && it->second && it->second->agent)
            {
                if (!it->second->reconnecting.load())
                {
                    std::cout << "[Agent] Reusing existing agent for " << peerId
                              << "\n";
                    return it->second->agent;
                }
            }
        }

        std::vector<Candidate> localsCopy;
        {
            std::lock_guard<std::mutex> lock(localCandidatesMtx);
            localsCopy = myLocalCandidates;
        }

        std::lock_guard<std::mutex> lock(peersMtx);

        auto newAgent = std::make_shared<IceAgent>(cp);
        newAgent->peerId = peerId;
        newAgent->onAudioReceived = [this, peerId](void *d, int l)
        {
            auto state = GetExistingPeerState(peerId);
            if (state)
            {
                auto packetIndex = state->audioRxPackets.fetch_add(1) + 1;
                if (packetIndex <= 5 || packetIndex % 50 == 0)
                {
                    std::cout << "[Audio] RX from " << peerId << " packet #"
                              << packetIndex << " len=" << l << std::endl;
                }
            }
            audio.PlayFrameFromSource(peerId, d, l);
        };
        newAgent->onScreenChunkReceived = [this, peerId](const void *d, int l)
        { HandleIncomingScreenChunk(peerId, d, l); };
        newAgent->onDisconnected = [this, peerId]()
        {
            std::cout << "[System] Connection lost to " << peerId
                      << ", scheduling reconnect...\n";
            ScheduleReconnect(peerId);
        };

        for (const auto &local : localsCopy)
        {
            newAgent->AddLocalCandidate(local);
        }

        auto state = std::make_shared<PeerState>();
        state->agent = newAgent;
        state->iceStarted = false;
        state->negotiationReady = false;
        peers[peerId] = state;

        std::cout << "[Agent] Created new agent for " << peerId << "\n";
        return newAgent;
    }

    std::string IceTaskKey(const std::string &peerId) const
    {
        return "ice:" + peerId;
    }

    std::string ReconnectTaskKey(const std::string &peerId) const
    {
        return "reconnect:" + peerId;
    }

    void CancelScheduledTask(const std::string &taskKey)
    {
        std::thread worker;
        std::shared_ptr<std::atomic<bool>> cancelled;

        {
            std::lock_guard<std::mutex> lock(pendingICEMtx);
            auto it = pendingTasks.find(taskKey);
            if (it == pendingTasks.end())
            {
                return;
            }

            cancelled = it->second.cancelled;
            if (cancelled)
            {
                cancelled->store(true, std::memory_order_release);
            }

            if (it->second.worker.joinable())
            {
                if (it->second.worker.get_id() == std::this_thread::get_id())
                {
                    it->second.worker.detach();
                }
                else
                {
                    worker = std::move(it->second.worker);
                }
            }
            pendingTasks.erase(it);
        }

        if (worker.joinable())
        {
            worker.join();
        }
    }

    template <typename Action>
    void ScheduleTask(const std::string &taskKey, int delayMs, Action &&action)
    {
        CancelScheduledTask(taskKey);

        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        ScheduledTask task;
        task.cancelled = cancelled;
        task.worker = std::thread(
            [this, delayMs, cancelled,
             action = std::forward<Action>(action)]() mutable
            {
                auto deadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(delayMs);
                while (running.load() &&
                       !cancelled->load(std::memory_order_acquire) &&
                       std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }

                if (!running.load() ||
                    cancelled->load(std::memory_order_acquire))
                {
                    return;
                }

                action();
            });

        std::lock_guard<std::mutex> lock(pendingICEMtx);
        pendingTasks[taskKey] = std::move(task);
    }

    void JoinScheduledTasks()
    {
        std::vector<std::thread> workers;

        {
            std::lock_guard<std::mutex> lock(pendingICEMtx);
            for (auto &[taskKey, task] : pendingTasks)
            {
                if (task.cancelled)
                {
                    task.cancelled->store(true, std::memory_order_release);
                }
                if (task.worker.joinable())
                {
                    workers.push_back(std::move(task.worker));
                }
            }
            pendingTasks.clear();
        }

        for (auto &worker : workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }
    }

    void CancelPendingICE(const std::string &peerId)
    {
        CancelScheduledTask(IceTaskKey(peerId));
    }

    void ScheduleDelayedICE(const std::string &peerId, int delayMs)
    {
        ScheduleTask(IceTaskKey(peerId), delayMs,
                     [this, peerId, delayMs]()
                     {
                         std::cout << "[ICE] Delayed start for " << peerId
                                   << " after " << delayMs << "ms\n";
                         TryStartICE(peerId, true);
                     });
    }

    void ScheduleReconnect(const std::string &peerId)
    {
        {
            std::lock_guard<std::mutex> lock(peersMtx);
            auto it = peers.find(peerId);
            if (it != peers.end())
            {
                it->second->reconnecting = true;
                it->second->iceStarted = false;
                it->second->negotiationReady = false;
            }
        }

        CancelPendingICE(peerId);
        CancelScheduledTask(ReconnectTaskKey(peerId));

        ScheduleTask(ReconnectTaskKey(peerId), 2000,
                     [this, peerId]()
                     {
                         std::cout << "[System] Attempting reconnect to "
                                   << peerId << "...\n";

                         {
                             std::lock_guard<std::mutex> lock(peersMtx);
                             peers.erase(peerId);
                         }

                         signalling.SendOffer(peerId);
                     });
    }

    void TryStartICE(const std::string &peerId, bool force = false)
    {
        std::shared_ptr<PeerState> state;
        {
            std::lock_guard<std::mutex> lock(peersMtx);
            auto it = peers.find(peerId);
            if (it != peers.end())
            {
                state = it->second;
            }
        }

        if (!state)
        {
            std::cout << "[ICE] No state for " << peerId << ", cannot start\n";
            return;
        }

        if (state->reconnecting.load())
        {
            std::cout << "[ICE] Waiting for reconnect to " << peerId << "...\n";
            return;
        }

        if (!force && state->iceStarted.load())
        {
            std::cout << "[ICE] Already started for " << peerId
                      << ", skipping\n";
            return;
        }

        auto [success, sock, remote] = state->agent->GetActiveConnection();
        if (!success)
        {
            bool started = state->agent->StartConnectivityChecks(
                [this, peerId](bool success, const CandidatePair &pair)
                {
                    if (success)
                    {
                        std::cout << "\n[" << id << "] ICE Success with "
                                  << peerId << "! Path: " << pair.local.ip
                                  << " -> " << pair.remote.ip << std::endl;

                        std::lock_guard<std::mutex> lock(peersMtx);
                        auto it = peers.find(peerId);
                        if (it != peers.end())
                        {
                            it->second->reconnecting = false;
                        }
                    }
                    else
                    {
                        std::cout << "\n[" << id << "] ICE failed with "
                                  << peerId << std::endl;
                        std::lock_guard<std::mutex> lock(peersMtx);
                        auto it = peers.find(peerId);
                        if (it != peers.end())
                        {
                            it->second->iceStarted = false;
                        }
                    }
                });

            if (started)
            {
                state->iceStarted = true;
                CancelPendingICE(peerId);
                std::cout << "[ICE] Started checks for " << peerId << std::endl;
            }
            else
            {
                std::cout << "[ICE] Cannot start checks for " << peerId
                          << " yet (no remote candidates?)\n";
            }
        }
        else
        {
            std::cout << "[ICE] Already established with " << peerId
                      << ", skipping new checks.\n";
        }
    }

    void InitMyCandidates()
    {
        gatherer.StartGathering(
            "stun.l.google.com", 19302,
            [this](std::vector<Candidate> locals)
            {
                std::lock_guard<std::mutex> lock(localCandidatesMtx);
                myLocalCandidates = locals;
                std::cout << "[System] My local candidates ready. Now you can "
                             "accept offers.\n";
            });
    }

    bool TryGetActiveSocket(const std::shared_ptr<IceAgent> &agent,
                            SOCKET &outSock, sockaddr_in &outAddr)
    {
        if (!agent)
            return false;

        auto [success, sock, remote] = agent->GetActiveConnection();

        if (!success || sock == INVALID_SOCKET)
        {
            return false;
        }

        outSock = sock;
        outAddr.sin_family = AF_INET;
        inet_pton(AF_INET, remote.ip.c_str(), &outAddr.sin_addr);
        outAddr.sin_port = htons(remote.port);

        return true;
    }

    struct ScreenTargetEndpoint
    {
        std::string peerId;
        SOCKET socket = INVALID_SOCKET;
        sockaddr_in addr{};
    };

    std::vector<ScreenTargetEndpoint>
    ResolveScreenTargets(const std::string &target)
    {
        std::vector<std::pair<std::string, std::shared_ptr<IceAgent>>> agents;
        {
            std::lock_guard<std::mutex> lock(peersMtx);
            if (target == "*" || target == "all")
            {
                for (const auto &[peerId, state] : peers)
                {
                    if (state && state->agent && !state->reconnecting.load())
                    {
                        agents.emplace_back(peerId, state->agent);
                    }
                }
            }
            else
            {
                auto it = peers.find(target);
                if (it != peers.end() && it->second && it->second->agent &&
                    !it->second->reconnecting.load())
                {
                    agents.emplace_back(it->first, it->second->agent);
                }
            }
        }

        std::vector<ScreenTargetEndpoint> endpoints;
        endpoints.reserve(agents.size());
        for (const auto &[peerId, agent] : agents)
        {
            SOCKET outSock = INVALID_SOCKET;
            sockaddr_in addr{};
            if (TryGetActiveSocket(agent, outSock, addr))
            {
                ScreenTargetEndpoint endpoint;
                endpoint.peerId = peerId;
                endpoint.socket = outSock;
                endpoint.addr = addr;
                endpoints.push_back(endpoint);
            }
        }
        return endpoints;
    }

    void SendScreenFrame(const ScreenShareEngine::EncodedFrame &frame)
    {
        if (frame.jpeg.empty())
        {
            return;
        }

        std::string target;
        {
            std::lock_guard<std::mutex> lock(screenShareMtx);
            target = screenShareTarget;
        }

        const auto endpoints = ResolveScreenTargets(target);
        if (endpoints.empty())
        {
            return;
        }

        const size_t totalSize = frame.jpeg.size();
        const size_t chunkCount =
            (totalSize + SCREEN_CHUNK_PAYLOAD - 1) / SCREEN_CHUNK_PAYLOAD;
        if (chunkCount == 0 || chunkCount > MAX_SCREEN_CHUNKS)
        {
            return;
        }

        const uint16_t frameId = nextScreenFrameId.fetch_add(1);
        for (size_t i = 0; i < chunkCount; ++i)
        {
            const size_t offset = i * SCREEN_CHUNK_PAYLOAD;
            const size_t chunkSize =
                std::min(SCREEN_CHUNK_PAYLOAD, totalSize - offset);

            ScreenChunkHeader header{};
            header.frameId = htons(frameId);
            header.chunkIndex = htons(static_cast<uint16_t>(i));
            header.chunkCount = htons(static_cast<uint16_t>(chunkCount));
            header.totalSize = htonl(static_cast<uint32_t>(totalSize));

            std::vector<uint8_t> packet;
            packet.resize(1 + sizeof(ScreenChunkHeader) + chunkSize);
            packet[0] = PACKET_SCREEN;
            memcpy(packet.data() + 1, &header, sizeof(ScreenChunkHeader));
            memcpy(packet.data() + 1 + sizeof(ScreenChunkHeader),
                   frame.jpeg.data() + offset, chunkSize);

            for (const auto &endpoint : endpoints)
            {
                sendto(endpoint.socket,
                       reinterpret_cast<const char *>(packet.data()),
                       static_cast<int>(packet.size()), 0,
                       reinterpret_cast<const sockaddr *>(&endpoint.addr),
                       sizeof(endpoint.addr));
            }
        }
    }

    void HandleIncomingScreenChunk(const std::string &peerId, const void *data,
                                   int len)
    {
        if (!data || len <= static_cast<int>(sizeof(ScreenChunkHeader)))
        {
            return;
        }

        const auto *bytes = reinterpret_cast<const uint8_t *>(data);
        ScreenChunkHeader header{};
        memcpy(&header, bytes, sizeof(ScreenChunkHeader));

        const uint16_t frameId = ntohs(header.frameId);
        const uint16_t chunkIndex = ntohs(header.chunkIndex);
        const uint16_t chunkCount = ntohs(header.chunkCount);
        const uint32_t totalSize = ntohl(header.totalSize);
        const uint8_t *payload = bytes + sizeof(ScreenChunkHeader);
        const int payloadLen =
            len - static_cast<int>(sizeof(ScreenChunkHeader));

        if (chunkCount == 0 || chunkCount > MAX_SCREEN_CHUNKS ||
            chunkIndex >= chunkCount || payloadLen <= 0 || totalSize == 0 ||
            totalSize > 8 * 1024 * 1024)
        {
            return;
        }

        std::vector<uint8_t> completedJpeg;
        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(screenRxMtx);

            for (auto it = screenRxBuffers.begin();
                 it != screenRxBuffers.end();)
            {
                if (now - it->second.lastUpdate > std::chrono::seconds(5))
                {
                    it = screenRxBuffers.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            auto &assembly = screenRxBuffers[peerId];
            if (assembly.frameId != frameId ||
                assembly.chunkCount != chunkCount ||
                assembly.totalSize != totalSize)
            {
                assembly = ScreenRxAssembly{};
                assembly.frameId = frameId;
                assembly.chunkCount = chunkCount;
                assembly.totalSize = totalSize;
                assembly.chunks.resize(chunkCount);
                assembly.received.assign(chunkCount, 0);
                assembly.lastUpdate = now;
            }

            assembly.lastUpdate = now;
            if (!assembly.received[chunkIndex])
            {
                assembly.chunks[chunkIndex].assign(payload,
                                                   payload + payloadLen);
                assembly.received[chunkIndex] = 1;
                assembly.receivedCount++;
                assembly.receivedBytes += payloadLen;
            }

            if (assembly.receivedCount == assembly.chunkCount)
            {
                completedJpeg.reserve(assembly.receivedBytes);
                for (const auto &chunk : assembly.chunks)
                {
                    completedJpeg.insert(completedJpeg.end(), chunk.begin(),
                                         chunk.end());
                }
                if (completedJpeg.size() > assembly.totalSize)
                {
                    completedJpeg.resize(assembly.totalSize);
                }
                screenRxBuffers.erase(peerId);
            }
        }

        if (completedJpeg.empty())
        {
            return;
        }

        ScreenShareEngine::PreviewFrame decoded;
        if (!screenShare.DecodeJpegToPreview(completedJpeg, decoded))
        {
            return;
        }

        decoded.sequence = remoteScreenPreviewSeq.fetch_add(1) + 1;
        {
            std::lock_guard<std::mutex> lock(screenRxMtx);
            remoteScreenPreviews[peerId] = std::move(decoded);
        }
    }

    bool StartScreenShare(const std::string &target)
    {
        if (target.empty())
        {
            return false;
        }

        StopScreenShare();
        {
            std::lock_guard<std::mutex> lock(screenShareMtx);
            screenShareTarget = target;
        }

        const bool started = screenShare.Start(
            [this](const ScreenShareEngine::EncodedFrame &frame)
            { SendScreenFrame(frame); });

        if (started)
        {
            std::cout << "[Screen] Sharing started to " << target << std::endl;
        }
        else
        {
            std::cout
                << "[Screen] Unable to start capture (GDI+/encoder init failed)"
                << std::endl;
        }
        return started;
    }

    void StopScreenShare()
    {
        if (screenShare.IsRunning())
        {
            screenShare.Stop();
            std::cout << "[Screen] Sharing stopped" << std::endl;
        }
    }

    bool IsScreenShareRunning() const
    {
        return screenShare.IsRunning();
    }

    void SetScreenShareFps(int value)
    {
        screenShare.SetFps(value);
    }

    int GetScreenShareFps() const
    {
        return screenShare.GetFps();
    }

    void SetScreenShareQuality(int value)
    {
        screenShare.SetQuality(value);
    }

    int GetScreenShareQuality() const
    {
        return screenShare.GetQuality();
    }

    void SetScreenShareScalePercent(int value)
    {
        screenShare.SetScalePercent(value);
    }

    int GetScreenShareScalePercent() const
    {
        return screenShare.GetScalePercent();
    }

    std::string GetScreenShareTarget()
    {
        std::lock_guard<std::mutex> lock(screenShareMtx);
        return screenShareTarget;
    }

    bool GetLocalScreenPreview(ScreenShareEngine::PreviewFrame &out) const
    {
        return screenShare.GetLatestPreview(out);
    }

    bool GetLatestRemoteScreenPreview(std::string &peerId,
                                      ScreenShareEngine::PreviewFrame &out)
    {
        std::lock_guard<std::mutex> lock(screenRxMtx);
        if (remoteScreenPreviews.empty())
        {
            return false;
        }

        const auto best = std::max_element(
            remoteScreenPreviews.begin(), remoteScreenPreviews.end(),
            [](const auto &a, const auto &b)
            { return a.second.sequence < b.second.sequence; });
        if (best == remoteScreenPreviews.end())
        {
            return false;
        }

        peerId = best->first;
        out = best->second;
        return !out.bgra.empty();
    }

    std::vector<std::string> GetRemoteScreenStreamIds()
    {
        std::lock_guard<std::mutex> lock(screenRxMtx);
        std::vector<std::string> ids;
        ids.reserve(remoteScreenPreviews.size());
        for (const auto &[peerId, frame] : remoteScreenPreviews)
        {
            if (!frame.bgra.empty() && frame.width > 0 && frame.height > 0)
            {
                ids.push_back(peerId);
            }
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    bool GetRemoteScreenPreviewByPeer(const std::string &peerId,
                                      ScreenShareEngine::PreviewFrame &out)
    {
        std::lock_guard<std::mutex> lock(screenRxMtx);
        auto it = remoteScreenPreviews.find(peerId);
        if (it == remoteScreenPreviews.end())
        {
            return false;
        }
        if (it->second.bgra.empty() || it->second.width <= 0 ||
            it->second.height <= 0)
        {
            return false;
        }
        out = it->second;
        return true;
    }

    void BroadcastMessage(const std::string &text)
    {
        std::string encrypted = XorKey(text);
        std::vector<char> buffer;
        buffer.push_back(PACKET_TEXT);
        buffer.insert(buffer.end(), encrypted.begin(), encrypted.end());
        int count = 0;

        std::vector<std::shared_ptr<IceAgent>> agentsCopy;
        {
            std::lock_guard<std::mutex> lock(peersMtx);
            for (auto const &[peerId, state] : peers)
            {
                if (!state || !state->agent)
                    continue;
                auto [isConnected, sock, remote] =
                    state->agent->GetActiveConnection();
                if (isConnected && sock != INVALID_SOCKET)
                {
                    agentsCopy.push_back(state->agent);
                }
            }
        }

        for (auto &agent : agentsCopy)
        {
            SOCKET outSock;
            sockaddr_in addr{};
            if (TryGetActiveSocket(agent, outSock, addr))
            {
                sendto(outSock, buffer.data(), (int)buffer.size(), 0,
                       (sockaddr *)&addr, sizeof(addr));
                count++;
            }
        }

        std::cout << "[System] Broadcast sent to " << count << " peers."
                  << std::endl;
    }

    void ProcessAgents()
    {
        std::vector<std::shared_ptr<IceAgent>> agentsCopy;
        {
            std::lock_guard<std::mutex> lock(peersMtx);
            for (auto const &[peerId, state] : peers)
            {
                if (!state || !state->agent || state->reconnecting.load())
                {
                    continue;
                }
                agentsCopy.push_back(state->agent);
            }
        }

        for (auto &agent : agentsCopy)
        {
            agent->Process();
        }
    }

    std::string XorKey(std::string data)
    {
        const char key = 0x55;
        for (size_t i = 0; i < data.size(); ++i)
        {
            data[i] ^= key;
        }
        return data;
    }

    void PingLoop()
    {
        constexpr int PING_INTERVAL_MS = 25000;

        while (running.load())
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(PING_INTERVAL_MS));

            if (!running.load())
                break;

            {
                std::lock_guard<std::mutex> lock(peersMtx);
                for (auto &[peerId, state] : peers)
                {
                    if (!state || !state->agent || state->reconnecting.load())
                        continue;

                    auto [isConnected, sock, remote] =
                        state->agent->GetActiveConnection();
                    if (!isConnected || sock == INVALID_SOCKET)
                        continue;

                    state->agent->SendPing();
                    state->lastPingSent = std::chrono::steady_clock::now();
                }
            }

            std::vector<std::string> timedOutPeers;
            {
                std::lock_guard<std::mutex> lock(peersMtx);
                for (auto &[peerId, state] : peers)
                {
                    if (!state || !state->agent || state->reconnecting.load())
                        continue;

                    if (state->agent->CheckActivityTimeout())
                    {
                        timedOutPeers.push_back(peerId);
                    }
                }
            }

            for (const auto &peerId : timedOutPeers)
            {
                auto agent = GetExistingAgent(peerId);
                if (agent && agent->onDisconnected)
                {
                    std::cout << "[System] Activity timeout for " << peerId
                              << ", connection dead\n";
                    agent->onDisconnected();
                }
            }
        }
    }

    bool TryParseCandidateMessage(const std::string &data, Candidate &cand)
    {
        if (data.find("host") == 0)
        {
            char ip[64] = {};
            int port = 0;
            int scanned = sscanf_s(data.c_str(), "host %63[^:]:%d", ip,
                                   (unsigned)sizeof(ip), &port);
            if (scanned != 2)
            {
                return false;
            }

            cand.type = HOST;
            cand.ip = ip;
            cand.port = static_cast<uint16_t>(port);
            return true;
        }

        if (data.find("srflx") == 0)
        {
            char rip[64] = {};
            char lip[64] = {};
            int rport = 0;
            int lport = 0;
            int scanned =
                sscanf_s(data.c_str(), "srflx %63[^:]:%d related %63[^:]:%d",
                         rip, (unsigned)sizeof(rip), &rport, lip,
                         (unsigned)sizeof(lip), &lport);

            if (scanned != 4)
            {
                return false;
            }

            cand.type = SERVER_REFLEXIVE;
            cand.ip = rip;
            cand.port = static_cast<uint16_t>(rport);
            cand.relatedIp = lip;
            cand.relatedPort = static_cast<uint16_t>(lport);
            return true;
        }

        return false;
    }

    void RegisterPeerAddress(const std::string &ip, uint16_t port,
                             std::shared_ptr<IceAgent> agent)
    {
        std::lock_guard<std::mutex> lock(addrMapMtx);
        std::string addrKey = ip + ":" + std::to_string(port);
        addrToAgent[addrKey] = agent;
    }

    std::vector<std::string> GetPeerIds()
    {
        std::vector<std::string> peerIds;
        std::lock_guard<std::mutex> lock(peersMtx);
        peerIds.reserve(peers.size());
        for (const auto &[peerId, state] : peers)
        {
            if (state)
            {
                peerIds.push_back(peerId);
            }
        }
        return peerIds;
    }

    float GetPeerVolume(const std::string &peerId)
    {
        return audio.GetSourceVolume(peerId);
    }

    void SetPeerVolume(const std::string &peerId, float volume)
    {
        const float clamped = std::clamp(volume, 0.0f, 2.0f);
        const float current = audio.GetSourceVolume(peerId);
        if (current >= clamped - 0.005f && current <= clamped + 0.005f)
        {
            return;
        }

        audio.SetSourceVolume(peerId, clamped);
        std::cout << "[AudioCtl] " << peerId << " volume set to "
                  << static_cast<int>(clamped * 100.0f + 0.5f) << "%"
                  << std::endl;
    }

    bool IsEchoSuppressionEnabled()
    {
        return audio.IsEchoSuppressionEnabled();
    }

    void SetEchoSuppressionEnabled(bool enabled)
    {
        audio.SetEchoSuppressionEnabled(enabled);
        std::cout << "[AudioCtl] Echo suppression " << (enabled ? "ON" : "OFF")
                  << std::endl;
    }

    float GetEchoCorrelationThreshold()
    {
        return audio.GetEchoCorrelationThreshold();
    }

    void SetEchoCorrelationThreshold(float value)
    {
        audio.SetEchoCorrelationThreshold(value);
        std::cout << "[AudioCtl] Echo correlation threshold "
                  << static_cast<int>(
                         audio.GetEchoCorrelationThreshold() * 100.0f + 0.5f)
                  << "%" << std::endl;
    }

    float GetEchoSubtractionMaxGain()
    {
        return audio.GetEchoSubtractionMaxGain();
    }

    void SetEchoSubtractionMaxGain(float value)
    {
        audio.SetEchoSubtractionMaxGain(value);
        std::cout << "[AudioCtl] Echo subtraction gain x"
                  << audio.GetEchoSubtractionMaxGain() << std::endl;
    }

    float GetEchoResidualAttenuation()
    {
        return audio.GetEchoResidualAttenuation();
    }

    void SetEchoResidualAttenuation(float value)
    {
        audio.SetEchoResidualAttenuation(value);
        std::cout << "[AudioCtl] Echo residual attenuation "
                  << static_cast<int>(
                         audio.GetEchoResidualAttenuation() * 100.0f + 0.5f)
                  << "%" << std::endl;
    }

    void PrintHelp()
    {
        std::cout << "[" << id << "] Ready. Commands:\n"
                  << "  register [id]    - send REGISTER (reuse or change id)\n"
                  << "  offer <id>       - send offer to peer\n"
                  << "  msg <id> <text>  - send message via ICE\n"
                  << "  smsg <id> <text> - send message via server relay\n"
                  << "  brd <text>       - broadcast to all peers\n"
                  << "  list             - list online users\n"
                  << "  voice on <id>    - start voice to peer\n"
                  << "  brdvoice on      - broadcast voice\n"
                  << "  voice off        - stop recording\n"
                  << "  volume <id> <0-200> - set playback volume for peer\n"
                  << "  stream on <id|*> - start screen sharing\n"
                  << "  stream off       - stop screen sharing\n"
                  << "  stream fps <n>   - set screen FPS (1-60)\n"
                  << "  stream quality <n> - set JPEG quality (20-95)\n"
                  << "  stream scale <n> - set capture scale % (20-100)\n"
                  << "  reconnect <id>   - force reconnect to peer\n\n";
    }

    void Start(const char *serverIp, int serverPort)
    {
        if (started.exchange(true))
        {
            return;
        }

        running = true;

        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        cp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

        InitMyCandidates();
        SetConsoleTitleA(("P2P Chat - ID: " + id).c_str());
        audio.StartPlaying();

        worker = std::thread(
            [this]()
            {
                auto lastProcessTick = std::chrono::steady_clock::now();
                while (running.load())
                {
                    DWORD bytes;
                    ULONG_PTR key;
                    LPOVERLAPPED ov;
                    BOOL ok =
                        GetQueuedCompletionStatus(cp, &bytes, &key, &ov, 100);
                    DWORD completionError = ok ? ERROR_SUCCESS : GetLastError();

                    auto now = std::chrono::steady_clock::now();
                    if (now - lastProcessTick >= std::chrono::milliseconds(100))
                    {
                        ProcessAgents();
                        lastProcessTick = now;
                    }

                    if (!ov)
                        continue;

                    auto *ctx = CONTAINING_RECORD(ov, IceAgent::IceUdpContext,
                                                  overlapped);

                    if (ctx->signature != IceAgent::IceUdpContext::SIGNATURE)
                    {
                        continue;
                    }

                    if (ctx->detached.load(std::memory_order_acquire))
                    {
                        continue;
                    }

                    char fromIp[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &ctx->remoteAddr.sin_addr, fromIp,
                              sizeof(fromIp));
                    std::string addrKey =
                        std::string(fromIp) + ":" +
                        std::to_string(ntohs(ctx->remoteAddr.sin_port));

                    std::shared_ptr<IceAgent> targetAgent = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(addrMapMtx);
                        auto it = addrToAgent.find(addrKey);
                        if (it != addrToAgent.end())
                        {
                            targetAgent = it->second.lock();
                        }
                    }

                    IceAgent *ctxAgent =
                        ctx->agent.load(std::memory_order_acquire);
                    if (!ctxAgent ||
                        ctx->detached.load(std::memory_order_acquire))
                    {
                        continue;
                    }

                    if (!ok && completionError == ERROR_OPERATION_ABORTED)
                    {
                        continue;
                    }

                    if (bytes == 0)
                    {
                        if (!ctx->detached.load(std::memory_order_acquire))
                        {
                            ctxAgent->RearmReceiveContext(ctx);
                        }
                        continue;
                    }

                    if (targetAgent && targetAgent.get() != ctxAgent)
                    {
                        targetAgent->OnUdpPacket(ctx->socket, ctx->remoteAddr,
                                                 ctx->buffer,
                                                 static_cast<int>(bytes));
                        if (!ctx->detached.load(std::memory_order_acquire))
                        {
                            ctxAgent->RearmReceiveContext(ctx);
                        }
                        continue;
                    }

                    if (!ctx->detached.load(std::memory_order_acquire))
                    {
                        ctxAgent->OnReceiveCompleted(ctx, bytes);
                    }
                }
            });

        pingWorker = std::thread(&P2PClient::PingLoop, this);

        signalling.Connect(
            serverIp, serverPort, id,
            [this](const std::string &from, const std::string &type,
                   const std::string &data)
            {
                if (type == "offer")
                {
                    std::cout << "\n[" << id << "] Offer from " << from
                              << ". Gathering..." << std::endl;

                    auto agent = GetOrCreateAgent(from);

                    {
                        std::lock_guard<std::mutex> lock(peersMtx);
                        auto it = peers.find(from);
                        if (it != peers.end() &&
                            it->second->reconnecting.load())
                        {
                            std::cout << "[System] Already reconnecting to "
                                      << from << ", ignoring offer\n";
                            return;
                        }
                    }

                    gatherer.StartGathering(
                        "stun.l.google.com", 19302,
                        [this, from, agent](std::vector<Candidate> locals)
                        {
                            {
                                std::lock_guard<std::mutex> lock(
                                    localCandidatesMtx);
                                myLocalCandidates = locals;
                            }
                            for (auto &c : locals)
                                agent->AddLocalCandidate(c);

                            for (const auto &cand : agent->GetLocalCandidates())
                            {
                                std::string candStr =
                                    (cand.type == HOST)
                                        ? "host " + cand.ip + ":" +
                                              std::to_string(cand.port)
                                        : "srflx " + cand.ip + ":" +
                                              std::to_string(cand.port) +
                                              " related " + cand.relatedIp +
                                              ":" +
                                              std::to_string(cand.relatedPort);
                                signalling.SendCandidate(from, candStr);
                            }
                            signalling.SendAnswer(from);
                            {
                                std::lock_guard<std::mutex> lock(peersMtx);
                                auto it = peers.find(from);
                                if (it != peers.end())
                                {
                                    it->second->negotiationReady = true;
                                }
                            }
                            ScheduleDelayedICE(from, 250);
                        });
                }
                else if (type == "candidate")
                {
                    Candidate cand;
                    if (!TryParseCandidateMessage(data, cand))
                    {
                        std::cerr << "[ICE] Ignoring malformed candidate from "
                                  << from << ": " << data << std::endl;
                        return;
                    }

                    auto parsedAgent = GetOrCreateAgent(from);
                    parsedAgent->AddRemoteCandidate(cand);
                    RegisterPeerAddress(cand.ip, cand.port, parsedAgent);
                    bool canStart = false;
                    {
                        std::lock_guard<std::mutex> lock(peersMtx);
                        auto it = peers.find(from);
                        canStart = (it != peers.end() &&
                                    it->second->negotiationReady.load());
                    }
                    if (canStart)
                    {
                        TryStartICE(from);
                    }
                    return;
#if 0

                if (false) {
                    char ip[64] = {}; 
                    int port = 0;
                    int scanned = sscanf_s(data.c_str(), "host %63[^:]:%d", ip, (unsigned)sizeof(ip), &port);
                    if (scanned == 2) {
                        cand.type = HOST; 
                        cand.ip = ip; 
                        cand.port = (uint16_t)port;
                    }
                } else if (data.find("srflx") == 0) {
                    char rip[64]={}, lip[64]={}; 
                    int rport=0, lport=0;
                    int scanned = sscanf_s(data.c_str(), "srflx %63[^:]:%d related %63[^:]:%d",
                            rip, (unsigned)sizeof(rip), &rport, lip, (unsigned)sizeof(lip), &lport);
                    if (scanned == 4) {
                        cand.type = SERVER_REFLEXIVE;
                        cand.ip = rip; 
                        cand.port = (uint16_t)rport;
                        cand.relatedIp = lip; 
                        cand.relatedPort = (uint16_t)lport;
                    }
                }
                
                
                auto agent = GetOrCreateAgent(from);
                agent->AddRemoteCandidate(cand);
                RegisterPeerAddress(cand.ip, cand.port, agent);
                TryStartICE(from);
#endif
                }
                else if (type == "answer")
                {
                    auto agent = GetOrCreateAgent(from);

                    {
                        std::lock_guard<std::mutex> lock(peersMtx);
                        auto it = peers.find(from);
                        if (it != peers.end() &&
                            it->second->reconnecting.load())
                        {
                            it->second->reconnecting = false;
                        }
                        if (it != peers.end())
                        {
                            it->second->negotiationReady = true;
                        }
                    }

                    for (const auto &cand : agent->GetLocalCandidates())
                    {
                        std::string candStr =
                            (cand.type == HOST)
                                ? "host " + cand.ip + ":" +
                                      std::to_string(cand.port)
                                : "srflx " + cand.ip + ":" +
                                      std::to_string(cand.port) + " related " +
                                      cand.relatedIp + ":" +
                                      std::to_string(cand.relatedPort);
                        signalling.SendCandidate(from, candStr);
                    }

                    std::cout << "[ICE] Got answer from " << from
                              << ", scheduling ICE start in 500ms...\n";
                    ScheduleDelayedICE(from, 500);
                }
                else if (type == "list")
                {
                    std::cout << "[System] Online users: " << data << std::endl;
                }
                else if (type == "server_chat")
                {
                    std::cout << "[Server Relay from " << from << "]: " << data
                              << std::endl;
                }
            });
    }

    void ExecuteCommand(const std::string &line)
    {
        if (!started.load())
        {
            return;
        }

        if (line == "register")
        {
            if (id.empty())
            {
                std::cout << "[Usage] register <id>" << std::endl;
                return;
            }

            if (signalling.Register(id))
            {
                std::cout << "[System] REGISTER sent as " << id << std::endl;
            }
            else
            {
                std::cout
                    << "[Error] Failed to send REGISTER (socket unavailable?)"
                    << std::endl;
            }
        }
        else if (line.find("register ") == 0)
        {
            std::string newId = line.substr(9);
            if (newId.empty() || newId.find(' ') != std::string::npos)
            {
                std::cout << "[Usage] register <id>" << std::endl;
                return;
            }

            if (signalling.Register(newId))
            {
                id = newId;
                SetConsoleTitleA(("P2P Chat - ID: " + id).c_str());
                std::cout << "[System] REGISTER sent as " << id << std::endl;
            }
            else
            {
                std::cout
                    << "[Error] Failed to send REGISTER (socket unavailable?)"
                    << std::endl;
            }
        }
        else if (line.find("stream on ") == 0)
        {
            std::string target = line.substr(10);
            if (target.empty())
            {
                std::cout << "[Usage] stream on <id|*>" << std::endl;
                return;
            }

            if (!StartScreenShare(target))
            {
                std::cout << "[Error] Failed to start screen sharing"
                          << std::endl;
            }
        }
        else if (line == "stream off")
        {
            StopScreenShare();
        }
        else if (line.find("stream fps ") == 0)
        {
            try
            {
                int fpsValue = std::stoi(line.substr(11));
                SetScreenShareFps(fpsValue);
                std::cout << "[Screen] FPS set to " << GetScreenShareFps()
                          << std::endl;
            }
            catch (...)
            {
                std::cout << "[Usage] stream fps <1-60>" << std::endl;
            }
        }
        else if (line.find("stream quality ") == 0)
        {
            try
            {
                int qualityValue = std::stoi(line.substr(15));
                SetScreenShareQuality(qualityValue);
                std::cout << "[Screen] Quality set to "
                          << GetScreenShareQuality() << std::endl;
            }
            catch (...)
            {
                std::cout << "[Usage] stream quality <20-95>" << std::endl;
            }
        }
        else if (line.find("stream scale ") == 0)
        {
            try
            {
                int scaleValue = std::stoi(line.substr(13));
                SetScreenShareScalePercent(scaleValue);
                std::cout << "[Screen] Scale set to "
                          << GetScreenShareScalePercent() << "%" << std::endl;
            }
            catch (...)
            {
                std::cout << "[Usage] stream scale <20-100>" << std::endl;
            }
        }
        else if (line.find("offer ") == 0)
        {
            std::string target = line.substr(6);
            auto agent = GetOrCreateAgent(target);

            {
                std::lock_guard<std::mutex> lock(peersMtx);
                auto it = peers.find(target);
                if (it != peers.end())
                {
                    it->second->iceStarted = false;
                    it->second->negotiationReady = false;
                }
            }
            CancelPendingICE(target);

            gatherer.StartGathering(
                "stun.l.google.com", 19302,
                [this, target, agent](std::vector<Candidate> locals)
                {
                    {
                        std::lock_guard<std::mutex> lock(localCandidatesMtx);
                        myLocalCandidates = locals;
                    }
                    for (auto &c : locals)
                        agent->AddLocalCandidate(c);
                    for (const auto &cand : agent->GetLocalCandidates())
                    {
                        std::string s =
                            (cand.type == HOST)
                                ? "host " + cand.ip + ":" +
                                      std::to_string(cand.port)
                                : "srflx " + cand.ip + ":" +
                                      std::to_string(cand.port) + " related " +
                                      cand.relatedIp + ":" +
                                      std::to_string(cand.relatedPort);
                        signalling.SendCandidate(target, s);
                    }
                    signalling.SendOffer(target);
                });
        }
        else if (line == "list")
        {
            signalling.RequestUserList();
        }
        else if (line.find("reconnect ") == 0)
        {
            std::string target = line.substr(10);
            std::cout << "[System] Forcing reconnect to " << target << "...\n";
            ScheduleReconnect(target);
        }
        else if (line.find("volume ") == 0)
        {
            size_t space2 = line.find(' ', 7);
            if (space2 == std::string::npos)
            {
                std::cout << "[Usage] volume <id> <0-200>" << std::endl;
                return;
            }

            std::string target = line.substr(7, space2 - 7);
            std::string valueText = line.substr(space2 + 1);
            if (target.empty() || valueText.empty())
            {
                std::cout << "[Usage] volume <id> <0-200>" << std::endl;
                return;
            }

            try
            {
                const float percent = std::stof(valueText);
                SetPeerVolume(target, percent / 100.0f);
            }
            catch (...)
            {
                std::cout << "[Usage] volume <id> <0-200>" << std::endl;
            }
        }
        else if (line.find("voice on ") == 0)
        {
            std::string target = line.substr(9);
            std::shared_ptr<IceAgent> targetAgent = GetExistingAgent(target);

            if (targetAgent)
            {
                std::cout << "[System] Microphone ON for " << target
                          << std::endl;
                audio.StartRecording(
                    [this, target](void *data, int len)
                    {
                        auto state = GetExistingPeerState(target);
                        auto currentAgent = GetExistingAgent(target);
                        if (!currentAgent)
                        {
                            if (state)
                            {
                                auto dropIndex =
                                    state->audioTxDrops.fetch_add(1) + 1;
                                if (dropIndex <= 5 || dropIndex % 50 == 0)
                                {
                                    std::cout << "[Audio] TX to " << target
                                              << " waiting for agent, drop #"
                                              << dropIndex << " len=" << len
                                              << std::endl;
                                }
                            }
                            return;
                        }

                        SOCKET outSock = INVALID_SOCKET;
                        sockaddr_in addr{};
                        if (!TryGetActiveSocket(currentAgent, outSock, addr))
                        {
                            if (state)
                            {
                                auto dropIndex =
                                    state->audioTxDrops.fetch_add(1) + 1;
                                if (dropIndex <= 5 || dropIndex % 50 == 0)
                                {
                                    std::cout
                                        << "[Audio] TX to " << target
                                        << " waiting for active path, drop #"
                                        << dropIndex << " len=" << len
                                        << std::endl;
                                }
                            }
                            return;
                        }

                        std::vector<char> packet;
                        packet.reserve(len + 1);
                        packet.push_back(PACKET_AUDIO);
                        packet.insert(packet.end(), (char *)data,
                                      (char *)data + len);

                        int sent =
                            sendto(outSock, packet.data(), (int)packet.size(),
                                   0, (sockaddr *)&addr, sizeof(addr));
                        if (sent == SOCKET_ERROR)
                        {
                            if (state)
                            {
                                auto errorIndex =
                                    state->audioTxErrors.fetch_add(1) + 1;
                                std::cerr << "[Audio] TX to " << target
                                          << " failed #" << errorIndex << ": "
                                          << WSAGetLastError() << std::endl;
                            }
                            return;
                        }

                        if (state)
                        {
                            auto packetIndex =
                                state->audioTxPackets.fetch_add(1) + 1;
                            if (packetIndex <= 5 || packetIndex % 50 == 0)
                            {
                                std::cout << "[Audio] TX to " << target
                                          << " packet #" << packetIndex
                                          << " len=" << len << std::endl;
                            }
                        }
                    });
            }
            else
            {
                std::cout << "[Error] No agent found for " << target
                          << std::endl;
            }
        }
        else if (line == "voice off")
        {
            audio.StopRecording();
            std::cout << "[System] Microphone OFF." << std::endl;
        }
        else if (line == "brdvoice on")
        {
            std::cout << "[System] Global voice ON." << std::endl;
            audio.StartRecording(
                [this](void *data, int len)
                {
                    std::vector<char> packet;
                    packet.push_back(PACKET_AUDIO);
                    packet.insert(packet.end(), (char *)data,
                                  (char *)data + len);

                    std::vector<std::shared_ptr<IceAgent>> agentsCopy;
                    {
                        std::lock_guard<std::mutex> lock(peersMtx);
                        for (auto const &[id, state] : peers)
                        {
                            if (state && state->agent)
                            {
                                agentsCopy.push_back(state->agent);
                            }
                        }
                    }

                    for (auto &agent : agentsCopy)
                    {
                        auto [isConnected, sock, remote] =
                            agent->GetActiveConnection();
                        if (isConnected && sock != INVALID_SOCKET)
                        {
                            SOCKET outSock;
                            sockaddr_in addr{};
                            if (TryGetActiveSocket(agent, outSock, addr))
                            {
                                sendto(outSock, packet.data(),
                                       (int)packet.size(), 0, (sockaddr *)&addr,
                                       sizeof(addr));
                            }
                        }
                    }
                });
        }
        else if (line.compare(0, 4, "brd ") == 0)
        {
            std::string text = line.substr(4);
            BroadcastMessage(text);
        }
        else if (line.find("msg ") == 0)
        {
            size_t space2 = line.find(' ', 4);
            if (space2 != std::string::npos)
            {
                std::string target = line.substr(4, space2 - 4);
                std::string text = line.substr(space2 + 1);

                std::shared_ptr<IceAgent> agent = nullptr;
                {
                    std::lock_guard<std::mutex> lock(peersMtx);
                    auto it = peers.find(target);
                    if (it != peers.end() && it->second->agent)
                    {
                        agent = it->second->agent;
                    }
                }

                if (!agent)
                {
                    std::cout << "[Error] No session found for " << target
                              << std::endl;
                    return;
                }

                auto [isConnected, sock, remote] = agent->GetActiveConnection();
                if (!isConnected || sock == INVALID_SOCKET)
                {
                    std::cout
                        << "[System] Still establishing ICE connection to "
                        << target << "..." << std::endl;
                    return;
                }

                std::string encrypted = XorKey(text);
                std::vector<char> buffer;
                buffer.push_back(PACKET_TEXT);
                buffer.insert(buffer.end(), encrypted.begin(), encrypted.end());

                SOCKET outSock;
                sockaddr_in addr{};
                if (TryGetActiveSocket(agent, outSock, addr))
                {
                    int sent =
                        sendto(outSock, buffer.data(), (int)buffer.size(), 0,
                               (sockaddr *)&addr, sizeof(addr));
                    if (sent != SOCKET_ERROR)
                    {
                        std::cout << "[Sent to " << target << "]" << std::endl;
                    }
                    else
                    {
                        std::cerr
                            << "[Error] sendto failed: " << WSAGetLastError()
                            << std::endl;
                    }
                }
            }
        }
        else if (line.find("smsg ") == 0)
        {
            size_t space2 = line.find(' ', 5);
            if (space2 != std::string::npos)
            {
                std::string target = line.substr(5, space2 - 5);
                std::string text = line.substr(space2 + 1);

                signalling.SendRelayMessage(target, text);
                std::cout << "[Server Relay] Sent to " << target << std::endl;
            }
        }
    }

    void Stop()
    {
        if (!started.exchange(false))
        {
            return;
        }

        running = false;
        StopScreenShare();
        audio.StopRecording();
        signalling.Disconnect();

        JoinScheduledTasks();

        if (worker.joinable())
            worker.join();
        if (pingWorker.joinable())
            pingWorker.join();

        {
            std::lock_guard<std::mutex> lock(peersMtx);
            peers.clear();
        }

        {
            std::lock_guard<std::mutex> lock(addrMapMtx);
            addrToAgent.clear();
        }

        {
            std::lock_guard<std::mutex> lock(screenRxMtx);
            screenRxBuffers.clear();
            remoteScreenPreviews.clear();
        }

        if (cp)
        {
            CloseHandle(cp);
            cp = nullptr;
        }

        WSACleanup();
    }

    void RunConsole(const char *serverIp, int serverPort)
    {
        Start(serverIp, serverPort);
        PrintHelp();

        std::string line;
        while (std::getline(std::cin, line))
        {
            ExecuteCommand(line);
        }

        Stop();
    }
};

#ifndef NEKOCHAT_NO_MAIN
int main()
{
    std::cout << "Enter your ID: ";
    std::string myId;
    std::getline(std::cin, myId);

    if (myId.empty())
    {
        std::cerr << "ID cannot be empty!" << std::endl;
        return 1;
    }

    P2PClient client;
    client.id = myId;
    client.RunConsole("SERVER IP!!!!!!!!!!!!!!!!!!!!!!!!!!!!!", 27015);
    return 0;
}
#endif
