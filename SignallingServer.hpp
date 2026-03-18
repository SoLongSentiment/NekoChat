
#pragma once

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

constexpr int DEFAULT_PORT = 27015;
constexpr int MAX_BUFFER_SIZE = 4096;

struct ClientContext
{
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in address{};
    std::string clientId;
    bool isRegistered = false;
    std::atomic<bool> active{true};
    std::mutex sendMutex;
    std::string recvBuffer;

    ~ClientContext()
    {
        if (socket != INVALID_SOCKET)
        {
            closesocket(socket);
        }
    }
};

class SignallingServer
{
  public:
    explicit SignallingServer(uint16_t port = DEFAULT_PORT) : m_port(port)
    {
    }

    ~SignallingServer()
    {
        Stop();
    }

    bool Start()
    {
        bool expected = false;
        if (!m_running.compare_exchange_strong(expected, true))
        {
            return false;
        }

        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        {
            logError("WSAStartup failed");
            m_running = false;
            return false;
        }
        m_wsaStarted = true;

        m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listenSocket == INVALID_SOCKET)
        {
            logError("socket failed");
            cleanup();
            m_running = false;
            return false;
        }

        BOOL reuseAddr = TRUE;
        setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char *>(&reuseAddr),
                   sizeof(reuseAddr));

        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        serverAddr.sin_port = htons(m_port);

        if (bind(m_listenSocket, reinterpret_cast<sockaddr *>(&serverAddr),
                 sizeof(serverAddr)) == SOCKET_ERROR)
        {
            logError("bind failed");
            cleanup();
            m_running = false;
            return false;
        }

        if (listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR)
        {
            logError("listen failed");
            cleanup();
            m_running = false;
            return false;
        }

        std::cout << "[SignallingServer] Started on port " << m_port
                  << std::endl;
        return true;
    }

    void Stop()
    {
        bool expected = true;
        if (!m_running.compare_exchange_strong(expected, false))
        {
            return;
        }

        if (m_listenSocket != INVALID_SOCKET)
        {
            shutdown(m_listenSocket, SD_BOTH);
            closesocket(m_listenSocket);
            m_listenSocket = INVALID_SOCKET;
        }

        std::vector<std::shared_ptr<ClientContext>> clientsCopy;
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            clientsCopy = m_clients;
        }

        for (const auto &client : clientsCopy)
        {
            if (!client)
            {
                continue;
            }

            client->active = false;
            SOCKET sock = client->socket;
            client->socket = INVALID_SOCKET;
            if (sock != INVALID_SOCKET)
            {
                shutdown(sock, SD_BOTH);
                closesocket(sock);
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            m_idToClient.clear();
            m_clients.clear();
        }

        cleanup();
        std::cout << "[SignallingServer] Stopped" << std::endl;
    }

    void Run()
    {
        if (!m_running.load())
        {
            return;
        }

        while (m_running.load())
        {
            sockaddr_in clientAddr{};
            int addrLen = sizeof(clientAddr);
            SOCKET clientSocket =
                accept(m_listenSocket,
                       reinterpret_cast<sockaddr *>(&clientAddr), &addrLen);

            if (clientSocket == INVALID_SOCKET)
            {
                if (!m_running.load())
                {
                    break;
                }

                const int err = WSAGetLastError();
                if (err != WSAEINTR && err != WSAENOTSOCK &&
                    err != WSAESHUTDOWN)
                {
                    logError("accept failed: " + std::to_string(err));
                }
                continue;
            }

            auto client = std::make_shared<ClientContext>();
            client->socket = clientSocket;
            client->address = clientAddr;

            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                m_clients.push_back(client);
            }

            std::thread([this, client]() { ClientLoop(client); }).detach();

            std::cout << "[SignallingServer] New client connected" << std::endl;
        }
    }

  private:
    void ClientLoop(const std::shared_ptr<ClientContext> &client)
    {
        char buffer[MAX_BUFFER_SIZE];

        while (m_running.load() && client && client->active.load())
        {
            int len = recv(client->socket, buffer, sizeof(buffer), 0);
            if (len <= 0)
            {
                break;
            }

            client->recvBuffer.append(buffer, len);

            size_t pos = std::string::npos;
            while ((pos = client->recvBuffer.find('\n')) != std::string::npos)
            {
                std::string cmd = client->recvBuffer.substr(0, pos);
                client->recvBuffer.erase(0, pos + 1);

                if (!cmd.empty() && cmd.back() == '\r')
                {
                    cmd.pop_back();
                }

                if (!cmd.empty())
                {
                    processCommand(client, cmd);
                }
            }
        }

        removeClient(client);
    }

    void processCommand(const std::shared_ptr<ClientContext> &client,
                        const std::string &cmd)
    {
        if (cmd.compare(0, 9, "REGISTER ") == 0)
        {
            std::string id = cmd.substr(9);
            if (id.empty() || id.find(' ') != std::string::npos)
            {
                sendLine(client, "ERROR Invalid ID\n");
                return;
            }

            bool idTaken = false;
            bool sameRegistration = false;
            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                auto existing = m_idToClient.find(id);
                if (existing != m_idToClient.end() &&
                    existing->second.get() != client.get())
                {
                    idTaken = true;
                }
                else
                {
                    sameRegistration =
                        (client->isRegistered && client->clientId == id);

                    if (client->isRegistered && client->clientId != id)
                    {
                        auto old = m_idToClient.find(client->clientId);
                        if (old != m_idToClient.end() &&
                            old->second.get() == client.get())
                        {
                            m_idToClient.erase(old);
                        }
                    }

                    client->clientId = id;
                    client->isRegistered = true;
                    m_idToClient[id] = client;
                }
            }

            if (idTaken)
            {
                sendLine(client, "ERROR ID taken\n");
                return;
            }

            sendLine(client, "REGISTERED\n");
            std::cout << "[SignallingServer] Client "
                      << (sameRegistration ? "re-registered (same id): "
                                           : "registered: ")
                      << id << std::endl;
            return;
        }

        if (!client->isRegistered)
        {
            sendLine(client, "ERROR Register first\n");
            return;
        }

        if (cmd.compare(0, 10, "CANDIDATE ") == 0)
        {
            size_t space = cmd.find(' ', 10);
            if (space == std::string::npos)
            {
                sendLine(client, "ERROR Malformed CANDIDATE\n");
                return;
            }

            std::string targetId = cmd.substr(10, space - 10);
            std::string candidate = cmd.substr(space + 1);

            auto target = findClientById(targetId);
            if (target)
            {
                sendLine(target, "CANDIDATE " + client->clientId + " " +
                                     candidate + "\n");
            }
            return;
        }

        if (cmd.compare(0, 6, "OFFER ") == 0 ||
            cmd.compare(0, 7, "ANSWER ") == 0)
        {
            const bool isOffer = (cmd.compare(0, 6, "OFFER ") == 0);
            const size_t cmdLen = isOffer ? 6 : 7;
            std::string targetId = cmd.substr(cmdLen);

            auto target = findClientById(targetId);
            if (target)
            {
                sendLine(target, std::string(isOffer ? "OFFER " : "ANSWER ") +
                                     client->clientId + "\n");
            }
            return;
        }

        if (cmd == "LIST")
        {
            std::string listMsg = "LIST ";

            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                bool first = true;
                for (const auto &[userId, ctx] : m_idToClient)
                {
                    if (!ctx || ctx.get() == client.get())
                    {
                        continue;
                    }

                    if (!first)
                    {
                        listMsg += ",";
                    }

                    listMsg += userId;
                    first = false;
                }
            }

            listMsg += "\n";
            sendLine(client, listMsg);
            return;
        }

        if (cmd.compare(0, 5, "CHAT ") == 0)
        {
            size_t space = cmd.find(' ', 5);
            if (space == std::string::npos)
            {
                sendLine(client, "ERROR Malformed CHAT\n");
                return;
            }

            std::string targetId = cmd.substr(5, space - 5);
            std::string message = cmd.substr(space + 1);

            auto target = findClientById(targetId);
            if (target)
            {
                sendLine(target, "SERVER_MSG " + client->clientId + " " +
                                     message + "\n");
            }
            return;
        }

        sendLine(client, "ERROR Unknown command\n");
    }

    std::shared_ptr<ClientContext> findClientById(const std::string &clientId)
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        auto it = m_idToClient.find(clientId);
        if (it == m_idToClient.end())
        {
            return nullptr;
        }

        return it->second;
    }

    bool sendLine(const std::shared_ptr<ClientContext> &client,
                  const std::string &msg)
    {
        if (!client || msg.empty() || !client->active.load())
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(client->sendMutex);

        const char *data = msg.data();
        int remaining = static_cast<int>(msg.size());
        while (remaining > 0)
        {
            int sent = send(client->socket, data, remaining, 0);
            if (sent == SOCKET_ERROR || sent == 0)
            {
                if (client->active.exchange(false))
                {
                    removeClient(client);
                }
                return false;
            }

            data += sent;
            remaining -= sent;
        }

        return true;
    }

    void removeClient(const std::shared_ptr<ClientContext> &client)
    {
        if (!client)
        {
            return;
        }

        client->active = false;

        SOCKET sock = client->socket;
        client->socket = INVALID_SOCKET;
        if (sock != INVALID_SOCKET)
        {
            shutdown(sock, SD_BOTH);
            closesocket(sock);
        }

        std::lock_guard<std::mutex> lock(m_clientsMutex);

        if (!client->clientId.empty())
        {
            auto it = m_idToClient.find(client->clientId);
            if (it != m_idToClient.end() && it->second.get() == client.get())
            {
                m_idToClient.erase(it);
            }
        }

        m_clients.erase(
            std::remove_if(
                m_clients.begin(), m_clients.end(),
                [&client](const std::shared_ptr<ClientContext> &current)
                { return !current || current.get() == client.get(); }),
            m_clients.end());
    }

    void cleanup()
    {
        if (m_listenSocket != INVALID_SOCKET)
        {
            closesocket(m_listenSocket);
            m_listenSocket = INVALID_SOCKET;
        }

        if (m_wsaStarted)
        {
            WSACleanup();
            m_wsaStarted = false;
        }
    }

    void logError(const std::string &msg)
    {
        std::cerr << "[SignallingServer] " << msg << std::endl;
    }

    uint16_t m_port;
    std::atomic<bool> m_running{false};
    SOCKET m_listenSocket = INVALID_SOCKET;
    bool m_wsaStarted = false;

    std::mutex m_clientsMutex;
    std::vector<std::shared_ptr<ClientContext>> m_clients;
    std::map<std::string, std::shared_ptr<ClientContext>> m_idToClient;
};

int main()
{
    SignallingServer server(27015);

    if (!server.Start())
    {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }

    std::thread serverThread([&server]() { server.Run(); });

    std::cout << "Server running. Press Enter to stop...\n";
    std::cin.get();

    server.Stop();
    if (serverThread.joinable())
    {
        serverThread.join();
    }

    return 0;
}
