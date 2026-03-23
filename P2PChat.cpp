#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <map>
#include <unordered_set>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <utility>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <immintrin.h>
#define NEKOCHAT_TEXT_SSE2 1
#endif

#include "STUN.hpp"
#include "CandidateGatherer.hpp"
#include "IceAgent.hpp"
#include "SignallingClient.hpp"
#include "AudioEngine.hpp"
#include "ScreenShareEngine.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

constexpr uint8_t PACKET_TEXT = 0x01;
constexpr uint8_t PACKET_AUDIO = 0x02;
constexpr uint8_t PACKET_SCREEN = 0x03;
constexpr uint8_t PACKET_FILE = 0x04;
constexpr size_t AUDIO_FRAME_BYTES = 960;
constexpr size_t AUDIO_PACKET_BYTES = AUDIO_FRAME_BYTES + 1;
constexpr size_t SCREEN_CHUNK_PAYLOAD = 1100;
constexpr size_t MAX_SCREEN_CHUNKS = 512;
constexpr size_t FILE_CHUNK_PAYLOAD = 1024;
constexpr uint8_t FILE_PACKET_META = 0x01;
constexpr uint8_t FILE_PACKET_DATA = 0x02;
constexpr uint8_t FILE_PACKET_ACK = 0x03;
constexpr uint8_t FILE_PACKET_REJECT = 0x04;
constexpr uint32_t FILE_META_CHUNK_INDEX = 0xFFFFFFFFu;
constexpr uint64_t MAX_FILE_TRANSFER_BYTES = 128ull * 1024ull * 1024ull;
constexpr size_t MAX_FILE_NAME_BYTES = 240;

static uint64_t HostToNetwork64(uint64_t value) {
    const uint32_t hi = htonl(static_cast<uint32_t>(value >> 32));
    const uint32_t lo = htonl(static_cast<uint32_t>(value & 0xFFFFFFFFull));
    return (static_cast<uint64_t>(lo) << 32) | hi;
}

static uint64_t NetworkToHost64(uint64_t value) {
    const uint32_t hi = ntohl(static_cast<uint32_t>(value & 0xFFFFFFFFull));
    const uint32_t lo = ntohl(static_cast<uint32_t>(value >> 32));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

static void ApplyXor55(char* data, size_t len) {
    if (!data || len == 0) {
        return;
    }

#ifdef NEKOCHAT_TEXT_SSE2
    const __m128i key = _mm_set1_epi8(0x55);
    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        const __m128i block = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(data + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(data + i),
                         _mm_xor_si128(block, key));
    }
    for (; i < len; ++i) {
        data[i] ^= 0x55;
    }
#else
    for (size_t i = 0; i < len; ++i) {
        data[i] ^= 0x55;
    }
#endif
}

#pragma pack(push, 1)
struct ScreenChunkHeader {
    uint16_t frameId;
    uint16_t chunkIndex;
    uint16_t chunkCount;
    uint32_t totalSize;
};

struct FileMetaHeader {
    uint8_t kind;
    uint32_t transferId;
    uint32_t chunkCount;
    uint64_t totalSize;
    uint16_t fileNameLen;
};

struct FileChunkHeader {
    uint8_t kind;
    uint32_t transferId;
    uint32_t chunkIndex;
};

struct FileAckHeader {
    uint8_t kind;
    uint32_t transferId;
    uint32_t chunkIndex;
};

struct FileRejectHeader {
    uint8_t kind;
    uint32_t transferId;
};
#pragma pack(pop)

struct ScreenRxAssembly {
    uint16_t frameId = 0;
    uint16_t chunkCount = 0;
    uint32_t totalSize = 0;
    std::vector<std::vector<uint8_t>> chunks;
    std::vector<uint8_t> received;
    size_t receivedCount = 0;
    size_t receivedBytes = 0;
    std::chrono::steady_clock::time_point lastUpdate = std::chrono::steady_clock::now();
};

struct IncomingFileTransfer {
    std::string peerId;
    std::string fileName;
    std::filesystem::path tempPath;
    std::filesystem::path finalPath;
    std::ofstream stream;
    uint32_t chunkCount = 0;
    uint32_t nextChunkIndex = 0;
    uint64_t totalSize = 0;
    uint64_t writtenBytes = 0;
    std::chrono::steady_clock::time_point lastUpdate = std::chrono::steady_clock::now();
};

struct CompletedIncomingFileTransfer {
    uint32_t chunkCount = 0;
    std::chrono::steady_clock::time_point completedAt = std::chrono::steady_clock::now();
};

struct IncomingOfferPrompt {
    std::string peerId;
    std::chrono::steady_clock::time_point createdAt = std::chrono::steady_clock::now();
};

struct PendingIncomingFilePrompt {
    std::string peerId;
    uint32_t transferId = 0;
    std::string fileName;
    uint32_t chunkCount = 0;
    uint64_t totalSize = 0;
    std::chrono::steady_clock::time_point createdAt = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastUpdate = std::chrono::steady_clock::now();
};

enum class FilePacketWaitResult {
    Acked,
    Rejected,
    Timeout,
    Stopped
};

struct PeerState {
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

struct ScheduledTask {
    std::thread worker;
    std::shared_ptr<std::atomic<bool>> cancelled;
};

struct P2PClient {
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
    std::mutex onlineUsersMtx;
    std::vector<std::string> onlineUsers;
    
    std::vector<Candidate> myLocalCandidates;
    std::mutex localCandidatesMtx;

    std::mutex screenShareMtx;
    std::string screenShareTarget = "*";
    std::atomic<uint16_t> nextScreenFrameId{1};
    std::mutex offerPromptMtx;
    std::vector<IncomingOfferPrompt> pendingOfferPrompts;
    std::unordered_set<std::string> pendingOfferPeers;
    std::atomic<bool> incomingOfferApprovalRequired{false};
    std::mutex screenRxMtx;
    std::unordered_map<std::string, ScreenRxAssembly> screenRxBuffers;
    std::unordered_map<std::string, ScreenShareEngine::PreviewFrame> remoteScreenPreviews;
    std::atomic<uint64_t> remoteScreenPreviewSeq{1};
    std::mutex fileTransferMtx;
    std::condition_variable fileTransferCv;
    std::unordered_map<std::string, IncomingFileTransfer> incomingFileTransfers;
    std::unordered_map<std::string, CompletedIncomingFileTransfer> completedIncomingFileTransfers;
    std::unordered_map<std::string, PendingIncomingFilePrompt> pendingIncomingFilePrompts;
    std::vector<PendingIncomingFilePrompt> pendingIncomingFilePromptQueue;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> rejectedIncomingFileTransfers;
    std::unordered_set<uint64_t> fileAcknowledgements;
    std::unordered_set<uint32_t> fileRejectedTransferIds;
    std::atomic<uint32_t> nextFileTransferId{1};
    std::atomic<bool> incomingFileApprovalRequired{false};
    std::mutex fileSendTasksMtx;
    std::vector<std::future<void>> fileSendTasks;

    HANDLE cp = nullptr;
    std::thread worker;
    std::thread pingWorker;
    std::atomic<bool> started{false};
    
    std::map<std::string, ScheduledTask> pendingTasks;
    std::mutex pendingICEMtx;

    
    std::shared_ptr<IceAgent> GetExistingAgent(const std::string& peerId) {
        std::lock_guard<std::mutex> lock(peersMtx);
        auto it = peers.find(peerId);
        if (it != peers.end() && it->second && it->second->agent) {
            return it->second->agent;
        }
        return nullptr;
    }

    std::shared_ptr<PeerState> GetExistingPeerState(const std::string& peerId) {
        std::lock_guard<std::mutex> lock(peersMtx);
        auto it = peers.find(peerId);
        if (it != peers.end()) {
            return it->second;
        }
        return nullptr;
    }

    std::shared_ptr<IceAgent> GetOrCreateAgent(const std::string& peerId) {
        
        {
            std::lock_guard<std::mutex> lock(peersMtx);
            auto it = peers.find(peerId);
            if (it != peers.end() && it->second && it->second->agent) {
                if (!it->second->reconnecting.load()) {
                    std::cout << "[Agent] Reusing existing agent for " << peerId << "\n";
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
        newAgent->onAudioReceived = [this, peerId](void* d, int l) {
            auto state = GetExistingPeerState(peerId);
            if (state) {
                auto packetIndex = state->audioRxPackets.fetch_add(1) + 1;
                if (packetIndex <= 5 || packetIndex % 50 == 0) {
                    std::cout << "[Audio] RX from " << peerId
                              << " packet #" << packetIndex
                              << " len=" << l << std::endl;
                }
            }
            audio.PlayFrameFromSource(peerId, d, l);
        };
        newAgent->onScreenChunkReceived = [this, peerId](const void* d, int l) {
            HandleIncomingScreenChunk(peerId, d, l);
        };
        newAgent->onFilePacketReceived = [this, peerId](const void* d, int l) {
            HandleIncomingFilePacket(peerId, d, l);
        };
        newAgent->onDisconnected = [this, peerId]() {
            std::cout << "[System] Connection lost to " << peerId << ", scheduling reconnect...\n";
            ScheduleReconnect(peerId);
        };

        for (const auto& local : localsCopy) {
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

    std::string IceTaskKey(const std::string& peerId) const {
        return "ice:" + peerId;
    }

    std::string ReconnectTaskKey(const std::string& peerId) const {
        return "reconnect:" + peerId;
    }

    void CancelScheduledTask(const std::string& taskKey) {
        std::thread worker;
        std::shared_ptr<std::atomic<bool>> cancelled;

        {
            std::lock_guard<std::mutex> lock(pendingICEMtx);
            auto it = pendingTasks.find(taskKey);
            if (it == pendingTasks.end()) {
                return;
            }

            cancelled = it->second.cancelled;
            if (cancelled) {
                cancelled->store(true, std::memory_order_release);
            }

            if (it->second.worker.joinable()) {
                if (it->second.worker.get_id() == std::this_thread::get_id()) {
                    it->second.worker.detach();
                } else {
                    worker = std::move(it->second.worker);
                }
            }
            pendingTasks.erase(it);
        }

        if (worker.joinable()) {
            worker.join();
        }
    }

    template <typename Action>
    void ScheduleTask(const std::string& taskKey, int delayMs, Action&& action) {
        CancelScheduledTask(taskKey);

        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        ScheduledTask task;
        task.cancelled = cancelled;
        task.worker = std::thread([this, delayMs, cancelled, action = std::forward<Action>(action)]() mutable {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
            while (running.load() &&
                   !cancelled->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            if (!running.load() || cancelled->load(std::memory_order_acquire)) {
                return;
            }

            action();
        });

        std::lock_guard<std::mutex> lock(pendingICEMtx);
        pendingTasks[taskKey] = std::move(task);
    }

    void JoinScheduledTasks() {
        std::vector<std::thread> workers;

        {
            std::lock_guard<std::mutex> lock(pendingICEMtx);
            for (auto& [taskKey, task] : pendingTasks) {
                if (task.cancelled) {
                    task.cancelled->store(true, std::memory_order_release);
                }
                if (task.worker.joinable()) {
                    workers.push_back(std::move(task.worker));
                }
            }
            pendingTasks.clear();
        }

        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void CancelPendingICE(const std::string& peerId) {
        CancelScheduledTask(IceTaskKey(peerId));
    }

    void ScheduleDelayedICE(const std::string& peerId, int delayMs) {
        ScheduleTask(IceTaskKey(peerId), delayMs, [this, peerId, delayMs]() {
            std::cout << "[ICE] Delayed start for " << peerId << " after " << delayMs << "ms\n";
            TryStartICE(peerId, true);
        });
    }

    void ScheduleReconnect(const std::string& peerId) {
        {
            std::lock_guard<std::mutex> lock(peersMtx);
            auto it = peers.find(peerId);
            if (it != peers.end()) {
                it->second->reconnecting = true;
                it->second->iceStarted = false;
                it->second->negotiationReady = false;
            }
        }

        CancelPendingICE(peerId);
        CancelScheduledTask(ReconnectTaskKey(peerId));

        ScheduleTask(ReconnectTaskKey(peerId), 2000, [this, peerId]() {
            std::cout << "[System] Attempting reconnect to " << peerId << "...\n";

            {
                std::lock_guard<std::mutex> lock(peersMtx);
                peers.erase(peerId);
            }

            signalling.SendOffer(peerId);
        });
    }

    void TryStartICE(const std::string& peerId, bool force = false) {
        std::shared_ptr<PeerState> state;
        {
            std::lock_guard<std::mutex> lock(peersMtx);
            auto it = peers.find(peerId);
            if (it != peers.end()) {
                state = it->second;
            }
        }
        
        if (!state) {
            std::cout << "[ICE] No state for " << peerId << ", cannot start\n";
            return;
        }
        
        if (state->reconnecting.load()) {
            std::cout << "[ICE] Waiting for reconnect to " << peerId << "...\n";
            return;
        }

        if (!force && state->iceStarted.load()) {
            std::cout << "[ICE] Already started for " << peerId << ", skipping\n";
            return;
        }

        auto [success, sock, remote] = state->agent->GetActiveConnection();
        if (!success) {
            bool started = state->agent->StartConnectivityChecks([this, peerId](bool success, const CandidatePair& pair) {
                if (success) {
                    std::cout << "\n[" << id << "] ICE Success with " << peerId 
                              << "! Path: " << pair.local.ip << " -> " << pair.remote.ip << std::endl;
                    
                    std::lock_guard<std::mutex> lock(peersMtx);
                    auto it = peers.find(peerId);
                    if (it != peers.end()) {
                        it->second->reconnecting = false;
                    }
                } else {
                    std::cout << "\n[" << id << "] ICE failed with " << peerId << std::endl;
                    std::lock_guard<std::mutex> lock(peersMtx);
                    auto it = peers.find(peerId);
                    if (it != peers.end()) {
                        it->second->iceStarted = false;
                    }
                }
            });
            
            if (started) {
                state->iceStarted = true;
                CancelPendingICE(peerId);
                std::cout << "[ICE] Started checks for " << peerId << std::endl;
            } else {
                std::cout << "[ICE] Cannot start checks for " << peerId << " yet (no remote candidates?)\n";
            }
        } else {
            std::cout << "[ICE] Already established with " << peerId << ", skipping new checks.\n";
        }
    }

    void InitMyCandidates() {
        gatherer.StartGathering("stun.l.google.com", 19302, [this](std::vector<Candidate> locals) {
            std::lock_guard<std::mutex> lock(localCandidatesMtx);
            myLocalCandidates = locals;
            std::cout << "[System] My local candidates ready. Now you can accept offers.\n";
        });
    }

    bool TryGetActiveSocket(const std::shared_ptr<IceAgent>& agent, 
                           SOCKET& outSock, sockaddr_in& outAddr) {
        if (!agent) return false;
        
        auto [success, sock, remote] = agent->GetActiveConnection();
        
        if (!success || sock == INVALID_SOCKET) {
            return false;
        }
        
        outSock = sock;
        outAddr.sin_family = AF_INET;
        inet_pton(AF_INET, remote.ip.c_str(), &outAddr.sin_addr);
        outAddr.sin_port = htons(remote.port);
        
        return true;
    }

    static std::string TrimEnclosingQuotes(std::string value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }

    static std::string TrimSpaces(std::string value) {
        while (!value.empty() &&
               std::isspace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }
        while (!value.empty() &&
               std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        return value;
    }

    static std::vector<std::string> ParseUserListPayload(const std::string& payload) {
        std::vector<std::string> users;
        std::unordered_set<std::string> dedupe;

        size_t cursor = 0;
        while (cursor <= payload.size()) {
            const size_t comma = payload.find(',', cursor);
            const size_t tokenEnd =
                (comma == std::string::npos) ? payload.size() : comma;
            std::string token = TrimSpaces(payload.substr(cursor, tokenEnd - cursor));
            if (!token.empty() && dedupe.insert(token).second) {
                users.push_back(std::move(token));
            }

            if (comma == std::string::npos) {
                break;
            }
            cursor = comma + 1;
        }

        std::sort(users.begin(), users.end());
        return users;
    }

    static std::string JoinUserList(const std::vector<std::string>& users) {
        if (users.empty()) {
            return "(none)";
        }

        std::string joined;
        for (size_t i = 0; i < users.size(); ++i) {
            if (i != 0) {
                joined += ", ";
            }
            joined += users[i];
        }
        return joined;
    }

    static std::string SanitizeFileName(const std::string& originalName) {
        std::string fileName = std::filesystem::path(originalName).filename().string();
        if (fileName.empty()) {
            fileName = "file.bin";
        }

        std::string sanitized;
        sanitized.reserve(fileName.size());
        for (char ch : fileName) {
            const unsigned char uch = static_cast<unsigned char>(ch);
            if (std::isalnum(uch) || ch == '.' || ch == '_' || ch == '-' || ch == ' ') {
                sanitized.push_back(ch);
            } else {
                sanitized.push_back('_');
            }
        }

        while (!sanitized.empty() &&
               (sanitized.back() == ' ' || sanitized.back() == '.')) {
            sanitized.pop_back();
        }

        if (sanitized.empty()) {
            sanitized = "file.bin";
        }

        if (sanitized.size() > MAX_FILE_NAME_BYTES) {
            const auto ext = std::filesystem::path(sanitized).extension().string();
            const auto stem = std::filesystem::path(sanitized).stem().string();
            const size_t maxStem =
                (ext.size() >= MAX_FILE_NAME_BYTES) ? 0 : MAX_FILE_NAME_BYTES - ext.size();
            sanitized = stem.substr(0, maxStem) + ext;
            if (sanitized.empty()) {
                sanitized = "file.bin";
            }
        }
        return sanitized;
    }

    static std::string MakeIncomingFileKey(const std::string& peerId, uint32_t transferId) {
        return peerId + "#" + std::to_string(transferId);
    }

    static uint64_t MakeFileAckKey(uint32_t transferId, uint32_t chunkIndex) {
        return (static_cast<uint64_t>(transferId) << 32) |
               static_cast<uint64_t>(chunkIndex);
    }

    std::filesystem::path GetReceivedFilesRoot() const {
        const std::string localId = id.empty() ? "session" : id;
        return std::filesystem::current_path() / "ReceivedFiles" / localId;
    }

    std::filesystem::path MakeUniquePath(const std::filesystem::path& dir,
                                         const std::string& fileName) const {
        std::error_code ec;
        std::filesystem::path candidate = dir / fileName;
        if (!std::filesystem::exists(candidate, ec)) {
            return candidate;
        }

        const std::string stem = candidate.stem().string();
        const std::string ext = candidate.extension().string();
        for (int index = 1; index < 10000; ++index) {
            const auto numbered =
                dir / (stem + " (" + std::to_string(index) + ")" + ext);
            if (!std::filesystem::exists(numbered, ec)) {
                return numbered;
            }
        }

        return dir / (stem + "_" + std::to_string(GetTickCount64()) + ext);
    }

    void QueueIncomingOfferPrompt(const std::string& peerId) {
        std::lock_guard<std::mutex> lock(offerPromptMtx);
        if (!pendingOfferPeers.insert(peerId).second) {
            return;
        }

        pendingOfferPrompts.push_back(IncomingOfferPrompt{peerId, std::chrono::steady_clock::now()});
        std::cout << "[Offer] Incoming offer from " << peerId
                  << ". Waiting for confirmation in the UI." << std::endl;
    }

    bool PopPendingOfferPrompt(IncomingOfferPrompt& out) {
        std::lock_guard<std::mutex> lock(offerPromptMtx);
        if (pendingOfferPrompts.empty()) {
            return false;
        }

        out = pendingOfferPrompts.front();
        pendingOfferPrompts.erase(pendingOfferPrompts.begin());
        return true;
    }

    void ProcessIncomingOffer(const std::string& from) {
        std::cout << "\n[" << id << "] Offer from " << from << ". Gathering..." << std::endl;

        auto agent = GetOrCreateAgent(from);

        {
            std::lock_guard<std::mutex> lock(peersMtx);
            auto it = peers.find(from);
            if (it != peers.end() && it->second->reconnecting.load()) {
                std::cout << "[System] Already reconnecting to " << from << ", ignoring offer\n";
                return;
            }
        }

        gatherer.StartGathering("stun.l.google.com", 19302, [this, from, agent](std::vector<Candidate> locals) {
            {
                std::lock_guard<std::mutex> lock(localCandidatesMtx);
                myLocalCandidates = locals;
            }
            for (auto& c : locals) agent->AddLocalCandidate(c);

            for (const auto& cand : agent->GetLocalCandidates()) {
                std::string candStr = (cand.type == HOST) ?
                    "host " + cand.ip + ":" + std::to_string(cand.port) :
                    "srflx " + cand.ip + ":" + std::to_string(cand.port) + " related " + cand.relatedIp + ":" + std::to_string(cand.relatedPort);
                signalling.SendCandidate(from, candStr);
            }
            signalling.SendAnswer(from);
            {
                std::lock_guard<std::mutex> lock(peersMtx);
                auto it = peers.find(from);
                if (it != peers.end()) {
                    it->second->negotiationReady = true;
                }
            }
            ScheduleDelayedICE(from, 250);
        });
    }

    void SetIncomingOfferApprovalRequired(bool enabled) {
        incomingOfferApprovalRequired = enabled;
    }

    bool AcceptPendingOffer(const std::string& peerId) {
        {
            std::lock_guard<std::mutex> lock(offerPromptMtx);
            if (pendingOfferPeers.erase(peerId) == 0) {
                return false;
            }
        }

        ProcessIncomingOffer(peerId);
        return true;
    }

    void DeclinePendingOffer(const std::string& peerId) {
        bool removed = false;
        {
            std::lock_guard<std::mutex> lock(offerPromptMtx);
            removed = pendingOfferPeers.erase(peerId) > 0;
        }

        if (!removed) {
            return;
        }

        signalling.SendOfferDeclined(peerId);
        std::cout << "[Offer] Declined incoming offer from " << peerId << std::endl;
    }

    bool BeginIncomingFileTransferLocked(const std::string& transferKey,
                                         const PendingIncomingFilePrompt& prompt,
                                         std::chrono::steady_clock::time_point now) {
        const auto targetDir = GetReceivedFilesRoot() / SanitizeFileName(prompt.peerId);
        std::error_code dirEc;
        std::filesystem::create_directories(targetDir, dirEc);
        if (dirEc) {
            std::cout << "[File] Cannot create receive folder for "
                      << prompt.peerId << std::endl;
            return false;
        }

        IncomingFileTransfer transfer{};
        transfer.peerId = prompt.peerId;
        transfer.fileName = prompt.fileName;
        transfer.chunkCount = prompt.chunkCount;
        transfer.totalSize = prompt.totalSize;
        transfer.nextChunkIndex = 0;
        transfer.writtenBytes = 0;
        transfer.lastUpdate = now;
        transfer.finalPath = MakeUniquePath(targetDir, prompt.fileName);
        transfer.tempPath = transfer.finalPath;
        transfer.tempPath += ".part";
        transfer.stream.open(transfer.tempPath, std::ios::binary | std::ios::trunc);
        if (!transfer.stream) {
            std::cout << "[File] Cannot open temp file for "
                      << prompt.fileName << std::endl;
            return false;
        }

        incomingFileTransfers[transferKey] = std::move(transfer);
        std::cout << "[File] Receiving " << prompt.fileName
                  << " from " << prompt.peerId << " (" << prompt.totalSize
                  << " bytes, " << prompt.chunkCount
                  << " chunks)" << std::endl;
        return true;
    }

    void CleanupFileTransfersLocked(std::chrono::steady_clock::time_point now) {
        constexpr auto incomingTimeout = std::chrono::seconds(20);
        constexpr auto completedTtl = std::chrono::seconds(30);
        constexpr auto pendingPromptTtl = std::chrono::seconds(60);
        constexpr auto rejectedTtl = std::chrono::seconds(45);

        for (auto it = incomingFileTransfers.begin(); it != incomingFileTransfers.end();) {
            if (now - it->second.lastUpdate > incomingTimeout) {
                if (it->second.stream.is_open()) {
                    it->second.stream.close();
                }
                std::error_code ec;
                std::filesystem::remove(it->second.tempPath, ec);
                std::cout << "[File] Timed out while receiving "
                          << it->second.fileName << " from "
                          << it->second.peerId << std::endl;
                it = incomingFileTransfers.erase(it);
            } else {
                ++it;
            }
        }

        for (auto it = completedIncomingFileTransfers.begin();
             it != completedIncomingFileTransfers.end();) {
            if (now - it->second.completedAt > completedTtl) {
                it = completedIncomingFileTransfers.erase(it);
            } else {
                ++it;
            }
        }

        for (auto it = pendingIncomingFilePrompts.begin();
             it != pendingIncomingFilePrompts.end();) {
            if (now - it->second.lastUpdate > pendingPromptTtl) {
                std::cout << "[File] Request expired: " << it->second.fileName
                          << " from " << it->second.peerId << std::endl;
                it = pendingIncomingFilePrompts.erase(it);
            } else {
                ++it;
            }
        }

        pendingIncomingFilePromptQueue.erase(
            std::remove_if(
                pendingIncomingFilePromptQueue.begin(),
                pendingIncomingFilePromptQueue.end(),
                [this](const PendingIncomingFilePrompt& prompt) {
                    return pendingIncomingFilePrompts.find(
                               MakeIncomingFileKey(prompt.peerId, prompt.transferId)) ==
                           pendingIncomingFilePrompts.end();
                }),
            pendingIncomingFilePromptQueue.end());

        for (auto it = rejectedIncomingFileTransfers.begin();
             it != rejectedIncomingFileTransfers.end();) {
            if (now - it->second > rejectedTtl) {
                it = rejectedIncomingFileTransfers.erase(it);
            } else {
                ++it;
            }
        }
    }

    void CleanupFileTransfersOnStop() {
        std::lock_guard<std::mutex> lock(fileTransferMtx);
        for (auto& [key, transfer] : incomingFileTransfers) {
            if (transfer.stream.is_open()) {
                transfer.stream.close();
            }
            std::error_code ec;
            std::filesystem::remove(transfer.tempPath, ec);
        }
        incomingFileTransfers.clear();
        completedIncomingFileTransfers.clear();
        pendingIncomingFilePrompts.clear();
        pendingIncomingFilePromptQueue.clear();
        rejectedIncomingFileTransfers.clear();
        fileAcknowledgements.clear();
        fileRejectedTransferIds.clear();
    }

    void WaitForFileSendTasks() {
        std::vector<std::future<void>> tasks;
        {
            std::lock_guard<std::mutex> lock(fileSendTasksMtx);
            tasks.swap(fileSendTasks);
        }

        for (auto& task : tasks) {
            if (task.valid()) {
                try {
                    task.get();
                } catch (const std::exception& ex) {
                    std::cout << "[File] Sender worker failed: "
                              << ex.what() << std::endl;
                } catch (...) {
                    std::cout << "[File] Sender worker failed with an unknown error"
                              << std::endl;
                }
            }
        }
    }

    void QueueFileSend(const std::string& peerId, const std::filesystem::path& path) {
        std::lock_guard<std::mutex> lock(fileSendTasksMtx);
        fileSendTasks.emplace_back(std::async(
            std::launch::async, [this, peerId, path]() {
                SendFileWorker(peerId, path);
            }));
    }

    bool SendBufferToPeer(const std::string& peerId, const void* data, size_t len) {
        if (!data || len == 0) {
            return false;
        }

        auto agent = GetExistingAgent(peerId);
        SOCKET outSock = INVALID_SOCKET;
        sockaddr_in addr{};
        if (!TryGetActiveSocket(agent, outSock, addr)) {
            return false;
        }

        const int sent = sendto(outSock,
                                reinterpret_cast<const char*>(data),
                                static_cast<int>(len),
                                0,
                                reinterpret_cast<const sockaddr*>(&addr),
                                sizeof(addr));
        return sent != SOCKET_ERROR;
    }

    std::vector<uint8_t> BuildFileMetaPacket(uint32_t transferId, uint32_t chunkCount,
                                             uint64_t totalSize,
                                             const std::string& fileName) const {
        FileMetaHeader header{};
        header.kind = FILE_PACKET_META;
        header.transferId = htonl(transferId);
        header.chunkCount = htonl(chunkCount);
        header.totalSize = HostToNetwork64(totalSize);
        header.fileNameLen = htons(static_cast<uint16_t>(fileName.size()));

        std::vector<uint8_t> packet(1 + sizeof(FileMetaHeader) + fileName.size());
        packet[0] = PACKET_FILE;
        memcpy(packet.data() + 1, &header, sizeof(header));
        memcpy(packet.data() + 1 + sizeof(header), fileName.data(), fileName.size());
        return packet;
    }

    std::vector<uint8_t> BuildFileChunkPacket(uint32_t transferId, uint32_t chunkIndex,
                                              const uint8_t* payload,
                                              size_t payloadSize) const {
        FileChunkHeader header{};
        header.kind = FILE_PACKET_DATA;
        header.transferId = htonl(transferId);
        header.chunkIndex = htonl(chunkIndex);

        std::vector<uint8_t> packet(1 + sizeof(FileChunkHeader) + payloadSize);
        packet[0] = PACKET_FILE;
        memcpy(packet.data() + 1, &header, sizeof(header));
        if (payloadSize > 0 && payload) {
            memcpy(packet.data() + 1 + sizeof(header), payload, payloadSize);
        }
        return packet;
    }

    std::vector<uint8_t> BuildFileAckPacket(uint32_t transferId, uint32_t chunkIndex) const {
        FileAckHeader header{};
        header.kind = FILE_PACKET_ACK;
        header.transferId = htonl(transferId);
        header.chunkIndex = htonl(chunkIndex);

        std::vector<uint8_t> packet(1 + sizeof(FileAckHeader));
        packet[0] = PACKET_FILE;
        memcpy(packet.data() + 1, &header, sizeof(header));
        return packet;
    }

    std::vector<uint8_t> BuildFileRejectPacket(uint32_t transferId) const {
        FileRejectHeader header{};
        header.kind = FILE_PACKET_REJECT;
        header.transferId = htonl(transferId);

        std::vector<uint8_t> packet(1 + sizeof(FileRejectHeader));
        packet[0] = PACKET_FILE;
        memcpy(packet.data() + 1, &header, sizeof(header));
        return packet;
    }

    void HandleFileAckPacket(uint32_t transferId, uint32_t chunkIndex) {
        {
            std::lock_guard<std::mutex> lock(fileTransferMtx);
            fileAcknowledgements.insert(MakeFileAckKey(transferId, chunkIndex));
        }
        fileTransferCv.notify_all();
    }

    void HandleFileRejectPacket(uint32_t transferId) {
        {
            std::lock_guard<std::mutex> lock(fileTransferMtx);
            fileRejectedTransferIds.insert(transferId);
        }
        fileTransferCv.notify_all();
    }

    FilePacketWaitResult WaitForFileAck(uint32_t transferId, uint32_t chunkIndex, int timeoutMs) {
        const uint64_t ackKey = MakeFileAckKey(transferId, chunkIndex);
        std::unique_lock<std::mutex> lock(fileTransferMtx);
        const bool signaled = fileTransferCv.wait_for(
            lock, std::chrono::milliseconds(timeoutMs), [this, ackKey, transferId]() {
                return !running.load() ||
                       fileAcknowledgements.count(ackKey) > 0 ||
                       fileRejectedTransferIds.count(transferId) > 0;
            });

        if (!signaled) {
            fileAcknowledgements.erase(ackKey);
            return FilePacketWaitResult::Timeout;
        }
        if (!running.load()) {
            fileAcknowledgements.erase(ackKey);
            fileRejectedTransferIds.erase(transferId);
            return FilePacketWaitResult::Stopped;
        }
        if (fileRejectedTransferIds.erase(transferId) > 0) {
            fileAcknowledgements.erase(ackKey);
            return FilePacketWaitResult::Rejected;
        }

        fileAcknowledgements.erase(ackKey);
        return FilePacketWaitResult::Acked;
    }

    bool PopPendingFilePrompt(PendingIncomingFilePrompt& out) {
        std::lock_guard<std::mutex> lock(fileTransferMtx);
        CleanupFileTransfersLocked(std::chrono::steady_clock::now());
        if (pendingIncomingFilePromptQueue.empty()) {
            return false;
        }

        out = pendingIncomingFilePromptQueue.front();
        pendingIncomingFilePromptQueue.erase(pendingIncomingFilePromptQueue.begin());
        return true;
    }

    void SetIncomingFileApprovalRequired(bool enabled) {
        incomingFileApprovalRequired = enabled;
    }

    bool AcceptPendingFilePrompt(const std::string& peerId, uint32_t transferId) {
        std::vector<uint8_t> ackPacket;
        {
            std::lock_guard<std::mutex> lock(fileTransferMtx);
            const auto now = std::chrono::steady_clock::now();
            CleanupFileTransfersLocked(now);

            const std::string transferKey = MakeIncomingFileKey(peerId, transferId);
            auto pendingIt = pendingIncomingFilePrompts.find(transferKey);
            if (pendingIt == pendingIncomingFilePrompts.end()) {
                return false;
            }

            if (!BeginIncomingFileTransferLocked(transferKey, pendingIt->second, now)) {
                return false;
            }

            pendingIncomingFilePrompts.erase(pendingIt);
            ackPacket = BuildFileAckPacket(transferId, FILE_META_CHUNK_INDEX);
        }

        SendBufferToPeer(peerId, ackPacket.data(), ackPacket.size());
        std::cout << "[File] Accepted incoming file from " << peerId
                  << ": transfer #" << transferId << std::endl;
        return true;
    }

    bool DeclinePendingFilePrompt(const std::string& peerId, uint32_t transferId) {
        std::vector<uint8_t> rejectPacket;
        {
            std::lock_guard<std::mutex> lock(fileTransferMtx);
            const std::string transferKey = MakeIncomingFileKey(peerId, transferId);
            auto pendingIt = pendingIncomingFilePrompts.find(transferKey);
            if (pendingIt == pendingIncomingFilePrompts.end()) {
                return false;
            }

            rejectedIncomingFileTransfers[transferKey] = std::chrono::steady_clock::now();
            pendingIncomingFilePrompts.erase(pendingIt);
            rejectPacket = BuildFileRejectPacket(transferId);
        }

        SendBufferToPeer(peerId, rejectPacket.data(), rejectPacket.size());
        std::cout << "[File] Declined incoming file from " << peerId
                  << ": transfer #" << transferId << std::endl;
        return true;
    }

    FilePacketWaitResult SendPacketWithAck(const std::string& peerId,
                                           const std::vector<uint8_t>& packet,
                                           uint32_t transferId,
                                           uint32_t chunkIndex,
                                           int maxRetries,
                                           int timeoutMs) {
        for (int attempt = 1; attempt <= maxRetries && running.load(); ++attempt) {
            if (!SendBufferToPeer(peerId, packet.data(), packet.size())) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                continue;
            }

            const auto result = WaitForFileAck(transferId, chunkIndex, timeoutMs);
            if (result == FilePacketWaitResult::Acked ||
                result == FilePacketWaitResult::Rejected ||
                result == FilePacketWaitResult::Stopped) {
                return result;
            }
        }
        return FilePacketWaitResult::Timeout;
    }

    void SendFileWorker(const std::string& peerId, std::filesystem::path path) {
        try {
            path = std::filesystem::absolute(path);
        } catch (const std::exception& ex) {
            std::cout << "[File] Invalid path: " << ex.what() << std::endl;
            return;
        }

        std::error_code ec;
        if (!std::filesystem::exists(path, ec) ||
            !std::filesystem::is_regular_file(path, ec)) {
            std::cout << "[File] File not found: " << path.string() << std::endl;
            return;
        }

        const uint64_t fileSize = std::filesystem::file_size(path, ec);
        if (ec) {
            std::cout << "[File] Unable to read size: " << path.string() << std::endl;
            return;
        }
        if (fileSize > MAX_FILE_TRANSFER_BYTES) {
            std::cout << "[File] Too large: " << path.filename().string()
                      << " (" << fileSize << " bytes, limit "
                      << MAX_FILE_TRANSFER_BYTES << ")" << std::endl;
            return;
        }

        auto agent = GetExistingAgent(peerId);
        SOCKET outSock = INVALID_SOCKET;
        sockaddr_in addr{};
        if (!TryGetActiveSocket(agent, outSock, addr)) {
            std::cout << "[File] No active ICE path to " << peerId << std::endl;
            return;
        }

        std::ifstream input(path, std::ios::binary);
        if (!input) {
            std::cout << "[File] Unable to open: " << path.string() << std::endl;
            return;
        }

        const std::string fileName = SanitizeFileName(path.filename().string());
        const uint32_t chunkCount = static_cast<uint32_t>(
            std::max<uint64_t>(1, (fileSize + FILE_CHUNK_PAYLOAD - 1) / FILE_CHUNK_PAYLOAD));
        const uint32_t transferId = nextFileTransferId.fetch_add(1);

        std::cout << "[File] Sending " << fileName << " to " << peerId
                  << " (" << fileSize << " bytes, " << chunkCount
                  << " chunks)" << std::endl;

        const auto metaPacket =
            BuildFileMetaPacket(transferId, chunkCount, fileSize, fileName);
        const auto metaResult = SendPacketWithAck(peerId, metaPacket, transferId,
                                                  FILE_META_CHUNK_INDEX, 40, 1500);
        if (metaResult == FilePacketWaitResult::Rejected) {
            std::cout << "[File] Transfer declined by " << peerId
                      << ": " << fileName << std::endl;
            return;
        }
        if (metaResult != FilePacketWaitResult::Acked) {
            std::cout << "[File] Metadata ACK timeout for " << fileName
                      << " -> " << peerId << std::endl;
            return;
        }

        std::vector<uint8_t> chunk(FILE_CHUNK_PAYLOAD);
        for (uint32_t chunkIndex = 0; chunkIndex < chunkCount && running.load(); ++chunkIndex) {
            const uint64_t remaining =
                (fileSize > static_cast<uint64_t>(chunkIndex) * FILE_CHUNK_PAYLOAD)
                    ? fileSize - static_cast<uint64_t>(chunkIndex) * FILE_CHUNK_PAYLOAD
                    : 0;
            const size_t chunkSize =
                static_cast<size_t>(std::min<uint64_t>(FILE_CHUNK_PAYLOAD, remaining));

            if (chunkSize > 0) {
                input.read(reinterpret_cast<char*>(chunk.data()),
                           static_cast<std::streamsize>(chunkSize));
                if (input.gcount() != static_cast<std::streamsize>(chunkSize)) {
                    std::cout << "[File] Read error while sending "
                              << fileName << std::endl;
                    return;
                }
            }

            const auto chunkPacket = BuildFileChunkPacket(
                transferId, chunkIndex, chunk.data(), chunkSize);
            const auto chunkResult = SendPacketWithAck(peerId, chunkPacket, transferId,
                                                       chunkIndex, 10, 1500);
            if (chunkResult == FilePacketWaitResult::Rejected) {
                std::cout << "[File] Transfer rejected mid-stream by " << peerId
                          << ": " << fileName << std::endl;
                return;
            }
            if (chunkResult != FilePacketWaitResult::Acked) {
                std::cout << "[File] Chunk " << (chunkIndex + 1) << "/"
                          << chunkCount << " failed for " << fileName
                          << " -> " << peerId << std::endl;
                return;
            }

            if (chunkIndex == 0 || (chunkIndex + 1) == chunkCount ||
                ((chunkIndex + 1) % 32u) == 0u) {
                std::cout << "[File] Progress " << fileName << " -> "
                          << peerId << ": " << (chunkIndex + 1)
                          << "/" << chunkCount << std::endl;
            }
        }

        if (running.load()) {
            std::cout << "[File] Sent successfully: " << fileName
                      << " -> " << peerId << std::endl;
        }
    }

    void HandleIncomingFilePacket(const std::string& peerId, const void* data, int len) {
        if (!data || len <= 0) {
            return;
        }

        const auto* bytes = reinterpret_cast<const uint8_t*>(data);
        const uint8_t kind = bytes[0];

        if (kind == FILE_PACKET_ACK) {
            if (len < static_cast<int>(sizeof(FileAckHeader))) {
                return;
            }

            FileAckHeader ack{};
            memcpy(&ack, bytes, sizeof(ack));
            HandleFileAckPacket(ntohl(ack.transferId), ntohl(ack.chunkIndex));
            return;
        }
        if (kind == FILE_PACKET_REJECT) {
            if (len < static_cast<int>(sizeof(FileRejectHeader))) {
                return;
            }

            FileRejectHeader reject{};
            memcpy(&reject, bytes, sizeof(reject));
            HandleFileRejectPacket(ntohl(reject.transferId));
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        std::vector<uint8_t> responsePacket;
        bool shouldRespond = false;

        {
            std::lock_guard<std::mutex> lock(fileTransferMtx);
            CleanupFileTransfersLocked(now);

            if (kind == FILE_PACKET_META) {
                if (len < static_cast<int>(sizeof(FileMetaHeader))) {
                    return;
                }

                FileMetaHeader header{};
                memcpy(&header, bytes, sizeof(header));
                const uint32_t transferId = ntohl(header.transferId);
                const uint32_t chunkCount = ntohl(header.chunkCount);
                const uint64_t totalSize = NetworkToHost64(header.totalSize);
                const uint16_t fileNameLen = ntohs(header.fileNameLen);
                if (chunkCount == 0 ||
                    fileNameLen > MAX_FILE_NAME_BYTES ||
                    totalSize > MAX_FILE_TRANSFER_BYTES ||
                    len < static_cast<int>(sizeof(FileMetaHeader) + fileNameLen)) {
                    return;
                }

                const std::string transferKey = MakeIncomingFileKey(peerId, transferId);
                const std::string rawName(
                    reinterpret_cast<const char*>(bytes + sizeof(FileMetaHeader)),
                    fileNameLen);
                const std::string safeName = SanitizeFileName(rawName);

                auto rejectedIt = rejectedIncomingFileTransfers.find(transferKey);
                if (rejectedIt != rejectedIncomingFileTransfers.end()) {
                    rejectedIt->second = now;
                    shouldRespond = true;
                    responsePacket = BuildFileRejectPacket(transferId);
                } else {
                    auto completedIt = completedIncomingFileTransfers.find(transferKey);
                    if (completedIt != completedIncomingFileTransfers.end()) {
                        shouldRespond = true;
                        responsePacket = BuildFileAckPacket(transferId, FILE_META_CHUNK_INDEX);
                    } else {
                        auto incomingIt = incomingFileTransfers.find(transferKey);
                        if (incomingIt != incomingFileTransfers.end()) {
                            incomingIt->second.lastUpdate = now;
                            shouldRespond = true;
                            responsePacket = BuildFileAckPacket(transferId, FILE_META_CHUNK_INDEX);
                        } else if (incomingFileApprovalRequired.load()) {
                            auto pendingIt = pendingIncomingFilePrompts.find(transferKey);
                            if (pendingIt == pendingIncomingFilePrompts.end()) {
                                PendingIncomingFilePrompt prompt{};
                                prompt.peerId = peerId;
                                prompt.transferId = transferId;
                                prompt.fileName = safeName;
                                prompt.chunkCount = chunkCount;
                                prompt.totalSize = totalSize;
                                prompt.createdAt = now;
                                prompt.lastUpdate = now;
                                pendingIncomingFilePrompts.emplace(transferKey, prompt);
                                pendingIncomingFilePromptQueue.push_back(prompt);
                                std::cout << "[File] Incoming file request from " << peerId
                                          << ": " << safeName << " (" << totalSize
                                          << " bytes, " << chunkCount
                                          << " chunks). Waiting for confirmation in the UI."
                                          << std::endl;
                            } else {
                                pendingIt->second.lastUpdate = now;
                            }
                            return;
                        } else {
                            PendingIncomingFilePrompt prompt{};
                            prompt.peerId = peerId;
                            prompt.transferId = transferId;
                            prompt.fileName = safeName;
                            prompt.chunkCount = chunkCount;
                            prompt.totalSize = totalSize;
                            prompt.createdAt = now;
                            prompt.lastUpdate = now;
                            if (!BeginIncomingFileTransferLocked(transferKey, prompt, now)) {
                                return;
                            }

                            shouldRespond = true;
                            responsePacket = BuildFileAckPacket(transferId, FILE_META_CHUNK_INDEX);
                        }
                    }
                }
            } else if (kind == FILE_PACKET_DATA) {
                if (len < static_cast<int>(sizeof(FileChunkHeader))) {
                    return;
                }

                FileChunkHeader header{};
                memcpy(&header, bytes, sizeof(header));
                const uint32_t transferId = ntohl(header.transferId);
                const uint32_t chunkIndex = ntohl(header.chunkIndex);
                const size_t payloadSize =
                    static_cast<size_t>(len - static_cast<int>(sizeof(FileChunkHeader)));
                const std::string transferKey = MakeIncomingFileKey(peerId, transferId);

                auto rejectedIt = rejectedIncomingFileTransfers.find(transferKey);
                if (rejectedIt != rejectedIncomingFileTransfers.end()) {
                    rejectedIt->second = now;
                    shouldRespond = true;
                    responsePacket = BuildFileRejectPacket(transferId);
                } else {
                    auto completedIt = completedIncomingFileTransfers.find(transferKey);
                    if (completedIt != completedIncomingFileTransfers.end()) {
                        if (chunkIndex < completedIt->second.chunkCount) {
                            shouldRespond = true;
                            responsePacket = BuildFileAckPacket(transferId, chunkIndex);
                        }
                    } else {
                        auto incomingIt = incomingFileTransfers.find(transferKey);
                        if (incomingIt == incomingFileTransfers.end()) {
                            return;
                        }

                        auto& transfer = incomingIt->second;
                        transfer.lastUpdate = now;
                        if (chunkIndex > transfer.nextChunkIndex ||
                            chunkIndex >= transfer.chunkCount) {
                            return;
                        }

                        if (chunkIndex < transfer.nextChunkIndex) {
                            shouldRespond = true;
                            responsePacket = BuildFileAckPacket(transferId, chunkIndex);
                        } else {
                            const uint64_t remaining =
                                (transfer.totalSize >
                                 static_cast<uint64_t>(chunkIndex) * FILE_CHUNK_PAYLOAD)
                                    ? transfer.totalSize -
                                          static_cast<uint64_t>(chunkIndex) * FILE_CHUNK_PAYLOAD
                                    : 0;
                            const size_t expectedSize =
                                static_cast<size_t>(std::min<uint64_t>(FILE_CHUNK_PAYLOAD, remaining));
                            if (payloadSize != expectedSize) {
                                return;
                            }

                            if (payloadSize > 0) {
                                transfer.stream.write(
                                    reinterpret_cast<const char*>(bytes + sizeof(FileChunkHeader)),
                                    static_cast<std::streamsize>(payloadSize));
                            }
                            if (!transfer.stream.good()) {
                                std::cout << "[File] Write error while receiving "
                                          << transfer.fileName << " from "
                                          << peerId << std::endl;
                                transfer.stream.close();
                                std::error_code removeEc;
                                std::filesystem::remove(transfer.tempPath, removeEc);
                                incomingFileTransfers.erase(incomingIt);
                                return;
                            }

                            transfer.writtenBytes += payloadSize;
                            ++transfer.nextChunkIndex;

                            if (transfer.nextChunkIndex == transfer.chunkCount &&
                                transfer.writtenBytes == transfer.totalSize) {
                                transfer.stream.close();
                                std::error_code renameEc;
                                std::filesystem::rename(
                                    transfer.tempPath, transfer.finalPath, renameEc);
                                if (renameEc) {
                                    std::cout << "[File] Finalize failed for "
                                              << transfer.fileName << std::endl;
                                    std::error_code removeEc;
                                    std::filesystem::remove(transfer.tempPath, removeEc);
                                    incomingFileTransfers.erase(incomingIt);
                                    return;
                                } else {
                                    std::cout << "[File] Received from " << peerId
                                              << ": " << transfer.fileName
                                              << " -> " << transfer.finalPath.string()
                                              << std::endl;
                                }

                                CompletedIncomingFileTransfer completed{};
                                completed.chunkCount = transfer.chunkCount;
                                completed.completedAt = now;
                                completedIncomingFileTransfers[transferKey] = completed;
                                shouldRespond = true;
                                responsePacket = BuildFileAckPacket(transferId, chunkIndex);
                                incomingFileTransfers.erase(incomingIt);
                            } else {
                                shouldRespond = true;
                                responsePacket = BuildFileAckPacket(transferId, chunkIndex);
                            }
                        }
                    }
                }
            } else {
                return;
            }
        }

        if (shouldRespond && !responsePacket.empty()) {
            SendBufferToPeer(peerId, responsePacket.data(), responsePacket.size());
        }
    }
    struct ScreenTargetEndpoint {
        std::string peerId;
        SOCKET socket = INVALID_SOCKET;
        sockaddr_in addr{};
    };

    std::vector<ScreenTargetEndpoint> ResolveScreenTargets(const std::string& target) {
        std::vector<std::pair<std::string, std::shared_ptr<IceAgent>>> agents;
        {
            std::lock_guard<std::mutex> lock(peersMtx);
            if (target == "*" || target == "all") {
                for (const auto& [peerId, state] : peers) {
                    if (state && state->agent && !state->reconnecting.load()) {
                        agents.emplace_back(peerId, state->agent);
                    }
                }
            } else {
                auto it = peers.find(target);
                if (it != peers.end() && it->second && it->second->agent && !it->second->reconnecting.load()) {
                    agents.emplace_back(it->first, it->second->agent);
                }
            }
        }

        std::vector<ScreenTargetEndpoint> endpoints;
        endpoints.reserve(agents.size());
        for (const auto& [peerId, agent] : agents) {
            SOCKET outSock = INVALID_SOCKET;
            sockaddr_in addr{};
            if (TryGetActiveSocket(agent, outSock, addr)) {
                ScreenTargetEndpoint endpoint;
                endpoint.peerId = peerId;
                endpoint.socket = outSock;
                endpoint.addr = addr;
                endpoints.push_back(endpoint);
            }
        }
        return endpoints;
    }

    void SendScreenFrame(const ScreenShareEngine::EncodedFrame& frame) {
        if (frame.jpeg.empty()) {
            return;
        }

        std::string target;
        {
            std::lock_guard<std::mutex> lock(screenShareMtx);
            target = screenShareTarget;
        }

        const auto endpoints = ResolveScreenTargets(target);
        if (endpoints.empty()) {
            return;
        }

        const size_t totalSize = frame.jpeg.size();
        const size_t chunkCount = (totalSize + SCREEN_CHUNK_PAYLOAD - 1) / SCREEN_CHUNK_PAYLOAD;
        if (chunkCount == 0 || chunkCount > MAX_SCREEN_CHUNKS) {
            return;
        }

        const uint16_t frameId = nextScreenFrameId.fetch_add(1);
        for (size_t i = 0; i < chunkCount; ++i) {
            const size_t offset = i * SCREEN_CHUNK_PAYLOAD;
            const size_t chunkSize = std::min(SCREEN_CHUNK_PAYLOAD, totalSize - offset);

            ScreenChunkHeader header{};
            header.frameId = htons(frameId);
            header.chunkIndex = htons(static_cast<uint16_t>(i));
            header.chunkCount = htons(static_cast<uint16_t>(chunkCount));
            header.totalSize = htonl(static_cast<uint32_t>(totalSize));

            std::vector<uint8_t> packet;
            packet.resize(1 + sizeof(ScreenChunkHeader) + chunkSize);
            packet[0] = PACKET_SCREEN;
            memcpy(packet.data() + 1, &header, sizeof(ScreenChunkHeader));
            memcpy(packet.data() + 1 + sizeof(ScreenChunkHeader), frame.jpeg.data() + offset, chunkSize);

            for (const auto& endpoint : endpoints) {
                sendto(endpoint.socket,
                       reinterpret_cast<const char*>(packet.data()),
                       static_cast<int>(packet.size()),
                       0,
                       reinterpret_cast<const sockaddr*>(&endpoint.addr),
                       sizeof(endpoint.addr));
            }
        }
    }

    void HandleIncomingScreenChunk(const std::string& peerId, const void* data, int len) {
        if (!data || len <= static_cast<int>(sizeof(ScreenChunkHeader))) {
            return;
        }

        const auto* bytes = reinterpret_cast<const uint8_t*>(data);
        ScreenChunkHeader header{};
        memcpy(&header, bytes, sizeof(ScreenChunkHeader));

        const uint16_t frameId = ntohs(header.frameId);
        const uint16_t chunkIndex = ntohs(header.chunkIndex);
        const uint16_t chunkCount = ntohs(header.chunkCount);
        const uint32_t totalSize = ntohl(header.totalSize);
        const uint8_t* payload = bytes + sizeof(ScreenChunkHeader);
        const int payloadLen = len - static_cast<int>(sizeof(ScreenChunkHeader));

        if (chunkCount == 0 || chunkCount > MAX_SCREEN_CHUNKS || chunkIndex >= chunkCount ||
            payloadLen <= 0 || totalSize == 0 || totalSize > 8 * 1024 * 1024) {
            return;
        }

        std::vector<uint8_t> completedJpeg;
        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(screenRxMtx);

            for (auto it = screenRxBuffers.begin(); it != screenRxBuffers.end();) {
                if (now - it->second.lastUpdate > std::chrono::seconds(5)) {
                    it = screenRxBuffers.erase(it);
                } else {
                    ++it;
                }
            }

            auto& assembly = screenRxBuffers[peerId];
            if (assembly.frameId != frameId ||
                assembly.chunkCount != chunkCount ||
                assembly.totalSize != totalSize) {
                assembly = ScreenRxAssembly{};
                assembly.frameId = frameId;
                assembly.chunkCount = chunkCount;
                assembly.totalSize = totalSize;
                assembly.chunks.resize(chunkCount);
                assembly.received.assign(chunkCount, 0);
                assembly.lastUpdate = now;
            }

            assembly.lastUpdate = now;
            if (!assembly.received[chunkIndex]) {
                assembly.chunks[chunkIndex].assign(payload, payload + payloadLen);
                assembly.received[chunkIndex] = 1;
                assembly.receivedCount++;
                assembly.receivedBytes += payloadLen;
            }

            if (assembly.receivedCount == assembly.chunkCount) {
                completedJpeg.reserve(assembly.receivedBytes);
                for (const auto& chunk : assembly.chunks) {
                    completedJpeg.insert(completedJpeg.end(), chunk.begin(), chunk.end());
                }
                if (completedJpeg.size() > assembly.totalSize) {
                    completedJpeg.resize(assembly.totalSize);
                }
                screenRxBuffers.erase(peerId);
            }
        }

        if (completedJpeg.empty()) {
            return;
        }

        ScreenShareEngine::PreviewFrame decoded;
        if (!screenShare.DecodeJpegToPreview(completedJpeg, decoded)) {
            return;
        }

        decoded.sequence = remoteScreenPreviewSeq.fetch_add(1) + 1;
        {
            std::lock_guard<std::mutex> lock(screenRxMtx);
            remoteScreenPreviews[peerId] = std::move(decoded);
        }
    }

    bool StartScreenShare(const std::string& target) {
        if (target.empty()) {
            return false;
        }

        StopScreenShare();
        {
            std::lock_guard<std::mutex> lock(screenShareMtx);
            screenShareTarget = target;
        }

        const bool started = screenShare.Start([this](const ScreenShareEngine::EncodedFrame& frame) {
            SendScreenFrame(frame);
        });

        if (started) {
            std::cout << "[Screen] Sharing started to " << target << std::endl;
        } else {
            std::cout << "[Screen] Unable to start capture (GDI+/encoder init failed)" << std::endl;
        }
        return started;
    }

    void StopScreenShare() {
        if (screenShare.IsRunning()) {
            screenShare.Stop();
            std::cout << "[Screen] Sharing stopped" << std::endl;
        }
    }

    bool IsScreenShareRunning() const {
        return screenShare.IsRunning();
    }

    void SetScreenShareFps(int value) {
        screenShare.SetFps(value);
    }

    int GetScreenShareFps() const {
        return screenShare.GetFps();
    }

    void SetScreenShareQuality(int value) {
        screenShare.SetQuality(value);
    }

    int GetScreenShareQuality() const {
        return screenShare.GetQuality();
    }

    void SetScreenShareScalePercent(int value) {
        screenShare.SetScalePercent(value);
    }

    int GetScreenShareScalePercent() const {
        return screenShare.GetScalePercent();
    }

    std::vector<ScreenShareEngine::SourceInfo> GetScreenShareSources() const {
        return screenShare.GetSources();
    }

    std::string GetScreenShareSourceId() const {
        return screenShare.GetSourceId();
    }

    bool SetScreenShareSource(const std::string& sourceId) {
        const bool changed = screenShare.SetSourceId(sourceId);
        if (changed) {
            std::cout << "[Screen] Source set to " << sourceId << std::endl;
        }
        return changed;
    }

    void SendFileToPeer(const std::string& peerId, const std::filesystem::path& path) {
        if (peerId.empty()) {
            std::cout << "[Usage] file <id> <path>" << std::endl;
            return;
        }
        QueueFileSend(peerId, path);
    }

    std::filesystem::path GetReceivedFilesDirectory() const {
        return GetReceivedFilesRoot();
    }

    std::vector<AudioEngine::DeviceInfo> GetInputDevices() const {
        return audio.GetInputDevices();
    }

    std::vector<AudioEngine::DeviceInfo> GetOutputDevices() const {
        return audio.GetOutputDevices();
    }

    UINT GetSelectedInputDeviceId() const {
        return audio.GetSelectedInputDeviceId();
    }

    UINT GetSelectedOutputDeviceId() const {
        return audio.GetSelectedOutputDeviceId();
    }

    bool SetInputDevice(UINT deviceId) {
        const bool changed = audio.SetInputDevice(deviceId);
        if (changed) {
            std::cout << "[AudioCtl] Input device switched" << std::endl;
        }
        return changed;
    }

    bool SetOutputDevice(UINT deviceId) {
        const bool changed = audio.SetOutputDevice(deviceId);
        if (changed) {
            std::cout << "[AudioCtl] Output device switched" << std::endl;
        }
        return changed;
    }

    std::string GetScreenShareTarget() {
        std::lock_guard<std::mutex> lock(screenShareMtx);
        return screenShareTarget;
    }

    bool GetLocalScreenPreview(ScreenShareEngine::PreviewFrame& out) const {
        return screenShare.GetLatestPreview(out);
    }

    bool GetLatestRemoteScreenPreview(std::string& peerId, ScreenShareEngine::PreviewFrame& out) {
        std::lock_guard<std::mutex> lock(screenRxMtx);
        if (remoteScreenPreviews.empty()) {
            return false;
        }

        const auto best = std::max_element(
            remoteScreenPreviews.begin(),
            remoteScreenPreviews.end(),
            [](const auto& a, const auto& b) {
                return a.second.sequence < b.second.sequence;
            });
        if (best == remoteScreenPreviews.end()) {
            return false;
        }

        peerId = best->first;
        out = best->second;
        return !out.bgra.empty();
    }

    std::vector<std::string> GetRemoteScreenStreamIds() {
        std::lock_guard<std::mutex> lock(screenRxMtx);
        std::vector<std::string> ids;
        ids.reserve(remoteScreenPreviews.size());
        for (const auto& [peerId, frame] : remoteScreenPreviews) {
            if (!frame.bgra.empty() && frame.width > 0 && frame.height > 0) {
                ids.push_back(peerId);
            }
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    bool GetRemoteScreenPreviewByPeer(const std::string& peerId, ScreenShareEngine::PreviewFrame& out) {
        std::lock_guard<std::mutex> lock(screenRxMtx);
        auto it = remoteScreenPreviews.find(peerId);
        if (it == remoteScreenPreviews.end()) {
            return false;
        }
        if (it->second.bgra.empty() || it->second.width <= 0 || it->second.height <= 0) {
            return false;
        }
        out = it->second;
        return true;
    }

    void BroadcastMessage(const std::string& text) {
        std::string encrypted = XorKey(text);
        std::vector<char> buffer;
        buffer.push_back(PACKET_TEXT);
        buffer.insert(buffer.end(), encrypted.begin(), encrypted.end());
        int count = 0;
        
        std::vector<std::shared_ptr<IceAgent>> agentsCopy;
        {
            std::lock_guard<std::mutex> lock(peersMtx);
            for (auto const& [peerId, state] : peers) {
                if (!state || !state->agent) continue;
                auto [isConnected, sock, remote] = state->agent->GetActiveConnection();
                if (isConnected && sock != INVALID_SOCKET) {
                    agentsCopy.push_back(state->agent);
                }
            }
        }
        
        for (auto& agent : agentsCopy) {
            SOCKET outSock;
            sockaddr_in addr{};
            if (TryGetActiveSocket(agent, outSock, addr)) {
                sendto(outSock, buffer.data(), (int)buffer.size(), 0, (sockaddr*)&addr, sizeof(addr));
                count++;
            }
        }

        std::cout << "[System] Broadcast sent to " << count << " peers." << std::endl;
    }

    void ProcessAgents() {
        std::vector<std::shared_ptr<IceAgent>> agentsCopy;
        {
            std::lock_guard<std::mutex> lock(peersMtx);
            for (auto const& [peerId, state] : peers) {
                if (!state || !state->agent || state->reconnecting.load()) {
                    continue;
                }
                agentsCopy.push_back(state->agent);
            }
        }

        for (auto& agent : agentsCopy) {
            agent->Process();
        }
    }

    std::string XorKey(std::string data) {
        ApplyXor55(data.data(), data.size());
        return data;
    }

    void PingLoop() {
        constexpr int LOOP_TICK_MS = 1000;
        constexpr int PING_INTERVAL_MS = 25000;
        constexpr int LIST_INTERVAL_MS = 6000;

        auto nextPingAt =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(PING_INTERVAL_MS);
        auto nextListAt =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);

        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(LOOP_TICK_MS));
            if (!running.load()) {
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= nextListAt) {
                signalling.RequestUserList();
                nextListAt = now + std::chrono::milliseconds(LIST_INTERVAL_MS);
            }

            if (now < nextPingAt) {
                continue;
            }
            nextPingAt = now + std::chrono::milliseconds(PING_INTERVAL_MS);

            {
                std::lock_guard<std::mutex> lock(peersMtx);
                for (auto& [peerId, state] : peers) {
                    if (!state || !state->agent || state->reconnecting.load()) continue;
                    
                    auto [isConnected, sock, remote] = state->agent->GetActiveConnection();
                    if (!isConnected || sock == INVALID_SOCKET) continue;
                    
                    state->agent->SendPing();
                    state->lastPingSent = std::chrono::steady_clock::now();
                }
            }

            std::vector<std::string> timedOutPeers;
            {
                std::lock_guard<std::mutex> lock(peersMtx);
                for (auto& [peerId, state] : peers) {
                    if (!state || !state->agent || state->reconnecting.load()) continue;
                    
                    if (state->agent->CheckActivityTimeout()) {
                        timedOutPeers.push_back(peerId);
                    }
                }
            }

            for (const auto& peerId : timedOutPeers) {
                auto agent = GetExistingAgent(peerId);
                if (agent && agent->onDisconnected) {
                    std::cout << "[System] Activity timeout for " << peerId << ", connection dead\n";
                    agent->onDisconnected();
                }
            }
        }
    }

    bool TryParseCandidateMessage(const std::string& data, Candidate& cand) {
        if (data.find("host") == 0) {
            char ip[64] = {};
            int port = 0;
            int scanned = sscanf_s(data.c_str(), "host %63[^:]:%d", ip, (unsigned)sizeof(ip), &port);
            if (scanned != 2) {
                return false;
            }

            cand.type = HOST;
            cand.ip = ip;
            cand.port = static_cast<uint16_t>(port);
            return true;
        }

        if (data.find("srflx") == 0) {
            char rip[64] = {};
            char lip[64] = {};
            int rport = 0;
            int lport = 0;
            int scanned = sscanf_s(
                data.c_str(),
                "srflx %63[^:]:%d related %63[^:]:%d",
                rip, (unsigned)sizeof(rip), &rport,
                lip, (unsigned)sizeof(lip), &lport);

            if (scanned != 4) {
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

    void RegisterPeerAddress(const std::string& ip, uint16_t port, std::shared_ptr<IceAgent> agent) {
        std::lock_guard<std::mutex> lock(addrMapMtx);
        std::string addrKey = ip + ":" + std::to_string(port);
        addrToAgent[addrKey] = agent;
    }

    std::vector<std::string> GetPeerIds() {
        std::vector<std::string> peerIds;
        std::lock_guard<std::mutex> lock(peersMtx);
        peerIds.reserve(peers.size());
        for (const auto& [peerId, state] : peers) {
            if (state) {
                peerIds.push_back(peerId);
            }
        }
        return peerIds;
    }

    std::vector<std::string> GetConnectedPeerIds() {
        std::vector<std::string> peerIds;
        std::lock_guard<std::mutex> lock(peersMtx);
        peerIds.reserve(peers.size());
        for (const auto& [peerId, state] : peers) {
            if (!state || !state->agent || state->reconnecting.load()) {
                continue;
            }

            auto [connected, sock, remote] = state->agent->GetActiveConnection();
            if (connected && sock != INVALID_SOCKET) {
                peerIds.push_back(peerId);
            }
        }
        std::sort(peerIds.begin(), peerIds.end());
        return peerIds;
    }

    std::vector<std::string> GetOnlineUsers() {
        std::lock_guard<std::mutex> lock(onlineUsersMtx);
        return onlineUsers;
    }

    float GetPeerVolume(const std::string& peerId) {
        return audio.GetSourceVolume(peerId);
    }

    void SetPeerVolume(const std::string& peerId, float volume) {
        const float clamped = std::clamp(volume, 0.0f, 2.0f);
        const float current = audio.GetSourceVolume(peerId);
        if (current >= clamped - 0.005f && current <= clamped + 0.005f) {
            return;
        }

        audio.SetSourceVolume(peerId, clamped);
        std::cout << "[AudioCtl] " << peerId << " volume set to "
                  << static_cast<int>(clamped * 100.0f + 0.5f) << "%" << std::endl;
    }

    bool IsEchoSuppressionEnabled() {
        return audio.IsEchoSuppressionEnabled();
    }

    void SetEchoSuppressionEnabled(bool enabled) {
        audio.SetEchoSuppressionEnabled(enabled);
        std::cout << "[AudioCtl] Echo suppression "
                  << (enabled ? "ON" : "OFF") << std::endl;
    }

    float GetEchoCorrelationThreshold() {
        return audio.GetEchoCorrelationThreshold();
    }

    void SetEchoCorrelationThreshold(float value) {
        audio.SetEchoCorrelationThreshold(value);
        std::cout << "[AudioCtl] Echo correlation threshold "
                  << static_cast<int>(audio.GetEchoCorrelationThreshold() * 100.0f + 0.5f)
                  << "%" << std::endl;
    }

    float GetEchoSubtractionMaxGain() {
        return audio.GetEchoSubtractionMaxGain();
    }

    void SetEchoSubtractionMaxGain(float value) {
        audio.SetEchoSubtractionMaxGain(value);
        std::cout << "[AudioCtl] Echo subtraction gain x"
                  << audio.GetEchoSubtractionMaxGain() << std::endl;
    }

    float GetEchoResidualAttenuation() {
        return audio.GetEchoResidualAttenuation();
    }

    void SetEchoResidualAttenuation(float value) {
        audio.SetEchoResidualAttenuation(value);
        std::cout << "[AudioCtl] Echo residual attenuation "
                  << static_cast<int>(audio.GetEchoResidualAttenuation() * 100.0f + 0.5f)
                  << "%" << std::endl;
    }

    float GetInputGain() {
        return audio.GetInputGain();
    }

    void SetInputGain(float value) {
        audio.SetInputGain(value);
        std::cout << "[AudioCtl] Input gain x"
                  << audio.GetInputGain() << std::endl;
    }

    bool IsAutomaticGainEnabled() {
        return audio.IsAutomaticGainEnabled();
    }

    void SetAutomaticGainEnabled(bool enabled) {
        audio.SetAutomaticGainEnabled(enabled);
        std::cout << "[AudioCtl] AGC "
                  << (enabled ? "ON" : "OFF") << std::endl;
    }

    float GetAutomaticGainTargetLevel() {
        return audio.GetAutomaticGainTargetLevel();
    }

    void SetAutomaticGainTargetLevel(float value) {
        audio.SetAutomaticGainTargetLevel(value);
        std::cout << "[AudioCtl] AGC target "
                  << static_cast<int>(audio.GetAutomaticGainTargetLevel() * 100.0f + 0.5f)
                  << "%" << std::endl;
    }

    float GetAutomaticGainMaxBoost() {
        return audio.GetAutomaticGainMaxBoost();
    }

    void SetAutomaticGainMaxBoost(float value) {
        audio.SetAutomaticGainMaxBoost(value);
        std::cout << "[AudioCtl] AGC max boost x"
                  << audio.GetAutomaticGainMaxBoost() << std::endl;
    }

    float GetNoiseGateThreshold() {
        return audio.GetNoiseGateThreshold();
    }

    void SetNoiseGateThreshold(float value) {
        audio.SetNoiseGateThreshold(value);
        std::cout << "[AudioCtl] Noise gate "
                  << static_cast<int>(audio.GetNoiseGateThreshold() * 100.0f + 0.5f)
                  << "%" << std::endl;
    }

    AudioEngine::VoicePreset GetVoicePreset() const {
        return audio.GetVoicePreset();
    }

    void ApplyVoicePreset(AudioEngine::VoicePreset preset) {
        audio.ApplyVoicePreset(preset);

        const char* label = "Custom";
        switch (audio.GetVoicePreset()) {
        case AudioEngine::VoicePreset::Balanced:
            label = "Balanced";
            break;
        case AudioEngine::VoicePreset::Headset:
            label = "Headset";
            break;
        case AudioEngine::VoicePreset::Speakers:
            label = "Speakers";
            break;
        case AudioEngine::VoicePreset::SpeakerMax:
            label = "Speaker Max";
            break;
        case AudioEngine::VoicePreset::Conference:
            label = "Conference";
            break;
        case AudioEngine::VoicePreset::Custom:
        default:
            label = "Custom";
            break;
        }
        std::cout << "[AudioCtl] Voice preset " << label << std::endl;
    }

    void PrintHelp() {
        std::cout << "[" << id << "] Ready. Commands:\n"
                  << "  register [id]    - send REGISTER (reuse or change id)\n"
                  << "  offer <id>       - send offer to peer\n"
                  << "  msg <id> <text>  - send message via ICE\n"
                  << "  file <id> <path> - send file via ICE with ACK/retry\n"
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

    void Start(const char* serverIp, int serverPort) {
        if (started.exchange(true)) {
            return;
        }

        running = true;

        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        cp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

        InitMyCandidates();
        SetConsoleTitleA(("P2P Chat - ID: " + id).c_str());
        audio.StartPlaying();
        
        worker = std::thread([this]() {
            auto lastProcessTick = std::chrono::steady_clock::now();
            while (running.load()) {
                DWORD bytes;
                ULONG_PTR key;
                LPOVERLAPPED ov;
                BOOL ok = GetQueuedCompletionStatus(cp, &bytes, &key, &ov, 100);
                DWORD completionError = ok ? ERROR_SUCCESS : GetLastError();

                auto now = std::chrono::steady_clock::now();
                if (now - lastProcessTick >= std::chrono::milliseconds(100)) {
                    ProcessAgents();
                    lastProcessTick = now;
                }
                
                if (!ov) continue;
                
                auto* ctx = CONTAINING_RECORD(ov, IceAgent::IceUdpContext, overlapped);
                
                if (ctx->signature != IceAgent::IceUdpContext::SIGNATURE) {
                    continue;
                }
                
                if (ctx->detached.load(std::memory_order_acquire)) {
                    continue;
                }

                char fromIp[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &ctx->remoteAddr.sin_addr, fromIp, sizeof(fromIp));
                std::string addrKey = std::string(fromIp) + ":" + std::to_string(ntohs(ctx->remoteAddr.sin_port));

                std::shared_ptr<IceAgent> targetAgent = nullptr;
                {
                    std::lock_guard<std::mutex> lock(addrMapMtx);
                    auto it = addrToAgent.find(addrKey);
                    if (it != addrToAgent.end()) {
                        targetAgent = it->second.lock();
                    }
                }

                IceAgent* ctxAgent = ctx->agent.load(std::memory_order_acquire);
                if (!ctxAgent || ctx->detached.load(std::memory_order_acquire)) {
                    continue;
                }

                if (!ok && completionError == ERROR_OPERATION_ABORTED) {
                    continue;
                }

                if (bytes == 0) {
                    if (!ctx->detached.load(std::memory_order_acquire)) {
                        ctxAgent->RearmReceiveContext(ctx);
                    }
                    continue;
                }

                if (targetAgent && targetAgent.get() != ctxAgent) {
                    targetAgent->OnUdpPacket(ctx->socket, ctx->remoteAddr, ctx->buffer, static_cast<int>(bytes));
                    if (!ctx->detached.load(std::memory_order_acquire)) {
                        ctxAgent->RearmReceiveContext(ctx);
                    }
                    continue;
                }

                if (!ctx->detached.load(std::memory_order_acquire)) {
                    ctxAgent->OnReceiveCompleted(ctx, bytes);
                }
            }
        });

        pingWorker = std::thread(&P2PClient::PingLoop, this);

        const bool signallingConnected = signalling.Connect(serverIp, serverPort, id, [this](const std::string& from, const std::string& type, const std::string& data) {
            if (type == "offer") {
                if (incomingOfferApprovalRequired.load()) {
                    QueueIncomingOfferPrompt(from);
                } else {
                    ProcessIncomingOffer(from);
                }
            }
            else if (type == "candidate") {
                Candidate cand;
                if (!TryParseCandidateMessage(data, cand)) {
                    std::cerr << "[ICE] Ignoring malformed candidate from " << from
                              << ": " << data << std::endl;
                    return;
                }

                auto parsedAgent = GetOrCreateAgent(from);
                parsedAgent->AddRemoteCandidate(cand);
                RegisterPeerAddress(cand.ip, cand.port, parsedAgent);
                bool canStart = false;
                {
                    std::lock_guard<std::mutex> lock(peersMtx);
                    auto it = peers.find(from);
                    canStart = (it != peers.end() && it->second->negotiationReady.load());
                }
                if (canStart) {
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
            else if (type == "offer_declined") {
                std::cout << "[Offer] " << from << " declined your offer" << std::endl;
            }
            else if (type == "answer") {
                auto agent = GetOrCreateAgent(from);
                
                {
                    std::lock_guard<std::mutex> lock(peersMtx);
                    auto it = peers.find(from);
                    if (it != peers.end() && it->second->reconnecting.load()) {
                        it->second->reconnecting = false;
                    }
                    if (it != peers.end()) {
                        it->second->negotiationReady = true;
                    }
                }
                
                for (const auto& cand : agent->GetLocalCandidates()) {
                    std::string candStr = (cand.type == HOST) ? 
                        "host " + cand.ip + ":" + std::to_string(cand.port) :
                        "srflx " + cand.ip + ":" + std::to_string(cand.port) + " related " + cand.relatedIp + ":" + std::to_string(cand.relatedPort);
                    signalling.SendCandidate(from, candStr);
                }
                
                std::cout << "[ICE] Got answer from " << from << ", scheduling ICE start in 500ms...\n";
                ScheduleDelayedICE(from, 500);
            }
            else if (type == "list") {
                const auto parsedUsers = ParseUserListPayload(data);
                {
                    std::lock_guard<std::mutex> lock(onlineUsersMtx);
                    onlineUsers = parsedUsers;
                }
                std::cout << "[System] Online users: "
                          << JoinUserList(parsedUsers) << std::endl;
            }
            else if (type == "server_chat") {
                std::cout << "[Server Relay from " << from << "]: " << data << std::endl;
            }
        });

        if (signallingConnected) {
            signalling.RequestUserList();
        } else {
            std::cerr << "[System] Failed to connect to signalling server "
                      << serverIp << ":" << serverPort << std::endl;
        }
    }

    void ExecuteCommand(const std::string& line) {
        if (!started.load()) {
            return;
        }

        if (line == "register") {
            if (id.empty()) {
                std::cout << "[Usage] register <id>" << std::endl;
                return;
            }

            if (signalling.Register(id)) {
                std::cout << "[System] REGISTER sent as " << id << std::endl;
            } else {
                std::cout << "[Error] Failed to send REGISTER (socket unavailable?)" << std::endl;
            }
        }
        else if (line.find("register ") == 0) {
            std::string newId = line.substr(9);
            if (newId.empty() || newId.find(' ') != std::string::npos) {
                std::cout << "[Usage] register <id>" << std::endl;
                return;
            }

            if (signalling.Register(newId)) {
                id = newId;
                SetConsoleTitleA(("P2P Chat - ID: " + id).c_str());
                std::cout << "[System] REGISTER sent as " << id << std::endl;
            } else {
                std::cout << "[Error] Failed to send REGISTER (socket unavailable?)" << std::endl;
            }
        }
        else if (line.find("stream on ") == 0) {
            std::string target = line.substr(10);
            if (target.empty()) {
                std::cout << "[Usage] stream on <id|*>" << std::endl;
                return;
            }

            if (!StartScreenShare(target)) {
                std::cout << "[Error] Failed to start screen sharing" << std::endl;
            }
        }
        else if (line == "stream off") {
            StopScreenShare();
        }
        else if (line.find("stream fps ") == 0) {
            try {
                int fpsValue = std::stoi(line.substr(11));
                SetScreenShareFps(fpsValue);
                std::cout << "[Screen] FPS set to " << GetScreenShareFps() << std::endl;
            } catch (...) {
                std::cout << "[Usage] stream fps <1-60>" << std::endl;
            }
        }
        else if (line.find("stream quality ") == 0) {
            try {
                int qualityValue = std::stoi(line.substr(15));
                SetScreenShareQuality(qualityValue);
                std::cout << "[Screen] Quality set to " << GetScreenShareQuality() << std::endl;
            } catch (...) {
                std::cout << "[Usage] stream quality <20-95>" << std::endl;
            }
        }
        else if (line.find("stream scale ") == 0) {
            try {
                int scaleValue = std::stoi(line.substr(13));
                SetScreenShareScalePercent(scaleValue);
                std::cout << "[Screen] Scale set to " << GetScreenShareScalePercent() << "%" << std::endl;
            } catch (...) {
                std::cout << "[Usage] stream scale <20-100>" << std::endl;
            }
        }
        else if (line.find("offer ") == 0) {
            std::string target = line.substr(6);
            auto agent = GetOrCreateAgent(target);
            
            {
                std::lock_guard<std::mutex> lock(peersMtx);
                auto it = peers.find(target);
                if (it != peers.end()) {
                    it->second->iceStarted = false;
                    it->second->negotiationReady = false;
                }
            }
            CancelPendingICE(target);
            
            gatherer.StartGathering("stun.l.google.com", 19302, [this, target, agent](std::vector<Candidate> locals) {
                {
                    std::lock_guard<std::mutex> lock(localCandidatesMtx);
                    myLocalCandidates = locals;
                }
                for (auto& c : locals) agent->AddLocalCandidate(c);
                for (const auto& cand : agent->GetLocalCandidates()) {
                    std::string s = (cand.type == HOST) ? 
                        "host " + cand.ip + ":" + std::to_string(cand.port) : 
                        "srflx " + cand.ip + ":" + std::to_string(cand.port) + " related " + cand.relatedIp + ":" + std::to_string(cand.relatedPort);
                    signalling.SendCandidate(target, s);
                }
                signalling.SendOffer(target);
            });
        }
        else if (line == "list") {
            signalling.RequestUserList();
        }
        else if (line.find("reconnect ") == 0) {
            std::string target = line.substr(10);
            std::cout << "[System] Forcing reconnect to " << target << "...\n";
            ScheduleReconnect(target);
        }
        else if (line.find("volume ") == 0) {
            size_t space2 = line.find(' ', 7);
            if (space2 == std::string::npos) {
                std::cout << "[Usage] volume <id> <0-200>" << std::endl;
                return;
            }

            std::string target = line.substr(7, space2 - 7);
            std::string valueText = line.substr(space2 + 1);
            if (target.empty() || valueText.empty()) {
                std::cout << "[Usage] volume <id> <0-200>" << std::endl;
                return;
            }

            try {
                const float percent = std::stof(valueText);
                SetPeerVolume(target, percent / 100.0f);
            } catch (...) {
                std::cout << "[Usage] volume <id> <0-200>" << std::endl;
            }
        }
        else if (line.find("voice on ") == 0) {
            std::string target = line.substr(9);
            std::shared_ptr<IceAgent> targetAgent = GetExistingAgent(target);

            if (targetAgent) {
                std::cout << "[System] Microphone ON for " << target << std::endl;
                audio.StartRecording([this, target](void* data, int len) {
                    auto state = GetExistingPeerState(target);
                    auto currentAgent = GetExistingAgent(target);
                    if (!currentAgent) {
                        if (state) {
                            auto dropIndex = state->audioTxDrops.fetch_add(1) + 1;
                            if (dropIndex <= 5 || dropIndex % 50 == 0) {
                                std::cout << "[Audio] TX to " << target
                                          << " waiting for agent, drop #" << dropIndex
                                          << " len=" << len << std::endl;
                            }
                        }
                        return;
                    }

                    SOCKET outSock = INVALID_SOCKET;
                    sockaddr_in addr{};
                    if (!TryGetActiveSocket(currentAgent, outSock, addr)) {
                        if (state) {
                            auto dropIndex = state->audioTxDrops.fetch_add(1) + 1;
                            if (dropIndex <= 5 || dropIndex % 50 == 0) {
                                std::cout << "[Audio] TX to " << target
                                          << " waiting for active path, drop #" << dropIndex
                                          << " len=" << len << std::endl;
                            }
                        }
                        return;
                    }

                    if (len <= 0 || static_cast<size_t>(len) > AUDIO_FRAME_BYTES) {
                        return;
                    }

                    std::array<char, AUDIO_PACKET_BYTES> packet{};
                    packet[0] = static_cast<char>(PACKET_AUDIO);
                    memcpy(packet.data() + 1, data, static_cast<size_t>(len));

                    const int packetLen = len + 1;
                    int sent = sendto(outSock, packet.data(), packetLen, 0, (sockaddr*)&addr, sizeof(addr));
                    if (sent == SOCKET_ERROR) {
                        if (state) {
                            auto errorIndex = state->audioTxErrors.fetch_add(1) + 1;
                            std::cerr << "[Audio] TX to " << target
                                      << " failed #" << errorIndex
                                      << ": " << WSAGetLastError() << std::endl;
                        }
                        return;
                    }

                    if (state) {
                        auto packetIndex = state->audioTxPackets.fetch_add(1) + 1;
                        if (packetIndex <= 5 || packetIndex % 50 == 0) {
                            std::cout << "[Audio] TX to " << target
                                      << " packet #" << packetIndex
                                      << " len=" << len << std::endl;
                        }
                    }
                });
            } else {
                std::cout << "[Error] No agent found for " << target << std::endl;
            }
        }
        else if (line == "voice off") {
            audio.StopRecording(); 
            std::cout << "[System] Microphone OFF." << std::endl;
        }
        else if (line == "brdvoice on") {
            std::cout << "[System] Global voice ON." << std::endl;
            audio.StartRecording([this](void* data, int len) {
                if (len <= 0 || static_cast<size_t>(len) > AUDIO_FRAME_BYTES) {
                    return;
                }

                std::array<char, AUDIO_PACKET_BYTES> packet{};
                packet[0] = static_cast<char>(PACKET_AUDIO);
                memcpy(packet.data() + 1, data, static_cast<size_t>(len));
                const int packetLen = len + 1;

                std::vector<std::shared_ptr<IceAgent>> agentsCopy;
                {
                    std::lock_guard<std::mutex> lock(peersMtx);
                    for (auto const& [id, state] : peers) {
                        if (state && state->agent) {
                            agentsCopy.push_back(state->agent);
                        }
                    }
                }
                
                for (auto& agent : agentsCopy) {
                    auto [isConnected, sock, remote] = agent->GetActiveConnection();
                    if (isConnected && sock != INVALID_SOCKET) {
                        SOCKET outSock;
                        sockaddr_in addr{};
                        if (TryGetActiveSocket(agent, outSock, addr)) {
                            sendto(outSock, packet.data(), packetLen, 0, (sockaddr*)&addr, sizeof(addr));
                        }
                    }
                }
            });
        }
        else if (line.compare(0, 4, "brd ") == 0) {
            std::string text = line.substr(4);
            BroadcastMessage(text);
        }
        else if (line.find("msg ") == 0) {
            size_t space2 = line.find(' ', 4);
            if (space2 != std::string::npos) {
                std::string target = line.substr(4, space2 - 4);
                std::string text = line.substr(space2 + 1);
                
                std::shared_ptr<IceAgent> agent = nullptr;
                {
                    std::lock_guard<std::mutex> lock(peersMtx);
                    auto it = peers.find(target);
                    if (it != peers.end() && it->second->agent) {
                        agent = it->second->agent;
                    }
                }
                
                if (!agent) {
                    std::cout << "[Error] No session found for " << target << std::endl;
                    return;
                }
                
                auto [isConnected, sock, remote] = agent->GetActiveConnection();
                if (!isConnected || sock == INVALID_SOCKET) {
                    std::cout << "[System] Still establishing ICE connection to " << target << "..." << std::endl;
                    return;
                }
                
                std::string encrypted = XorKey(text);
                std::vector<char> buffer;
                buffer.push_back(PACKET_TEXT); 
                buffer.insert(buffer.end(), encrypted.begin(), encrypted.end());
                
                SOCKET outSock;
                sockaddr_in addr{};
                if (TryGetActiveSocket(agent, outSock, addr)) {
                    int sent = sendto(outSock, buffer.data(), (int)buffer.size(), 0, (sockaddr*)&addr, sizeof(addr));
                    if (sent != SOCKET_ERROR) {
                        std::cout << "[Sent to " << target << "]" << std::endl;
                    } else {
                        std::cerr << "[Error] sendto failed: " << WSAGetLastError() << std::endl;
                    }
                }
            }
        }
        else if (line.find("file ") == 0) {
            size_t space2 = line.find(' ', 5);
            if (space2 == std::string::npos) {
                std::cout << "[Usage] file <id> <path>" << std::endl;
                return;
            }

            const std::string target = line.substr(5, space2 - 5);
            const std::string rawPath = TrimEnclosingQuotes(line.substr(space2 + 1));
            if (target.empty() || rawPath.empty()) {
                std::cout << "[Usage] file <id> <path>" << std::endl;
                return;
            }

            QueueFileSend(target, std::filesystem::path(rawPath));
        }
        else if (line.find("smsg ") == 0) {
            size_t space2 = line.find(' ', 5);
            if (space2 != std::string::npos) {
                std::string target = line.substr(5, space2 - 5);
                std::string text = line.substr(space2 + 1);
                
                signalling.SendRelayMessage(target, text);
                std::cout << "[Server Relay] Sent to " << target << std::endl;
            }
        }
    }

    void Stop() {
        if (!started.exchange(false)) {
            return;
        }

        running = false;
        fileTransferCv.notify_all();
        WaitForFileSendTasks();
        StopScreenShare();
        audio.StopRecording();
        signalling.Disconnect();

        JoinScheduledTasks();
        
        if (worker.joinable()) worker.join();
        if (pingWorker.joinable()) pingWorker.join();
        
        {
            std::lock_guard<std::mutex> lock(peersMtx);
            peers.clear();
        }

        {
            std::lock_guard<std::mutex> lock(addrMapMtx);
            addrToAgent.clear();
        }

        {
            std::lock_guard<std::mutex> lock(onlineUsersMtx);
            onlineUsers.clear();
        }

        {
            std::lock_guard<std::mutex> lock(screenRxMtx);
            screenRxBuffers.clear();
            remoteScreenPreviews.clear();
        }
        {
            std::lock_guard<std::mutex> lock(offerPromptMtx);
            pendingOfferPrompts.clear();
            pendingOfferPeers.clear();
        }

        CleanupFileTransfersOnStop();
        
        if (cp) {
            CloseHandle(cp);
            cp = nullptr;
        }
        
        WSACleanup();
    }

    void RunConsole(const char* serverIp, int serverPort) {
        Start(serverIp, serverPort);
        PrintHelp();

        std::string line;
        while (std::getline(std::cin, line)) {
            ExecuteCommand(line);
        }

        Stop();
    }
};

#ifndef NEKOCHAT_NO_MAIN
int main() {
    std::cout << "Enter your ID: ";
    std::string myId; 
    std::getline(std::cin, myId);

    if (myId.empty()) {
        std::cerr << "ID cannot be empty!" << std::endl;
        return 1;
    }

    P2PClient client;
    client.id = myId;
    client.RunConsole("ENTERYOURIP", 27015);
    return 0;
}
#endif







