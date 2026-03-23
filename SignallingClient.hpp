
#pragma once
#include <winsock2.h>
#include <string>
#include <thread>
#include <functional>
#include <atomic>
#include <queue>
#include <mutex>
#include <iostream>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

class SignallingClient {
public:
    using MessageCallback = std::function<void(const std::string& from, const std::string& type, const std::string& data)>;

    SignallingClient() : m_socket(INVALID_SOCKET), m_running(false) {}

    ~SignallingClient() {
        Disconnect();
    }

    bool Connect(const char* serverIp, uint16_t serverPort, const std::string& clientId, MessageCallback cb) {
        Disconnect();

        m_callback = cb;
        m_clientId = clientId;

        m_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (m_socket == INVALID_SOCKET) {
            return false;
        }

        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(serverPort);
        if (inet_pton(AF_INET, serverIp, &serverAddr.sin_addr) != 1) {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
            return false;
        }

        if (connect(m_socket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
            return false;
        }

        m_running = true;
        m_recvThread = std::thread(&SignallingClient::ReceiveLoop, this);

        if (!Register(clientId)) {
            Disconnect();
            return false;
        }

        return true;
    }

    void Disconnect() {
        m_running = false;

        SOCKET socket = m_socket;
        m_socket = INVALID_SOCKET;

        if (socket != INVALID_SOCKET) {
            shutdown(socket, SD_BOTH);
            closesocket(socket);
        }

        if (m_recvThread.joinable()) {
            m_recvThread.join();
        }
    }

    void SendCandidate(const std::string& targetId, const std::string& candidate) {
        SendCommand("CANDIDATE " + targetId + " " + candidate + "\n");
    }

    void SendOffer(const std::string& targetId) {
        SendCommand("OFFER " + targetId + "\n");
    }

    void SendAnswer(const std::string& targetId) {
        SendCommand("ANSWER " + targetId + "\n");
    }

    void SendOfferDeclined(const std::string& targetId) {
        SendCommand("OFFER_DECLINED " + targetId + "\n");
    }

    void RequestUserList() {
        SendCommand("LIST\n");
    }

    void SendRelayMessage(const std::string& targetId, const std::string& text) {
        SendCommand("CHAT " + targetId + " " + text + "\n");
    }

    bool Register(const std::string& clientId) {
        if (clientId.empty() || clientId.find(' ') != std::string::npos) {
            return false;
        }

        m_clientId = clientId;
        return SendCommand("REGISTER " + clientId + "\n");
    }

private:
    bool SendCommand(const std::string& msg) {
        if (m_socket == INVALID_SOCKET) {
            return false;
        }

        return send(m_socket, msg.c_str(), static_cast<int>(msg.size()), 0) != SOCKET_ERROR;
    }

    void Notify(const std::string& from, const std::string& type, const std::string& data) {
        if (m_callback) {
            m_callback(from, type, data);
        }
    }

    void ReceiveLoop() {
        const SOCKET socket = m_socket;
        if (socket == INVALID_SOCKET) {
            return;
        }

        char buffer[2048];
        std::string leftover;
        while (m_running.load()) {
            int len = recv(socket, buffer, sizeof(buffer) - 1, 0);
            if (len <= 0) {
                break;
            }

            buffer[len] = '\0';
            leftover += buffer;

            size_t pos;
            while ((pos = leftover.find('\n')) != std::string::npos) {
                std::string line = leftover.substr(0, pos);
                leftover.erase(0, pos + 1);
                ProcessLine(line);
            }
        }
    }

    void ProcessLine(const std::string& line) {
        if (line.empty()) {
            return;
        }

        std::cout << "Signalling received: " << line << std::endl;

        size_t space1 = line.find(' ');
        std::string cmd = (space1 == std::string::npos) ? line : line.substr(0, space1);

        if (cmd == "CANDIDATE") {
            if (space1 == std::string::npos) {
                return;
            }

            size_t space2 = line.find(' ', space1 + 1);
            if (space2 == std::string::npos) {
                return;
            }

            std::string from = line.substr(space1 + 1, space2 - space1 - 1);
            std::string candidate = line.substr(space2 + 1);
            Notify(from, "candidate", candidate);
        }
        else if (cmd == "OFFER") {
            if (space1 == std::string::npos) {
                return;
            }

            Notify(line.substr(space1 + 1), "offer", "");
        }
        else if (cmd == "ANSWER") {
            if (space1 == std::string::npos) {
                return;
            }

            Notify(line.substr(space1 + 1), "answer", "");
        }
        else if (cmd == "OFFER_DECLINED") {
            if (space1 == std::string::npos) {
                return;
            }

            Notify(line.substr(space1 + 1), "offer_declined", "");
        }
        else if (cmd == "LIST") {
            std::string userList = (space1 == std::string::npos) ? "" : line.substr(space1 + 1);
            Notify("server", "list", userList);
        }
        else if (cmd == "REGISTERED") {
            return;
        }
        else if (cmd == "SERVER_MSG") {
            if (space1 == std::string::npos) {
                return;
            }

            size_t space2 = line.find(' ', space1 + 1);
            if (space2 == std::string::npos) {
                return;
            }

            std::string from = line.substr(space1 + 1, space2 - space1 - 1);
            std::string text = line.substr(space2 + 1);
            Notify(from, "server_chat", text);
        }
        else if (cmd == "ERROR") {
            std::string err = (space1 == std::string::npos) ? "" : line.substr(space1 + 1);
            std::cerr << "Signalling error: " << err << std::endl;
        }
    }

    SOCKET m_socket;
    std::thread m_recvThread;
    std::atomic<bool> m_running;
    std::string m_clientId;
    MessageCallback m_callback;
};

