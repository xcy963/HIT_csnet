#if defined(_MSC_VER) && __has_include("stdafx.h")
#include "stdafx.h"
#endif

#include <array>
#include <algorithm>
#include <chrono>
#include <clocale>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

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
constexpr std::size_t kMaxCacheEntries = 128;
constexpr std::size_t kMaxCacheObjectBytes = 2 * 1024 * 1024;
constexpr const char* kFilterConfigPath = "proxy_rules.conf";
constexpr const char* kCacheDirectory = "proxy_cache";

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
    std::string method;//post get connect(专门给代理服务器使用的方法)
    std::string url;//ts4.tc.mm.bing.net:443
    std::string host;//ts4.tc.mm.bing.net:443
    std::string cookie;
};

struct CacheEntry {
    std::string response;
    std::string lastModified;
    std::chrono::steady_clock::time_point storedAt;
};

struct FilterConfig {
    std::vector<std::string> blockSites;
    std::vector<std::string> blockUsers;
    std::vector<std::pair<std::string, std::string>> siteRedirects;
};

//这个缓存信息
std::mutex gCacheMutex;
std::unordered_map<std::string, CacheEntry> gHttpCache;

//这个文件是我们的黑名单信息，因为会有多个用户连接代理服务器，所以需要上锁
std::mutex gFilterMutex;
FilterConfig gFilterConfig;
bool SendAll(Socket sock, const char* data, std::size_t len);
std::string GetHeaderValue(const std::string& message, const std::string& headerName);

std::string Trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t");
    return std::string(value.substr(first, last - first + 1));
}

bool StartsWithCaseInsensitive(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));
        if (a != b) {
            return false;
        }
    }
    return true;
}

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string NormalizeHost(std::string host) {
    host = Trim(host);
    if (!host.empty() && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    return ToLower(host);
}

bool MatchPattern(const std::string& value, const std::string& pattern) {
    if (pattern == "*") {
        return true;
    }
    if (pattern.size() > 2 && pattern[0] == '*' && pattern[1] == '.') {
        const std::string suffix = pattern.substr(1);  // ".example.com"
        if (value.size() >= suffix.size() &&
            value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return true;
        }
    }
    if (!pattern.empty() && pattern.back() == '*') {
        const std::string prefix = pattern.substr(0, pattern.size() - 1);
        return value.rfind(prefix, 0) == 0;
    }
    return value == pattern;
}

//封装windows的api，获得用户程序的ip地址，来判断是否放行
std::string ClientIpFromSockaddr(const sockaddr_storage& addr) {
    char host[NI_MAXHOST]{};
    const int rc = getnameinfo(reinterpret_cast<const sockaddr*>(&addr),
                               (addr.ss_family == AF_INET) ? sizeof(sockaddr_in) : sizeof(sockaddr_in6),
                               host, sizeof(host), nullptr, 0, NI_NUMERICHOST);
    if (rc == 0) {
        return std::string(host);
    }
    return {};
}

//设置对应的配置文件，没有的话创建一个
bool EnsureFilterConfigExists(const std::string& path) {
    std::ifstream in(path);
    if (in.good()) {
        return true;
    }

    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.good()) {
        return false;
    }
    out << "# Proxy filter rules\n"
        << "# key=value\n"
        << "# site_block=bad.com\n"
        << "# user_block=10.0.0.8\n"
        << "# site_redirect=old.example.com,https://new.example.com/\n"
        << "# Supports exact and wildcard pattern\n"
        << "# site_block=*.example.com\n"
        << "# user_block=192.168.1.*\n"
        << "# site_redirect=*.legacy.com,https://portal.example.com/\n";
    return true;
}

bool LoadFilterConfig(const std::string& path) {
    std::ifstream in(path);
    if (!in.good()) {
        return false;
    }

    FilterConfig cfg;
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = ToLower(Trim(line.substr(0, eq)));
        const std::string valueRaw = Trim(line.substr(eq + 1));
        const std::string value = ToLower(valueRaw);
        if (valueRaw.empty()) {
            continue;
        }

        if (key == "site_block") {
            cfg.blockSites.push_back(value);
        } else if (key == "user_block") {
            cfg.blockUsers.push_back(value);
        } else if (key == "site_redirect") {
            const auto comma = valueRaw.find(',');
            if (comma == std::string::npos) {
                continue;
            }
            const std::string sourcePattern = ToLower(Trim(valueRaw.substr(0, comma)));
            const std::string destinationUrl = Trim(valueRaw.substr(comma + 1));
            if (sourcePattern.empty() || destinationUrl.empty()) {
                continue;
            }
            cfg.siteRedirects.emplace_back(sourcePattern, destinationUrl);
        }
    }

    std::lock_guard<std::mutex> lock(gFilterMutex);
    gFilterConfig = std::move(cfg);
    return true;
}

bool IsAllowedByBlockList(const std::string& value, const std::vector<std::string>& blockRules) {
    for (const auto& pattern : blockRules) {
        if (MatchPattern(value, pattern)) {
            return false;
        }
    }
    return true;
}

bool IsUserAllowed(const std::string& clientIp) {
    std::lock_guard<std::mutex> lock(gFilterMutex);
    return IsAllowedByBlockList(clientIp, gFilterConfig.blockUsers);
}

bool IsSiteAllowed(const std::string& host) {
    const std::string normalizedHost = NormalizeHost(host);
    std::lock_guard<std::mutex> lock(gFilterMutex);
    return IsAllowedByBlockList(normalizedHost, gFilterConfig.blockSites);
}

//直接重定向
bool TryFindSiteRedirect(const std::string& host, std::string& location) {
    const std::string normalizedHost = NormalizeHost(host);
    std::lock_guard<std::mutex> lock(gFilterMutex);
    for (const auto& [pattern, target] : gFilterConfig.siteRedirects) {
        if (MatchPattern(normalizedHost, pattern)) {
            location = target;
            return true;
        }
    }
    return false;
}

void SendSimpleHttpError(Socket sock, int statusCode, const std::string& reason, const std::string& bodyText) {
    const std::string body = bodyText + "\n";
    std::string response = "HTTP/1.1 " + std::to_string(statusCode) + " " + reason + "\r\n";
    response += "Content-Type: text/plain; charset=utf-8\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Connection: close\r\n\r\n";
    response += body;
    SendAll(sock, response.data(), response.size());
}

void SendRedirectResponse(Socket sock, const std::string& location) {
    const std::string body = "Redirecting to " + location + "\n";
    std::string response = "HTTP/1.1 302 Found\r\n";
    response += "Location: " + location + "\r\n";
    response += "Content-Type: text/plain; charset=utf-8\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Connection: close\r\n\r\n";
    response += body;
    SendAll(sock, response.data(), response.size());
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
        //第一个空格之前的部分是method
        header.method = firstLine.substr(0, methodEnd);
        //第二个空格之前的部分是url
        const auto urlEnd = firstLine.find(' ', methodEnd + 1);
        if (urlEnd != std::string::npos) {
            header.url = firstLine.substr(methodEnd + 1, urlEnd - methodEnd - 1);
        }
        //其实还有一个是http的协议版本，但是这里忽略
    }

    //指向下一行的开头
    std::size_t cursor = lineEnd + 2;
    //遍历剩下的行,寻找Host和Cookie
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

std::string BuildCacheKey(const HttpHeader& header) {
    return header.host + "|" + header.url;
}

std::uint64_t Fnv1a64(std::string_view value) {
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string HexU64(std::uint64_t value) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text(16, '0');
    for (int i = 15; i >= 0; --i) {
        text[static_cast<std::size_t>(i)] = kHex[value & 0xF];
        value >>= 4;
    }
    return text;
}

std::filesystem::path CacheFilePathForKey(const std::string& key) {
    const std::string fileName = HexU64(Fnv1a64(key)) + ".cache";
    // const std::string fileName = key + ".cache";
    return std::filesystem::path(kCacheDirectory) / fileName;
}

bool EnsureCacheDirectoryExists() {
    std::error_code ec;
    if (std::filesystem::exists(kCacheDirectory, ec)) {
        return !ec;
    }
    if (ec) {
        return false;
    }
    return std::filesystem::create_directories(kCacheDirectory, ec) && !ec;
}

bool WriteCacheToDisk(const std::string& key, const std::string& response) {
    const std::filesystem::path path = CacheFilePathForKey(key);
    //trunc：有文件存在先清空
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        return false;
    }
    out.write(response.data(), static_cast<std::streamsize>(response.size()));
    return out.good();
}

void PutCacheEntryToMemory(const std::string& key, CacheEntry entry) {
    std::lock_guard<std::mutex> lock(gCacheMutex);
    if (gHttpCache.size() >= kMaxCacheEntries) {
        auto oldest = gHttpCache.begin();
        for (auto it = gHttpCache.begin(); it != gHttpCache.end(); ++it) {
            if (it->second.storedAt < oldest->second.storedAt) {
                oldest = it;
            }
        }
        gHttpCache.erase(oldest);
    }
    gHttpCache[key] = std::move(entry);
}


std::string RewriteProxyRequest(const std::string& request, const std::string& ifModifiedSince) {
    const auto headerEnd = request.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return request;
    }
    const auto firstLineEnd = request.find("\r\n");
    if (firstLineEnd == std::string::npos || firstLineEnd > headerEnd) {
        return request;
    }

    std::string rewritten;
    rewritten.reserve(request.size() + 64);
    rewritten.append(request.data(), firstLineEnd + 2);

    bool hasIfModifiedSince = false;
    std::size_t cursor = firstLineEnd + 2;
    while (cursor < headerEnd) {
        const auto lineEnd = request.find("\r\n", cursor);
        if (lineEnd == std::string::npos || lineEnd > headerEnd) {
            break;
        }
        std::string_view line(request.data() + cursor, lineEnd - cursor);
        if (StartsWithCaseInsensitive(line, "Connection:") ||
            StartsWithCaseInsensitive(line, "Proxy-Connection:")) {
            cursor = lineEnd + 2;
            continue;
        }
        if (StartsWithCaseInsensitive(line, "If-Modified-Since:")) {
            hasIfModifiedSince = true;
            if (!ifModifiedSince.empty()) {
                rewritten.append("If-Modified-Since: ");
                rewritten.append(ifModifiedSince);
                rewritten.append("\r\n");
            }
            cursor = lineEnd + 2;
            continue;
        }
        rewritten.append(line.data(), line.size());
        rewritten.append("\r\n");
        cursor = lineEnd + 2;
    }

    rewritten.append("Connection: close\r\n");
    if (!ifModifiedSince.empty() && !hasIfModifiedSince) {
        rewritten.append("If-Modified-Since: ");
        rewritten.append(ifModifiedSince);
        rewritten.append("\r\n");
    }

    rewritten.append("\r\n");
    rewritten.append(request.data() + headerEnd + 4, request.size() - headerEnd - 4);
    return rewritten;
}

std::string ReadSocketFully(Socket sock) {
    std::array<char, kMaxSize> buffer{};
    std::string data;
    while (true) {
        const int n = recv(sock, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (n <= 0) {
            break;
        }
        data.append(buffer.data(), static_cast<std::size_t>(n));
    }
    return data;
}

//在第一行，HTTP/1.1 304 Not Modified
int ParseStatusCode(const std::string& response) {
    const auto lineEnd = response.find("\r\n");
    if (lineEnd == std::string::npos) {
        return 0;
    }
    std::string_view statusLine(response.data(), lineEnd);
    const auto firstSpace = statusLine.find(' ');
    if (firstSpace == std::string_view::npos) {
        return 0;
    }
    const auto secondSpace = statusLine.find(' ', firstSpace + 1);
    const std::string codeText = std::string(
        statusLine.substr(firstSpace + 1,
                          (secondSpace == std::string_view::npos ? statusLine.size() : secondSpace) - firstSpace - 1));
    try {
        return std::stoi(codeText);
    } catch (...) {
        return -1;
    }
}

//查询特定的头
std::string GetHeaderValue(const std::string& message, const std::string& headerName) {
    const auto firstLineEnd = message.find("\r\n");
    const auto headerEnd = message.find("\r\n\r\n");
    if (firstLineEnd == std::string::npos || headerEnd == std::string::npos || firstLineEnd >= headerEnd) {
        return {};
    }

    const std::string loweredNeedle = ToLower(headerName);
    std::size_t cursor = firstLineEnd + 2;
    while (cursor < headerEnd) {
        const auto lineEnd = message.find("\r\n", cursor);
        if (lineEnd == std::string::npos || lineEnd > headerEnd) {
            break;
        }
        std::string_view line(message.data() + cursor, lineEnd - cursor);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string key = ToLower(std::string(line.substr(0, colon)));
            if (key == loweredNeedle) {
                return Trim(line.substr(colon + 1));
            }
        }
        cursor = lineEnd + 2;
    }
    return {};
}

bool IsResponseCacheable(const std::string& response) {
    if (ParseStatusCode(response) != 200) {
        return false;
    }
    const std::string cacheControl = ToLower(GetHeaderValue(response, "Cache-Control"));
    if (cacheControl.find("no-store") != std::string::npos) {
        return false;
    }
    return !GetHeaderValue(response, "Last-Modified").empty();
}

void SaveCache(const std::string& key, std::string response) {
    if (response.empty() || response.size() > kMaxCacheObjectBytes) {
        return;
    }

    const std::string lastModified = GetHeaderValue(response, "Last-Modified");
    if (lastModified.empty()) {
        return;
    }

    CacheEntry entry;
    entry.lastModified = lastModified;
    entry.response = std::move(response);
    entry.storedAt = std::chrono::steady_clock::now();

    WriteCacheToDisk(key, entry.response);
    PutCacheEntryToMemory(key, std::move(entry));
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

// void Relay(Socket source, Socket target) {
//     std::array<char, kMaxSize> relayBuffer{};
//     while (true) {
//         const int n = recv(source, relayBuffer.data(), static_cast<int>(relayBuffer.size()), 0);
//         if (n <= 0) {
//             break;
//         }
//         if (!SendAll(target, relayBuffer.data(), static_cast<std::size_t>(n))) {
//             break;
//         }
//     }
// }
//解析端口，如果用户程序没有指定的话，使用默认的端口，http和https的默认端口不同
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

void HandleClient(Socket clientSocket, const sockaddr_storage& clientAddr) {
    SocketGuard client(clientSocket);
    std::array<char, kMaxSize> buffer{};

    const int recvSize = recv(client.sock, buffer.data(), static_cast<int>(buffer.size()), 0);
    if (recvSize <= 0) {
        return;
    }

    const std::string request(buffer.data(), static_cast<std::size_t>(recvSize));
    // std::printf("[debug]现在接收到: %s",request.c_str());
    const HttpHeader header = ParseHttpHead(request);
    const bool isConnect = (header.method == "CONNECT");

    //如果是connect的话优先使用header.url来寻找远端服务器
    const std::string endpoint = isConnect ? header.url : header.host;
    //解析结果例如[ws.bilibili.com,443]
    const auto [targetHost, targetPort] =
        SplitHostPort(endpoint, isConnect ? kHttpsPort : kHttpPort);
    const std::string clientIp = ClientIpFromSockaddr(clientAddr);

    if (targetHost.empty()) {
        std::cerr << "Missing target host\n";
        return;
    }

    //判断是否应该ban掉用户或网站
    if (!clientIp.empty() && !IsUserAllowed(ToLower(clientIp))) {
        std::cerr << "Blocked user: " << clientIp << "\n";
        SendSimpleHttpError(client.sock, 403, "Forbidden", "User access denied by proxy policy.");
        return;
    }
    if (!IsSiteAllowed(targetHost)) {
        std::cerr << "[嘿嘿嘿我不让你访问]: " << targetHost << "\n";
        SendSimpleHttpError(client.sock, 403, "Forbidden", "Site blocked by proxy policy.");
        return;
    }

    std::printf("[debug]接收到一个请求=%s host=%s url=%s\n",
                header.method.c_str(),
                header.host.c_str(),
                header.url.c_str());

    //发现不应该封禁就建立和远程服务器的连接
    if (!isConnect) {
        std::string redirectLocation;
        if (TryFindSiteRedirect(targetHost, redirectLocation)) {
            std::cerr << "Redirect " << targetHost << " -> " << redirectLocation << "\n";
            //向着客户端发送信息
            SendRedirectResponse(client.sock, redirectLocation);
            return;
        }
    }

    SocketGuard server(ConnectToServer(targetHost, targetPort));
    if (server.sock == kInvalidSocket) {
        std::cerr << "Failed to connect target host: " << targetHost << ":" << targetPort << "\n";
        return;
    }

    //如果是connect请求，建立一个隧道就好，直接给客户端返回字符串
    //connect需要先禁止，不然不能缓存
    if (isConnect) {
        // constexpr const char* kConnectOk = "HTTP/1.1 200 Connection Established\r\n\r\n";
        // if (!SendAll(client.sock, kConnectOk, std::strlen(kConnectOk))) {
        //     return;
        // }

        // std::thread c2s([&]() {
        //     //创建一个线程把客户端的数据直接发送到服务端
        //     Relay(client.sock, server.sock);
        //     shutdown(server.sock, SD_SEND);
        // });

        // //主线程继续把服务端的通信转发到客户端
        // Relay(server.sock, client.sock);
        // shutdown(client.sock, SD_SEND);
        // c2s.join();
        return;
    }
    // std::printf("[debug]现在接收到:\n %s\n",request.c_str());

    // std::printf("[debug]接收到一个请求=%s host=%s url=%s\n",
    //         header.method.c_str(),
    //         header.host.c_str(),
    //         header.url.c_str());
    //标记是不是get方法
    const bool isGet = (header.method == "GET");
    //形式是host | url
    const std::string cacheKey = BuildCacheKey(header);

    CacheEntry cached{};
    bool hasValidCached = false;
    if (isGet) {
        {
            std::lock_guard<std::mutex> lock(gCacheMutex);
            auto it = gHttpCache.find(cacheKey);
        //找到本地缓存，并且本地已经标记过他是修改过的
        if (it != gHttpCache.end() && !it->second.lastModified.empty()) {
            cached = it->second;
            hasValidCached = true;
        }
        }
    }

    //访问原网站，重新验证缓存
    std::string outboundRequest = request;
    if (hasValidCached) {
        std::cout<<"本地现在有缓存"<<std::endl;
        outboundRequest = RewriteProxyRequest(request, cached.lastModified);
    } else {
        std::cout<<"找不到缓存"<<std::endl;
        outboundRequest = RewriteProxyRequest(request, "");
    }
    if (!SendAll(server.sock, outboundRequest.data(), outboundRequest.size())) {
        return;
    }


    std::string upstreamResponse = ReadSocketFully(server.sock);
    if (upstreamResponse.empty()) {
        return;
    }
    // std::printf("[debug]远程返回: \n%s\n",upstreamResponse.c_str());
    const int statusCode = ParseStatusCode(upstreamResponse);
    if (hasValidCached && statusCode == 304) {
        //304说明远程网站没有变换，那么直接给客户端返回本地的缓存
        SendAll(client.sock, cached.response.data(), cached.response.size());
        return;
    }

    //否则返回远程的缓存
    if (!SendAll(client.sock, upstreamResponse.data(), upstreamResponse.size())) {
        return;
    }

    //get方法就保存，post通常比较复杂?
    if (IsResponseCacheable(upstreamResponse)) {
        SaveCache(cacheKey, std::move(upstreamResponse));
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

    if (!EnsureFilterConfigExists(kFilterConfigPath)) {
        std::cerr << "Failed to create default filter config: " << kFilterConfigPath << "\n";
        return 1;
    }
    if (!LoadFilterConfig(kFilterConfigPath)) {
        std::cerr << "Failed to load filter config: " << kFilterConfigPath << "\n";
        return 1;
    }
    if (!EnsureCacheDirectoryExists()) {
        std::cerr << "Failed to create cache directory: " << kCacheDirectory << "\n";
        return 1;
    }

    SocketGuard proxyServer(InitSocket());
    if (proxyServer.sock == kInvalidSocket) {
        std::cerr << "Failed to initialize proxy socket on port " << kProxyPort << "\n";
        return 1;
    }

    std::cout << "Proxy is listening on port " << kProxyPort
              << " with rules from " << kFilterConfigPath << '\n';

    while (true) {
        sockaddr_storage clientAddr{};
        int clientAddrLen = sizeof(clientAddr);
        Socket client = accept(proxyServer.sock, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);
        if (client == kInvalidSocket) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::thread([client, clientAddr]() { HandleClient(client, clientAddr); }).detach();
    }
}
