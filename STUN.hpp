#pragma once
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>


#pragma comment(lib, "ws2_32.lib")

// STUN message types (RFC 5389)
enum StunMessageType : uint16_t
{
    BINDING_REQUEST = 0x0001,
    BINDING_RESPONSE = 0x0101,
    BINDING_ERROR_RESPONSE = 0x0111,
};

// STUN attribute types
enum StunAttributeType : uint16_t
{
    MAPPED_ADDRESS = 0x0001,
    XOR_MAPPED_ADDRESS = 0x0020,
    ERROR_CODE = 0x0009,
    UNKNOWN_ATTRIBUTES = 0x000A,
    SOFTWARE = 0x8022,
    FINGERPRINT = 0x8028,
};

// STUN header (20 bytes, packed)
#pragma pack(push, 1)
struct StunHeader
{
    uint16_t type;
    uint16_t length;
    uint32_t magicCookie; // 0x2112A442
    uint8_t transactionId[12];
};
#pragma pack(pop)

#pragma pack(push, 1)
struct StunAttribute
{
    uint16_t type;
    uint16_t length;
    // values
};
#pragma pack(pop)

class StunClient
{
  public:
    static bool GetReflexiveAddress(const char *stunHost, uint16_t stunPort,
                                    SOCKET udpSocket,
                                    sockaddr_in &reflexiveAddr)
    {
        sockaddr_in serverAddr;
        if (!ResolveHost(stunHost, stunPort, serverAddr))
            return false;

        std::vector<uint8_t> transactionId(12);
        std::mt19937 rng(static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        std::uniform_int_distribution<int> dist(0, 255);
        for (int i = 0; i < 12; ++i)
            transactionId[i] = static_cast<uint8_t>(dist(rng));

        std::vector<uint8_t> request =
            BuildBindingRequest(transactionId.data());

        int sent =
            sendto(udpSocket, reinterpret_cast<const char *>(request.data()),
                   static_cast<int>(request.size()), 0, (sockaddr *)&serverAddr,
                   sizeof(serverAddr));
        if (sent == SOCKET_ERROR)
        {
            std::cerr << "[STUN] sendto failed: " << WSAGetLastError()
                      << std::endl;
            return false;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(udpSocket, &readfds);
        timeval tv{2, 0};
        if (select(0, &readfds, nullptr, nullptr, &tv) <= 0)
        {
            std::cerr << "[STUN] Response timeout" << std::endl;
            return false;
        }

        char buffer[1500];
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int recvLen = recvfrom(udpSocket, buffer, sizeof(buffer), 0,
                               (sockaddr *)&from, &fromLen);
        if (recvLen == SOCKET_ERROR)
        {
            std::cerr << "[STUN] recvfrom failed: " << WSAGetLastError()
                      << std::endl;
            return false;
        }

        return ParseBindingResponse(buffer, recvLen, transactionId.data(),
                                    reflexiveAddr);
    }

    static bool ResolveHost(const char *host, uint16_t port, sockaddr_in &addr)
    {
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        if (inet_pton(AF_INET, host, &addr.sin_addr) == 1)
            return true;

        hostent *he = gethostbyname(host);
        if (!he || he->h_addrtype != AF_INET)
            return false;

        memcpy(&addr.sin_addr, he->h_addr, he->h_length);
        return true;
    }

  private:
    static std::vector<uint8_t>
    BuildBindingRequest(const uint8_t *transactionId)
    {
        StunHeader header{};
        header.type = htons(BINDING_REQUEST);
        header.length = 0;
        header.magicCookie = htonl(0x2112A442);
        memcpy(header.transactionId, transactionId, 12);

        std::vector<uint8_t> result(sizeof(StunHeader));
        memcpy(result.data(), &header, sizeof(StunHeader));
        return result;
    }

    static bool ParseBindingResponse(const char *buffer, int len,
                                     const uint8_t *expectedTid,
                                     sockaddr_in &reflexiveAddr)
    {
        if (len < static_cast<int>(sizeof(StunHeader)))
            return false;

        const StunHeader *header = reinterpret_cast<const StunHeader *>(buffer);
        if (ntohs(header->type) != BINDING_RESPONSE)
            return false;
        if (ntohl(header->magicCookie) != 0x2112A442)
            return false;
        if (memcmp(header->transactionId, expectedTid, 12) != 0)
            return false;

        uint16_t attrLen = ntohs(header->length);
        const uint8_t *ptr =
            reinterpret_cast<const uint8_t *>(buffer + sizeof(StunHeader));
        const uint8_t *end = ptr + attrLen;

        while (ptr + 4 <= end)
        {
            const StunAttribute *attr =
                reinterpret_cast<const StunAttribute *>(ptr);
            uint16_t type = ntohs(attr->type);
            uint16_t length = ntohs(attr->length);
            const uint8_t *value = ptr + 4;

            if (type == XOR_MAPPED_ADDRESS && length >= 8)
            {
                if (value[1] != 0x01)
                {
                    ptr += 4 + ((length + 3) & ~3);
                    continue;
                }

                constexpr uint32_t MAGIC = 0x2112A442;

                uint16_t xorPort;
                memcpy(&xorPort, value + 2, 2);

                uint16_t realPort =
                    xorPort ^ htons(static_cast<uint16_t>(MAGIC >> 16));

                uint32_t xorAddr;
                memcpy(&xorAddr, value + 4, 4);
                uint32_t realAddr = xorAddr ^ htonl(MAGIC);

                reflexiveAddr.sin_family = AF_INET;
                reflexiveAddr.sin_port = realPort;
                reflexiveAddr.sin_addr.s_addr = realAddr;
                return true;
            }

            else if (type == MAPPED_ADDRESS && length >= 8)
            {
                if (value[1] != 0x01)
                {
                    ptr += 4 + ((length + 3) & ~3);
                    continue;
                }
                uint16_t port;
                uint32_t addr;
                memcpy(&port, value + 2, 2);
                memcpy(&addr, value + 4, 4);
                reflexiveAddr.sin_family = AF_INET;
                reflexiveAddr.sin_port = port;
                reflexiveAddr.sin_addr.s_addr = addr;
                return true;
            }

            ptr += 4 + ((length + 3) & ~3);
        }
        return false;
    }
};