#include "IceAgent.hpp"
#include "STUN.hpp"
#include <iphlpapi.h>
#include <random>
#include <chrono>
#include <iostream>
#include <set>
#include <array>
#include <algorithm>
#include <thread>
#include <optional>   

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <immintrin.h>
#define NEKOCHAT_TEXT_SSE2 1
#endif

struct AutoCS {
    CRITICAL_SECTION& cs;
    AutoCS(CRITICAL_SECTION& c) : cs(c) { EnterCriticalSection(&cs); }
    ~AutoCS() { LeaveCriticalSection(&cs); }
};

static bool TryParseIpv4Address(const std::string& ip, uint32_t& outHostOrder) {
    in_addr addr{};
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
        return false;
    }

    outHostOrder = ntohl(addr.s_addr);
    return true;
}

static bool IsPrivateOrSharedIpv4(uint32_t addr) {
    return ((addr & 0xFF000000U) == 0x0A000000U) ||
           ((addr & 0xFFF00000U) == 0xAC100000U) ||
           ((addr & 0xFFFF0000U) == 0xC0A80000U) ||
           ((addr & 0xFFC00000U) == 0x64400000U);
}

static bool IsLinkLocalIpv4(uint32_t addr) {
    return (addr & 0xFFFF0000U) == 0xA9FE0000U;
}

static bool AreLikelySameSubnet(uint32_t a, uint32_t b) {
    return (a & 0xFFFFFF00U) == (b & 0xFFFFFF00U);
}

static uint64_t ComputePairPriority(const Candidate& local, const Candidate& remote) {
    uint32_t major = 0;
    uint32_t localAddr = 0;
    uint32_t remoteAddr = 0;
    const bool localIpOk = TryParseIpv4Address(local.ip, localAddr);
    const bool remoteIpOk = TryParseIpv4Address(remote.ip, remoteAddr);

    if (local.type == HOST && remote.type == HOST) {
        major = 280;
        if (localIpOk && remoteIpOk) {
            if (AreLikelySameSubnet(localAddr, remoteAddr)) {
                major = 360;
            } else if (IsPrivateOrSharedIpv4(localAddr) && IsPrivateOrSharedIpv4(remoteAddr)) {
                major = 330;
            } else if (IsLinkLocalIpv4(localAddr) || IsLinkLocalIpv4(remoteAddr)) {
                major = 180;
            }
        }
    } else if (local.type == SERVER_REFLEXIVE && remote.type == SERVER_REFLEXIVE) {
        major = 340;
    } else if (local.type == HOST || remote.type == HOST) {
        major = 220;
    } else {
        major = 160;
    }

    uint32_t minor = 0;
    minor += (local.type == HOST) ? 20U : 10U;
    minor += (remote.type == HOST) ? 10U : 5U;
    if (localIpOk && remoteIpOk && AreLikelySameSubnet(localAddr, remoteAddr)) {
        minor += 40U;
    }

    return (static_cast<uint64_t>(major) << 32) | static_cast<uint64_t>(minor);
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

IceAgent::IceAgent(HANDLE completionPort)
    : m_completionPort(completionPort)
    , m_maxRetries(3)
    , m_retryTimeoutMs(2000)
    , m_lastActivity(std::chrono::steady_clock::now()) {
    InitializeCriticalSection(&m_cs);
}

IceAgent::~IceAgent() {
    Cancel();
    DeleteCriticalSection(&m_cs);
}

std::vector<Candidate> IceAgent::GetLocalCandidates() {
    AutoCS lock(m_cs);
    std::vector<Candidate> result;
    result.reserve(m_localCandidates.size());
    for (const auto& loc : m_localCandidates) {
        result.push_back(loc.info);
    }
    return result;
}

std::tuple<bool, SOCKET, Candidate> IceAgent::GetActiveConnection() const {
    AutoCS lock(m_cs);
    return std::make_tuple(m_successNotified.load(), m_activeSocket.load(), m_activeRemote);
}

bool IceAgent::IsConnected() const {
    return m_successNotified.load();
}

void IceAgent::UpdateActivity() {
    AutoCS lock(m_cs);
    m_lastActivity = std::chrono::steady_clock::now();
    for (auto& pairPtr : m_pairs) {
        if (pairPtr->state == PAIR_SUCCEEDED || pairPtr->state == PAIR_IN_PROGRESS) {
            pairPtr->lastActivity = m_lastActivity;
        }
    }
}

bool IceAgent::CheckActivityTimeout() {
    AutoCS lock(m_cs);
    if (!m_successNotified.load()) return false;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastActivity).count();
    return elapsed > ACTIVITY_TIMEOUT_MS;
}

void IceAgent::SendPing() {
    auto [success, sock, remote] = GetActiveConnection();
    if (!success || sock == INVALID_SOCKET) return;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, remote.ip.c_str(), &addr.sin_addr);
    addr.sin_port = htons(remote.port);
    char pingPacket = 0xFF;
    sendto(sock, &pingPacket, 1, 0, (sockaddr*)&addr, sizeof(addr));
}

void IceAgent::AddLocalCandidate(const Candidate& cand) {
    SOCKET sock = INVALID_SOCKET;
    bool needStartReceiving = false;
    bool addedPairs = false;

    {
        AutoCS lock(m_cs);

        
        for (const auto& local : m_localCandidates) {
            if (local.info.ip == cand.ip && local.info.port == cand.port) {
                std::cout << "[ICE] Local candidate already added, skipping: "
                          << cand.ip << ":" << cand.port << "\n";
                return;
            }
        }

        bool alreadyReceiving = (m_recvContexts.find(cand.sock) != m_recvContexts.end());
        m_localCandidates.emplace_back(cand);
        sock = cand.sock;
        needStartReceiving = (m_completionPort != NULL && sock != INVALID_SOCKET && !alreadyReceiving);
    }

    if (needStartReceiving) {
        StartReceiving(sock, nullptr);
    }

    if (m_started.load() && !m_successNotified.load()) {
        {
            AutoCS lock(m_cs);
            addedPairs = AddNewPairsIncrementalInternal();
        }

        if (addedPairs) {
            StartNextBatch(2);
        }
    }
}

void IceAgent::AddRemoteCandidate(const Candidate& cand) {
    bool addedPairs = false;

    {
        AutoCS lock(m_cs);

        for (const auto& remote : m_remoteCandidates) {
            if (remote.ip == cand.ip && remote.port == cand.port) {
                std::cout << "[ICE] Remote candidate already added, skipping: "
                          << cand.ip << ":" << cand.port << "\n";
                return;
            }
        }

        m_remoteCandidates.push_back(cand);
        std::cout << "[ICE] Added remote candidate: "
                  << (cand.type == HOST ? "host" : "srflx")
                  << " " << cand.ip << ":" << cand.port << std::endl;

        if (m_started.load() && !m_successNotified.load()) {
            addedPairs = AddNewPairsIncrementalInternal();
        }
    }

    if (addedPairs) {
        StartNextBatch(2);
    }
}

bool IceAgent::AddNewPairsIncrementalInternal() {
    std::set<std::string> existingPairs;
    for (const auto& pPtr : m_pairs) {
        existingPairs.insert(
            pPtr->local.ip + ":" + std::to_string(pPtr->local.port) + "|" +
            pPtr->remote.ip + ":" + std::to_string(pPtr->remote.port)
        );
    }

    size_t added = 0;

    for (const auto& local : m_localCandidates) {
        for (const auto& remote : m_remoteCandidates) {
            std::string pairKey = local.info.ip + ":" + std::to_string(local.info.port) + "|" +
                                  remote.ip + ":" + std::to_string(remote.port);
            if (existingPairs.count(pairKey)) continue;

            auto pair = std::make_shared<CandidatePair>();
            pair->local = local.info;
            pair->remote = remote;
            pair->socket = local.info.sock;
            pair->state = PAIR_WAITING;
            pair->priority = ComputePairPriority(local.info, remote);
            pair->retryCount = 0;
            pair->lastActivity = std::chrono::steady_clock::now();
            m_pairs.push_back(pair);
            existingPairs.insert(pairKey);
            ++added;
        }
    }

    if (added > 0) {
        std::vector<std::shared_ptr<CandidatePair>> sorted = m_pairs;
        std::stable_sort(sorted.begin(), sorted.end(),
            [](const std::shared_ptr<CandidatePair>& a, const std::shared_ptr<CandidatePair>& b) {
                return a->priority > b->priority;
            });
        m_pairs.swap(sorted);
        m_currentPairIndex = 0;
        std::cout << "[ICE] Added " << added << " new pairs incrementally (total: "
                  << m_pairs.size() << ")\n";
        return true;
    }

    return false;
}

bool IceAgent::StartConnectivityChecks(IceCallback callback) {
    m_callback = callback;
    m_successNotified = false;
    m_currentPairIndex = 0;
    m_lastActivity = std::chrono::steady_clock::now();

    {
        AutoCS lock(m_cs);

        if (m_localCandidates.empty() || m_remoteCandidates.empty()) {
            std::cerr << "[ICE] Cannot start checks: local=" << m_localCandidates.size()
                      << " remote=" << m_remoteCandidates.size() << std::endl;
            return false;
        }

        FormPairsInternal();
        m_started = true;
        m_lastCheckTime = std::chrono::steady_clock::now();

        std::cout << "[ICE] Starting connectivity checks with " << m_pairs.size() << " pairs" << std::endl;
    }

    StartNextBatch(4);
    return true;
}

void IceAgent::StartNextBatch(size_t maxParallel) {
    EnterCriticalSection(&m_cs);

    size_t started = 0;
    size_t currentIdx = m_currentPairIndex.load();

    for (size_t i = currentIdx; i < m_pairs.size() && started < maxParallel; ++i) {
        auto& pairPtr = m_pairs[i];

        if (pairPtr->state == PAIR_WAITING || pairPtr->state == PAIR_FAILED) {
            if (pairPtr->state == PAIR_FAILED) {
                pairPtr->retryCount = 0;
            }
            pairPtr->state = PAIR_IN_PROGRESS;
            pairPtr->lastSent = std::chrono::steady_clock::now();
            pairPtr->lastActivity = std::chrono::steady_clock::now();

            LeaveCriticalSection(&m_cs);                 
            bool sent = SendBindingRequest(pairPtr);
            EnterCriticalSection(&m_cs);                

            if (sent) {
                ++started;
            } else {
                pairPtr->state = PAIR_FAILED;
            }
            m_currentPairIndex = i + 1;
        }
    }

    if (started == 0 && m_currentPairIndex >= m_pairs.size()) {
        std::cout << "[ICE] All pairs exhausted, connection failed\n";
        auto cb = m_callback;
        LeaveCriticalSection(&m_cs);                     
        if (cb) cb(false, CandidatePair{});
    } else {
        LeaveCriticalSection(&m_cs);
    }
}

void IceAgent::StartReceiving(SOCKET sock, LocalCandidate*) {
    {
        AutoCS lock(m_cs);
        if (m_recvContexts.find(sock) != m_recvContexts.end()) {
            return;
        }
    }

    auto ctx = std::make_unique<IceUdpContext>(this, sock);

    if (CreateIoCompletionPort((HANDLE)sock, m_completionPort,
                               (ULONG_PTR)ctx.get(), 0) == nullptr) {
        DWORD err = GetLastError();
        if (err != ERROR_INVALID_PARAMETER) {
            std::cerr << "[ICE] Failed to associate UDP socket with IOCP: "
                      << err << std::endl;
            return;
        }
    }

    DWORD recvBytes = 0, flags = 0;
    int rc = WSARecvFrom(sock, &ctx->wsaBuf, 1, &recvBytes, &flags,
                         (sockaddr*)&ctx->remoteAddr, &ctx->remoteAddrLen,
                         &ctx->overlapped, nullptr);

    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        std::cerr << "[ICE] WSARecvFrom failed: " << WSAGetLastError() << std::endl;
        return;
    }

    {
        AutoCS lock(m_cs);
        m_recvContexts[sock] = std::move(ctx);
    }

    std::cout << "[ICE] Started receiving on socket " << sock << std::endl;
}

void IceAgent::OnReceiveCompleted(IceUdpContext* ctx, DWORD bytesTransferred) {
    if (bytesTransferred == 0 || ctx->detached.load(std::memory_order_acquire)) {
        return;
    }

    IceAgent* agent = ctx->agent.load(std::memory_order_acquire);
    if (!agent) return;

    agent->OnUdpPacket(ctx->socket, ctx->remoteAddr, ctx->buffer, bytesTransferred);

   
    if (ctx->detached.load(std::memory_order_acquire)) {
        return;
    }

   
    agent->RearmReceiveContext(ctx);
}

void IceAgent::RearmReceiveContext(IceUdpContext* ctx) {
    if (!ctx || ctx->detached.load(std::memory_order_acquire)) {
        return;
    }

    AutoCS lock(m_cs);
    auto it = m_recvContexts.find(ctx->socket);
    if (it == m_recvContexts.end() || it->second.get() != ctx) {
        return;
    }

    ZeroMemory(&ctx->overlapped, sizeof(OVERLAPPED));
    ctx->wsaBuf.buf = ctx->buffer;
    ctx->wsaBuf.len = sizeof(ctx->buffer);
    ctx->remoteAddrLen = sizeof(ctx->remoteAddr);
    ctx->flags = 0;

    DWORD recvBytes = 0;
    DWORD flags = 0;
    int rc = WSARecvFrom(ctx->socket, &ctx->wsaBuf, 1, &recvBytes, &flags,
                         (sockaddr*)&ctx->remoteAddr, &ctx->remoteAddrLen,
                         &ctx->overlapped, nullptr);

    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        std::cerr << "[ICE] WSARecvFrom re-arm failed: " << WSAGetLastError() << std::endl;
    }
}

void IceAgent::FormPairsInternal() {
    m_pairs.clear();

    std::set<std::string> seenPairs;

    for (const auto& local : m_localCandidates) {
        for (const auto& remote : m_remoteCandidates) {
            std::string pairKey = local.info.ip + ":" + std::to_string(local.info.port) + "|" +
                                  remote.ip + ":" + std::to_string(remote.port);
            if (seenPairs.count(pairKey)) continue;
            seenPairs.insert(pairKey);

            auto pair = std::make_shared<CandidatePair>();
            pair->local = local.info;
            pair->remote = remote;
            pair->socket = local.info.sock;
            pair->state = PAIR_WAITING;
            pair->priority = ComputePairPriority(local.info, remote);
            pair->retryCount = 0;
            pair->lastActivity = std::chrono::steady_clock::now();
            m_pairs.push_back(pair);
        }
    }

    std::sort(m_pairs.begin(), m_pairs.end(),
        [](const std::shared_ptr<CandidatePair>& a, const std::shared_ptr<CandidatePair>& b) {
            return a->priority > b->priority;
        });

    std::cout << "[ICE] Formed " << m_pairs.size()
              << " pairs across host and srflx candidates" << std::endl;
}

void IceAgent::GenerateTransactionId(uint8_t* tid) {
    static thread_local std::mt19937_64 rng(
        std::chrono::steady_clock::now().time_since_epoch().count() +
        std::hash<std::thread::id>{}(std::this_thread::get_id())
    );
    static thread_local std::uniform_int_distribution<uint64_t> dist;

    uint64_t v1 = dist(rng), v2 = dist(rng);
    memcpy(tid, &v1, 8);
    memcpy(tid + 8, &v2, 4);
}

bool IceAgent::SendBindingRequest(std::shared_ptr<CandidatePair> pair) {
    if (pair->socket == INVALID_SOCKET) {
        std::cerr << "[ICE] SendBindingRequest: invalid socket\n";
        return false;
    }

  
    {
        AutoCS lock(m_cs);
        if (pair->activeTid.has_value()) {
            PendingKey oldKey{pair->socket, pair->activeTid.value()};
            m_pendingRequests.erase(oldKey);
            pair->activeTid.reset();
        }
    }

    StunHeader header{};
    header.type = htons(BINDING_REQUEST);
    header.length = 0;
    header.magicCookie = htonl(0x2112A442);

    uint8_t tid[12];
    GenerateTransactionId(tid);
    memcpy(header.transactionId, tid, 12);

    sockaddr_in remoteAddr{};
    remoteAddr.sin_family = AF_INET;
    if (inet_pton(AF_INET, pair->remote.ip.c_str(), &remoteAddr.sin_addr) != 1) {
        std::cerr << "[ICE] Invalid remote IP: " << pair->remote.ip << "\n";
        return false;
    }
    remoteAddr.sin_port = htons(pair->remote.port);

    std::cout << "[ICE] Sending STUN Binding Request to "
              << pair->remote.ip << ":" << pair->remote.port
              << " from socket " << pair->socket << "\n";

    int sent = sendto(pair->socket, reinterpret_cast<const char*>(&header),
                      sizeof(header), 0, (sockaddr*)&remoteAddr, sizeof(remoteAddr));

    if (sent == SOCKET_ERROR) {
        std::cerr << "[ICE] sendto failed: " << WSAGetLastError() << "\n";
        return false;
    }

    TidKey tidKey;
    memcpy(tidKey.data.data(), tid, 12);

    {
        AutoCS lock(m_cs);
        PendingKey key{pair->socket, tidKey};
        m_pendingRequests[key] = pair;   
        pair->activeTid = tidKey;        
    }

    return true;
}

void IceAgent::CheckPairTimeouts() {
    auto now = std::chrono::steady_clock::now();
    bool needMorePairs = false;

    EnterCriticalSection(&m_cs);

    for (auto& pairPtr : m_pairs) {
        if (pairPtr->state != PAIR_IN_PROGRESS) continue;

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - pairPtr->lastSent).count();

        if (elapsed > m_retryTimeoutMs) {
            if (pairPtr->retryCount < m_maxRetries) {
                ++pairPtr->retryCount;
                pairPtr->lastSent = now;
                std::cout << "[ICE] Retrying pair (attempt " << pairPtr->retryCount
                          << "/" << m_maxRetries << "): "
                          << pairPtr->local.ip << ":" << pairPtr->local.port << "\n";

             
                if (pairPtr->activeTid.has_value()) {
                    PendingKey oldKey{pairPtr->socket, pairPtr->activeTid.value()};
                    m_pendingRequests.erase(oldKey);
                    pairPtr->activeTid.reset();
                }

                LeaveCriticalSection(&m_cs);
                SendBindingRequest(pairPtr);
                EnterCriticalSection(&m_cs);
            } else {
                pairPtr->state = PAIR_FAILED;
                std::cout << "[ICE] Pair failed after " << m_maxRetries << " attempts: "
                          << pairPtr->local.ip << ":" << pairPtr->local.port << "\n";
                needMorePairs = true;
            }
        }
    }

    LeaveCriticalSection(&m_cs);

    if (needMorePairs) {
        StartNextBatch(2);
    }
}

void IceAgent::OnUdpPacket(SOCKET socket, const sockaddr_in& from, const char* data, int len) {
    if (len < 1) return;

    char fromStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from.sin_addr, fromStr, sizeof(fromStr));
    uint16_t fromPort = ntohs(from.sin_port);

    bool isStun = (len >= 20 &&
                   ntohl(*reinterpret_cast<const uint32_t*>(data + 4)) == 0x2112A442);

    if (!isStun) {
        UpdateActivity();

        std::string currentPeerId;
        bool found = false;

        {
            AutoCS lock(m_cs);
            currentPeerId = peerId;
            for (const auto& r : m_remoteCandidates) {
                if (r.ip == fromStr && r.port == fromPort) {
                    found = true;
                    break;
                }
            }
        }

        bool allowFromUnknown = false;
        {
            AutoCS lock(m_cs);
            allowFromUnknown = m_remoteCandidates.empty();
        }

        if (!allowFromUnknown && !found) {
            return;
        }

        uint8_t pType = static_cast<uint8_t>(data[0]);
        const char* payload = data + 1;
        int payloadLen = len - 1;

        if (pType == 0xFF) {
            return;
        }
        else if (pType == 0x01) {
            std::string msg(payload, payloadLen);
            ApplyXor55(msg.data(), msg.size());
            std::cout << "\n[" << (currentPeerId.empty() ? fromStr : currentPeerId)
                      << "]: " << msg << std::endl;
        }
        else if (pType == 0x02) {
            if (onAudioReceived) {
                onAudioReceived((void*)payload, payloadLen);
            }
        }
        else if (pType == 0x03) {
            if (onScreenChunkReceived) {
                onScreenChunkReceived(payload, payloadLen);
            }
        }
        else if (pType == 0x04) {
            if (onFilePacketReceived) {
                onFilePacketReceived(payload, payloadLen);
            }
        }
        return;
    }

    const StunHeader* header = reinterpret_cast<const StunHeader*>(data);
    uint16_t type = ntohs(header->type);

    if (type == BINDING_REQUEST) {
        SendStunResponse(socket, from, header->transactionId);
    }
    else if (type == BINDING_RESPONSE) {
        HandleStunResponseInternal(data, len, from, socket);
    }
}

void IceAgent::HandleStunResponseInternal(const char* data, int len,
                                         const sockaddr_in& from, SOCKET recvSocket) {
    EnterCriticalSection(&m_cs);

    std::cout << "[ICE] Got STUN response from: " << inet_ntoa(from.sin_addr)
              << ":" << ntohs(from.sin_port) << "\n";

    const StunHeader* header = reinterpret_cast<const StunHeader*>(data);
    uint16_t type = ntohs(header->type);
    if (type != BINDING_RESPONSE) {
        std::cout << "[ICE] Not a BINDING_RESPONSE\n";
        LeaveCriticalSection(&m_cs);
        return;
    }

    TidKey tidKey;
    memcpy(tidKey.data.data(), header->transactionId, 12);

    std::cout << "[ICE] Looking for TID in pending requests...\n";

    std::shared_ptr<CandidatePair> matchedPair;
    PendingKey searchKey{recvSocket, tidKey};
    auto it = m_pendingRequests.find(searchKey);
    if (it != m_pendingRequests.end()) {
        matchedPair = it->second.lock();  
        std::cout << "[ICE] Matched pending request for pair: "
                  << matchedPair->local.ip << ":" << matchedPair->local.port
                  << " -> " << matchedPair->remote.ip << ":" << matchedPair->remote.port << "\n";
      
        m_pendingRequests.erase(it);
        if (matchedPair) {
            matchedPair->activeTid.reset(); 
        }
    }

    if (!matchedPair) {
        std::cout << "[ICE] No matching request for this TID\n";
        LeaveCriticalSection(&m_cs);
        return;
    }

    uint16_t attrLen = ntohs(header->length);
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data + sizeof(StunHeader));
    const uint8_t* end = ptr + attrLen;
    bool foundAddress = false;

    while (ptr + 4 <= end) {
        uint16_t attrType = ntohs(*reinterpret_cast<const uint16_t*>(ptr));
        uint16_t attrLength = ntohs(*reinterpret_cast<const uint16_t*>(ptr + 2));
        const uint8_t* value = ptr + 4;

        if (attrType == XOR_MAPPED_ADDRESS && attrLength >= 8 && value[1] == 0x01) {
            constexpr uint32_t MAGIC = 0x2112A442;

            uint16_t xorPort;
            memcpy(&xorPort, value + 2, 2);
            uint16_t realPort = ntohs(xorPort) ^ static_cast<uint16_t>(MAGIC >> 16);

            uint32_t xorAddr;
            memcpy(&xorAddr, value + 4, 4);
            [[maybe_unused]] uint32_t realAddr = ntohl(xorAddr) ^ MAGIC;

            foundAddress = true;
            break;
        }
        ptr += 4 + ((attrLength + 3) & ~3);
    }

    if (foundAddress && !m_successNotified.load()) {
        auto localPair = *matchedPair;   

        m_successNotified = true;
        m_activeRemote = matchedPair->remote;
        m_activeSocket = matchedPair->socket;
        m_lastActivity = std::chrono::steady_clock::now();
        m_pendingRequests.clear();       

        for (auto& pPtr : m_pairs) {
            if (pPtr->state == PAIR_IN_PROGRESS) pPtr->state = PAIR_SUCCEEDED;
        }

        auto localCb = m_callback;
        LeaveCriticalSection(&m_cs);

        if (localCb) {
            localCb(true, localPair);
        }
    } else {
        LeaveCriticalSection(&m_cs);
    }
}

void IceAgent::SendStunResponse(SOCKET sock, const sockaddr_in& from, const uint8_t* tid) {
    constexpr uint32_t MAGIC = 0x2112A442;
    char response[sizeof(StunHeader) + 12];

    auto* respHeader = reinterpret_cast<StunHeader*>(response);
    respHeader->type = htons(BINDING_RESPONSE);
    respHeader->length = htons(12);
    respHeader->magicCookie = htonl(MAGIC);
    memcpy(respHeader->transactionId, tid, 12);

    uint8_t* attrPtr = reinterpret_cast<uint8_t*>(response + sizeof(StunHeader));
    *reinterpret_cast<uint16_t*>(attrPtr + 0) = htons(XOR_MAPPED_ADDRESS);
    *reinterpret_cast<uint16_t*>(attrPtr + 2) = htons(8);

    uint8_t* valuePtr = attrPtr + 4;
    valuePtr[0] = 0;
    valuePtr[1] = 0x01;

    uint16_t hostPort = ntohs(from.sin_port);
    uint16_t xorPortHost = hostPort ^ static_cast<uint16_t>(MAGIC >> 16);
    uint16_t xorPortNet = htons(xorPortHost);
    memcpy(valuePtr + 2, &xorPortNet, 2);

    uint32_t xorAddr = from.sin_addr.s_addr ^ htonl(MAGIC);
    memcpy(valuePtr + 4, &xorAddr, 4);

    sendto(sock, response, sizeof(response), 0, (sockaddr*)&from, sizeof(from));
}

void IceAgent::SetPairSucceeded(const CandidatePair& pair) {
    AutoCS lock(m_cs);

    if (m_successNotified.load()) return;

    m_successNotified = true;
    m_activeRemote = pair.remote;
    m_activeSocket = pair.socket;
    m_lastActivity = std::chrono::steady_clock::now();
    m_pendingRequests.clear();

    for (auto& pPtr : m_pairs) {
        if (pPtr->state == PAIR_IN_PROGRESS) pPtr->state = PAIR_SUCCEEDED;
    }
}

void IceAgent::Cancel() {
    {
        AutoCS lock(m_cs);
        for (auto& [sock, ctx] : m_recvContexts) {
            ctx->detached.store(true, std::memory_order_release);
            ctx->agent.store(nullptr, std::memory_order_release);
        }
    }

    {
        AutoCS lock(m_cs);
        for (auto& [sock, ctx] : m_recvContexts) {
            if (sock != INVALID_SOCKET) {
                CancelIoEx((HANDLE)sock, &ctx->overlapped);
            }
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    {
        AutoCS lock(m_cs);
        m_recvContexts.clear();
        m_localCandidates.clear();
        m_remoteCandidates.clear();
        m_pairs.clear();
        m_pendingRequests.clear();
    }

    m_started = false;
    m_successNotified = false;
    m_activeSocket = INVALID_SOCKET;
}

void IceAgent::Process() {
    if (!m_started.load() || m_successNotified.load()) return;

    CheckPairTimeouts();

    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastCheckTime).count() > 500) {
        m_lastCheckTime = now;
    }
}
