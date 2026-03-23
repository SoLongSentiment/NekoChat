#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <chrono>
#include <random>
#include <memory>
#include <optional>
#include <atomic>
#include <iostream>
#include <tuple>

#include "CandidateGatherer.hpp"

#pragma comment(lib, "winmm.lib")

enum PairState {
    PAIR_WAITING,
    PAIR_IN_PROGRESS,
    PAIR_SUCCEEDED,
    PAIR_FAILED
};

struct TidKey {
    std::array<uint8_t, 12> data{};
    bool operator==(const TidKey& other) const { return data == other.data; }
};

struct CandidatePair {
    Candidate local;
    Candidate remote;
    SOCKET socket;
    PairState state;
    uint64_t priority;
    std::chrono::steady_clock::time_point lastSent;
    int retryCount;
    std::chrono::steady_clock::time_point lastActivity;
    std::optional<TidKey> activeTid;

    CandidatePair() : socket(INVALID_SOCKET), state(PAIR_WAITING), priority(0), retryCount(0) {}
};

struct TidKeyHash {
    size_t operator()(const TidKey& key) const {
        uint64_t h1;
        uint32_t h2;
        memcpy(&h1, key.data.data(), 8);
        memcpy(&h2, key.data.data() + 8, 4);
        return std::hash<uint64_t>{}(h1) ^ (std::hash<uint32_t>{}(h2) << 1);
    }
};

struct PendingKey {
    SOCKET socket;
    TidKey tid;
    bool operator==(const PendingKey& other) const { 
        return socket == other.socket && tid == other.tid; 
    }
};

struct PendingKeyHash {
    size_t operator()(const PendingKey& key) const {
        return std::hash<SOCKET>{}(key.socket) ^ TidKeyHash{}(key.tid);
    }
};

class IceAgent {
public:
    using IceCallback = std::function<void(bool success, const CandidatePair& pair)>;
    using AudioFrameCallback = std::function<void(void* data, int len)>;
    using ScreenChunkCallback = std::function<void(const void* data, int len)>;
    using FilePacketCallback = std::function<void(const void* data, int len)>;

    AudioFrameCallback onAudioReceived;
    ScreenChunkCallback onScreenChunkReceived;
    FilePacketCallback onFilePacketReceived;
    std::string peerId;

    explicit IceAgent(HANDLE completionPort);
    ~IceAgent();

    void AddLocalCandidate(const Candidate& cand);
    void AddRemoteCandidate(const Candidate& cand);
    bool StartConnectivityChecks(IceCallback callback);
    void OnUdpPacket(SOCKET socket, const sockaddr_in& from, const char* data, int len);
    void Process();
    void Cancel();
    std::vector<Candidate> GetLocalCandidates();

    std::tuple<bool, SOCKET, Candidate> GetActiveConnection() const;
    bool IsConnected() const;
    
    void SendPing();
    bool CheckActivityTimeout();
    void UpdateActivity();



    struct LocalCandidate {
        Candidate info;
        explicit LocalCandidate(const Candidate& c) : info(c) {}
    };

    struct IceUdpContext {
        static const DWORD SIGNATURE = 0x55445000;
        DWORD signature;
        OVERLAPPED overlapped;
        WSABUF wsaBuf;
        char buffer[1500];
        sockaddr_in remoteAddr{};
        int remoteAddrLen;
        DWORD flags;
        SOCKET socket;
        std::atomic<IceAgent*> agent;
        LocalCandidate* localCandidate;
        std::atomic<bool> detached;

        IceUdpContext(IceAgent* a, SOCKET s, LocalCandidate* lc = nullptr)
            : signature(SIGNATURE), agent(a), socket(s), flags(0),
              remoteAddrLen(sizeof(remoteAddr)), localCandidate(lc), detached(false) {
            ZeroMemory(&overlapped, sizeof(overlapped));
            wsaBuf.buf = buffer;
            wsaBuf.len = sizeof(buffer);
        }
    };

    void StartReceiving(SOCKET sock, LocalCandidate* localCandidate);
    void OnReceiveCompleted(IceUdpContext* ctx, DWORD bytesTransferred);
    void RearmReceiveContext(IceUdpContext* ctx);
    void FormPairsInternal();
    bool AddNewPairsIncrementalInternal();
    bool SendBindingRequest(std::shared_ptr<CandidatePair> pair);
    void SendStunResponse(SOCKET sock, const sockaddr_in& from, const uint8_t* tid);
    void HandleStunResponseInternal(const char* data, int len, const sockaddr_in& from, SOCKET recvSocket);
    void SetPairSucceeded(const CandidatePair& pair);
    void GenerateTransactionId(uint8_t* tid);
    void CheckPairTimeouts();
    void StartNextBatch(size_t maxParallel = 4);

    using DisconnectCallback = std::function<void()>;
    DisconnectCallback onDisconnected;

private:
    HANDLE m_completionPort;
    std::vector<LocalCandidate> m_localCandidates;
    std::vector<Candidate> m_remoteCandidates;
    std::vector<std::shared_ptr<CandidatePair>> m_pairs;
    std::unordered_map<PendingKey, std::weak_ptr<CandidatePair>, PendingKeyHash> m_pendingRequests;
    std::map<SOCKET, std::unique_ptr<IceUdpContext>> m_recvContexts;

    
    mutable CRITICAL_SECTION m_cs;

    std::atomic<bool> m_successNotified{false};
    std::atomic<SOCKET> m_activeSocket{INVALID_SOCKET};
    Candidate m_activeRemote;
    std::chrono::steady_clock::time_point m_lastActivity;

    std::atomic<bool> m_started{false};
    std::atomic<size_t> m_currentPairIndex{0};
    IceCallback m_callback;
    std::chrono::steady_clock::time_point m_lastCheckTime;
    int m_maxRetries;
    int m_retryTimeoutMs;
    static constexpr int ACTIVITY_TIMEOUT_MS = 60000;


};
