#if defined(_MSC_VER) && __has_include("stdafx.h")
#include "stdafx.h"
#endif

#include <array>
#include <chrono>
#include <clocale>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif

using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;

namespace {

constexpr std::size_t kMaxSize = 65507;
constexpr std::uint16_t kHttpPort = 80;
constexpr std::uint16_t kHttpsPort = 443;
constexpr std::uint16_t kProxyPort = 10240;

void SetupUtf8Console() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    std::setlocale(LC_ALL, ".UTF-8");
}

void CloseSocket(Socket sock) {
    if (sock != kInvalidSocket) {
        closesocket(sock);
    }
}

struct SocketGuard {
    explicit SocketGuard(Socket s = kInvalidSocket) : sock(s) {}
    ~SocketGuard() { CloseSocket(sock); }
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
    SocketGuard(SocketGuard&& other) noexcept : sock(other.sock) { other.sock = kInvalidSocket; }
    SocketGuard& operator=(SocketGuard&& other) noexcept {
        if (this != &other) {
            CloseSocket(sock);
            sock = other.sock;
            other.sock = kInvalidSocket;
        }
        return *this;
    }
    Socket Release() {
        Socket out = sock;
        sock = kInvalidSocket;
        return out;
    }

    Socket sock;
};

struct NetworkRuntime {
    bool ok = true;

    NetworkRuntime() {
        WSADATA wsaData{};
        ok = (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
    }

    ~NetworkRuntime() {
        if (ok) {
            WSACleanup();
        }
    }
};

struct HttpHeader {
    std::string method;//post get connect 
    std::string url;//ts4.tc.mm.bing.net:443
    std::string host;//ts4.tc.mm.bing.net:443
    std::string cookie;
};

std::string Trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t");
    return std::string(value.substr(first, last - first + 1));
}

HttpHeader ParseHttpHead(const std::string& request) {
    HttpHeader header;
    const auto lineEnd = request.find("\r\n");
    if (lineEnd == std::string::npos) {
        return header;
    }
    //寻找第一个行
    const std::string firstLine = request.substr(0, lineEnd);
    const auto methodEnd = firstLine.find(' ');
    if (methodEnd != std::string::npos) {
        header.method = firstLine.substr(0, methodEnd);
        const auto urlEnd = firstLine.find(' ', methodEnd + 1);
        if (urlEnd != std::string::npos) {
            header.url = firstLine.substr(methodEnd + 1, urlEnd - methodEnd - 1);
        }
    }
    //指向下一行的开头
    std::size_t cursor = lineEnd + 2;
    while (cursor < request.size()) {
        const auto nextLineEnd = request.find("\r\n", cursor);
        if (nextLineEnd == std::string::npos || nextLineEnd == cursor) {
            break;
        }

        std::string_view line(request.data() + cursor, nextLineEnd - cursor);
        if (line.rfind("Host:", 0) == 0) {
            header.host = Trim(line.substr(5));
        } else if (line.rfind("Cookie:", 0) == 0) {
            header.cookie = Trim(line.substr(7));
        }

        cursor = nextLineEnd + 2;
    }

    return header;
}

bool SendAll(Socket sock, const char* data, std::size_t len) {
    std::size_t sentTotal = 0;
    while (sentTotal < len) {
        const int sent = send(sock, data + sentTotal, static_cast<int>(len - sentTotal), 0);
        if (sent <= 0) {
            return false;
        }
        sentTotal += static_cast<std::size_t>(sent);
    }
    return true;
}

void Relay(Socket source, Socket target) {
    std::array<char, kMaxSize> relayBuffer{};
    while (true) {
        const int n = recv(source, relayBuffer.data(), static_cast<int>(relayBuffer.size()), 0);
        if (n <= 0) {
            break;
        }
        if (!SendAll(target, relayBuffer.data(), static_cast<std::size_t>(n))) {
            break;
        }
    }
}

std::pair<std::string, std::uint16_t> SplitHostPort(const std::string& endpoint, std::uint16_t defaultPort) {
    if (endpoint.empty()) {
        return {"", defaultPort};
    }

    const auto colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon == endpoint.size() - 1) {
        return {endpoint, defaultPort};
    }

    const std::string host = endpoint.substr(0, colon);
    const std::string portText = endpoint.substr(colon + 1);

    try {
        const int port = std::stoi(portText);
        if (port > 0 && port <= 65535) {
            return {host, static_cast<std::uint16_t>(port)};
        }
    } catch (...) {
    }

    return {endpoint, defaultPort};
}

Socket ConnectToServer(const std::string& host, std::uint16_t port) {
    if (host.empty()) {
        return kInvalidSocket;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string portText = std::to_string(port);
    if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0) {
        return kInvalidSocket;
    }

    SocketGuard candidate;
    for (addrinfo* p = result; p != nullptr; p = p->ai_next) {
        candidate = SocketGuard(socket(p->ai_family, p->ai_socktype, p->ai_protocol));
        if (candidate.sock == kInvalidSocket) {
            continue;
        }
        if (connect(candidate.sock, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) {
            freeaddrinfo(result);
            return candidate.Release();
        }
    }

    freeaddrinfo(result);
    return kInvalidSocket;
}

void HandleClient(Socket clientSocket) {
    SocketGuard client(clientSocket);
    std::array<char, kMaxSize> buffer{};

    const int recvSize = recv(client.sock, buffer.data(), static_cast<int>(buffer.size()), 0);
    if (recvSize <= 0) {
        return;
    }

    const std::string request(buffer.data(), static_cast<std::size_t>(recvSize));
    const HttpHeader header = ParseHttpHead(request);
    const bool isConnect = (header.method == "CONNECT");

    const std::string endpoint = isConnect ? header.url : header.host;
    const auto [targetHost, targetPort] =
        SplitHostPort(endpoint, isConnect ? kHttpsPort : kHttpPort);

    if (targetHost.empty()) {
        std::cerr << "Missing target host\n";
        return;
    }

    std::printf("Incoming method=%s host=%s url=%s\n",
                header.method.c_str(),
                header.host.c_str(),
                header.url.c_str());

    SocketGuard server(ConnectToServer(targetHost, targetPort));
    if (server.sock == kInvalidSocket) {
        std::cerr << "Failed to connect target host: " << targetHost << ":" << targetPort << "\n";
        return;
    }

    if (isConnect) {
        constexpr const char* kConnectOk = "HTTP/1.1 200 Connection Established\r\n\r\n";
        if (!SendAll(client.sock, kConnectOk, std::strlen(kConnectOk))) {
            return;
        }

        std::thread c2s([&]() {
            Relay(client.sock, server.sock);
            shutdown(server.sock, SD_SEND);
        });

        Relay(server.sock, client.sock);
        shutdown(client.sock, SD_SEND);
        c2s.join();
        return;
    }

    if (!SendAll(server.sock, request.data(), request.size())) {
        return;
    }

    while (true) {
        const int upstreamRecv = recv(server.sock, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (upstreamRecv <= 0) {
            break;
        }
        if (!SendAll(client.sock, buffer.data(), static_cast<std::size_t>(upstreamRecv))) {
            break;
        }
    }
}

Socket InitSocket() {
    Socket listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == kInvalidSocket) {
        return kInvalidSocket;
    }

    SocketGuard listenGuard(listenSocket);

    int reuse = 1;
    setsockopt(listenGuard.sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kProxyPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listenGuard.sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        return kInvalidSocket;
    }

    if (listen(listenGuard.sock, SOMAXCONN) != 0) {
        return kInvalidSocket;
    }

    return listenGuard.Release();
}

}  // namespace

int main() {
    SetupUtf8Console();

    NetworkRuntime network;
    if (!network.ok) {
        std::cerr << "Failed to initialize network runtime\n";
        return 1;
    }

    SocketGuard proxyServer(InitSocket());
    if (proxyServer.sock == kInvalidSocket) {
        std::cerr << "Failed to initialize proxy socket on port " << kProxyPort << "\n";
        return 1;
    }

    std::cout << "Proxy is listening on port " << kProxyPort << '\n';

    while (true) {
        sockaddr_storage clientAddr{};
        int clientAddrLen = sizeof(clientAddr);
        Socket client = accept(proxyServer.sock, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);
        if (client == kInvalidSocket) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::thread([client]() { HandleClient(client); }).detach();
    }
}
