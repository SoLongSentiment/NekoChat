#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <vector>
#include <memory>
#include <functional>
#include <string>
#include <iostream>
#include <chrono>
#include <cstring>

#include "STUN.hpp"

#pragma comment(lib, "iphlpapi.lib")

enum CandidateType
{
    HOST,
    SERVER_REFLEXIVE
};

struct Candidate
{
    CandidateType type;
    std::string ip;
    uint16_t port;
    SOCKET sock;
    std::string relatedIp;
    uint16_t relatedPort;

    Candidate() : type(HOST), port(0), relatedPort(0), sock(INVALID_SOCKET)
    {
    }
};

class CandidateGatherer
{
  public:
    using CandidateCallback =
        std::function<void(std::vector<Candidate> candidates)>;

    CandidateGatherer()
    {
        InitializeCriticalSection(&m_cs);
    }

    ~CandidateGatherer()
    {
        CloseCurrentSocket();
        DeleteCriticalSection(&m_cs);
    }

    bool StartGathering(const char *stunHost, uint16_t stunPort,
                        CandidateCallback callback)
    {
        {
            CSGuard guard(m_cs);
            if (m_gatherSocket != INVALID_SOCKET && !m_cachedCandidates.empty())
            {
                std::vector<Candidate> cacheCopy = m_cachedCandidates;
                guard.Leave();
                if (callback)
                {
                    callback(cacheCopy);
                }
                return true;
            }
        }

        CloseCurrentSocket();

        SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == INVALID_SOCKET)
        {
            if (callback)
            {
                callback({});
            }
            return false;
        }

        int udpBufferSize = 256 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char *>(&udpBufferSize),
                   sizeof(udpBufferSize));
        setsockopt(sock, SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char *>(&udpBufferSize),
                   sizeof(udpBufferSize));

        sockaddr_in localAddr{};
        localAddr.sin_family = AF_INET;
        localAddr.sin_addr.s_addr = INADDR_ANY;
        localAddr.sin_port = 0;

        if (bind(sock, reinterpret_cast<sockaddr *>(&localAddr),
                 sizeof(localAddr)) == SOCKET_ERROR)
        {
            std::cerr << "[Gatherer] Bind failed: " << WSAGetLastError()
                      << std::endl;
            closesocket(sock);
            if (callback)
            {
                callback({});
            }
            return false;
        }

        int len = sizeof(localAddr);
        if (getsockname(sock, reinterpret_cast<sockaddr *>(&localAddr), &len) ==
            SOCKET_ERROR)
        {
            std::cerr << "[Gatherer] getsockname failed: " << WSAGetLastError()
                      << std::endl;
            closesocket(sock);
            if (callback)
            {
                callback({});
            }
            return false;
        }

        char hostIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &localAddr.sin_addr, hostIp, sizeof(hostIp));
        uint16_t hostPort = ntohs(localAddr.sin_port);

        std::string finalHostIp = hostIp;
        if (finalHostIp == "0.0.0.0" || std::strlen(hostIp) == 0)
        {
            auto ips = GetLocalIPv4Addresses();
            if (!ips.empty())
            {
                finalHostIp = ips[0];
            }
        }

        std::vector<Candidate> candidates;
        candidates.reserve(2);

        {
            Candidate cand;
            cand.type = HOST;
            cand.ip = finalHostIp;
            cand.port = hostPort;
            cand.sock = sock;
            candidates.push_back(cand);
        }
        std::cout << "[Gatherer] Host candidate: " << finalHostIp << ":"
                  << hostPort << std::endl;

        sockaddr_in reflexiveAddr{};
        bool stunOk = StunClient::GetReflexiveAddress(stunHost, stunPort, sock,
                                                      reflexiveAddr);

        if (stunOk && reflexiveAddr.sin_addr.s_addr != INADDR_ANY &&
            reflexiveAddr.sin_addr.s_addr != 0)
        {
            char srflxIp[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &reflexiveAddr.sin_addr, srflxIp,
                          sizeof(srflxIp)))
            {
                uint16_t srflxPort = ntohs(reflexiveAddr.sin_port);

                Candidate cand;
                cand.type = SERVER_REFLEXIVE;
                cand.ip = srflxIp;
                cand.port = srflxPort;
                cand.sock = sock;
                cand.relatedIp = finalHostIp;
                cand.relatedPort = hostPort;
                candidates.push_back(cand);

                std::cout << "[Gatherer] SRFLX candidate found: " << srflxIp
                          << ":" << srflxPort << std::endl;
            }
        }
        else
        {
            std::cerr << "[Gatherer] STUN failed or timed out. Only local P2P "
                         "might work.\n";
        }

        {
            CSGuard guard(m_cs);
            m_gatherSocket = sock;
            m_candidates = candidates;
            m_cachedCandidates = candidates;
        }

        if (callback)
        {
            callback(candidates);
        }
        return true;
    }

    std::vector<Candidate> GetCachedCandidates()
    {
        CSGuard guard(m_cs);
        return m_cachedCandidates;
    }

  private:
    struct CSGuard
    {
        CRITICAL_SECTION &cs;
        bool locked;

        explicit CSGuard(CRITICAL_SECTION &c) : cs(c), locked(true)
        {
            EnterCriticalSection(&cs);
        }

        void Leave()
        {
            if (locked)
            {
                LeaveCriticalSection(&cs);
                locked = false;
            }
        }

        ~CSGuard()
        {
            if (locked)
            {
                LeaveCriticalSection(&cs);
            }
        }
    };

    void CloseCurrentSocket()
    {
        CSGuard guard(m_cs);
        if (m_gatherSocket != INVALID_SOCKET)
        {
            closesocket(m_gatherSocket);
            m_gatherSocket = INVALID_SOCKET;
        }
    }

    std::vector<std::string> GetLocalIPv4Addresses(bool includePrivate = true)
    {
        std::vector<std::string> ips;
        DWORD size = 0;

        if (GetAdaptersAddresses(AF_INET, 0, nullptr, nullptr, &size) !=
            ERROR_BUFFER_OVERFLOW)
        {
            return ips;
        }

        std::vector<uint8_t> buffer(size);
        PIP_ADAPTER_ADDRESSES adapters =
            reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

        if (GetAdaptersAddresses(AF_INET, 0, nullptr, adapters, &size) !=
            NO_ERROR)
        {
            return ips;
        }

        for (PIP_ADAPTER_ADDRESSES a = adapters; a; a = a->Next)
        {
            if (a->OperStatus != IfOperStatusUp ||
                a->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
                a->IfType == IF_TYPE_TUNNEL)
            {
                continue;
            }

            for (PIP_ADAPTER_UNICAST_ADDRESS u = a->FirstUnicastAddress; u;
                 u = u->Next)
            {
                auto *addr =
                    reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr);
                char str[INET_ADDRSTRLEN];

                if (inet_ntop(AF_INET, &addr->sin_addr, str, sizeof(str)) ==
                    nullptr)
                {
                    continue;
                }

                std::string ip(str);

                if (!includePrivate)
                {
                    uint32_t addrVal = ntohl(addr->sin_addr.s_addr);
                    if ((addrVal & 0xFF000000) == 0x0A000000 ||
                        (addrVal & 0xFFF00000) == 0xAC100000 ||
                        (addrVal & 0xFFFF0000) == 0xC0A80000 ||
                        (addrVal & 0xFF000000) == 0x7F000000 ||
                        (addrVal & 0xFFFF0000) == 0xA9FE0000)
                    {
                        continue;
                    }
                }

                ips.push_back(ip);
            }
        }

        return ips;
    }

    CRITICAL_SECTION m_cs;
    std::vector<Candidate> m_candidates;
    std::vector<Candidate> m_cachedCandidates;
    SOCKET m_gatherSocket{INVALID_SOCKET};
};
