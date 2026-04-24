#include <WinSock2.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "include/SR.hpp"

#pragma comment(lib, "ws2_32.lib")

#define SERVER_PORT 12341

int waitText(SOCKET sock, char* buf, int len, SOCKADDR_IN* fromAddr, int* fromLen, int maxRetry) {
    for (int i = 0; i < maxRetry; ++i) {
        memset(buf, 0, len);
        int n = recvfrom(sock, buf, len, 0, (SOCKADDR*)fromAddr, fromLen);
        if (n > 0) {
            return n;
        }
        Sleep(50);
    }
    return -1;
}

int main() {
    WORD wVersionRequested = MAKEWORD(2, 2);
    WSADATA wsaData;
    int err = WSAStartup(wVersionRequested, &wsaData);
    if (err != 0) {
        printf("WSAStartup failed with error: %d\n", err);
        return -1;
    }

    SOCKET sockServer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockServer == INVALID_SOCKET) {
        printf("Could not create socket. Error code is %d\n", WSAGetLastError());
        WSACleanup();
        return -1;
    }

    SOCKADDR_IN addrServer;
    memset(&addrServer, 0, sizeof(addrServer));
    addrServer.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
    addrServer.sin_family = AF_INET;
    addrServer.sin_port = htons(SERVER_PORT);

    err = bind(sockServer, (SOCKADDR*)&addrServer, sizeof(SOCKADDR));
    if (err) {
        printf(
            "Could not bind the port %d for socket. Error code is %lu\n",
            SERVER_PORT,
            static_cast<unsigned long>(GetLastError())
        );
        closesocket(sockServer);
        WSACleanup();
        return -1;
    }

    int timeoutMs = 500;
    setsockopt(sockServer, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

    std::ifstream icin("./test.txt", std::ios::binary);
    char data[1024 * 10];
    ZeroMemory(data, sizeof(data));
    if (icin.is_open()) {
        icin.read(data, sizeof(data));
        icin.close();
    }
    std::vector<char> localFileData(data, data + sizeof(data));
    printf("Loaded local file ../test.txt into fixed buffer (%zu bytes)\n", localFileData.size());

    printf("SR UDP server started on 0.0.0.0:%d\n", SERVER_PORT);
    printf("Commands: -time | -quit | -echofile\n");

    char buffer[2048];
    SOCKADDR_IN addrClient;
    int length = sizeof(SOCKADDR);

    while (true) {
        memset(buffer, 0, sizeof(buffer));
        int recvSize = recvfrom(sockServer, buffer, sizeof(buffer), 0, (SOCKADDR*)&addrClient, &length);
        if (recvSize <= 0) {
            Sleep(50);
            continue;
        }

        printf("[SR-SERVER] recv from client: %s\n", buffer);

        if (strcmp(buffer, "-time") == 0) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            char ts[128];
            sprintf_s(ts, "%d/%d/%d %d:%d:%d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            sendto(sockServer, ts, static_cast<int>(strlen(ts)) + 1, 0, (SOCKADDR*)&addrClient, sizeof(SOCKADDR));
            continue;
        }

        if (strcmp(buffer, "-quit") == 0) {
            const char* bye = "Good bye!";
            sendto(sockServer, bye, static_cast<int>(strlen(bye)) + 1, 0, (SOCKADDR*)&addrClient, sizeof(SOCKADDR));
            continue;
        }

        if (strcmp(buffer, "-echofile") == 0) {
            char fileInfo[256];
            sprintf_s(fileInfo, "FILE_INFO %zu", localFileData.size());
            sendto(sockServer, fileInfo, static_cast<int>(strlen(fileInfo)) + 1, 0, (SOCKADDR*)&addrClient, sizeof(SOCKADDR));

            int readyRecvN = waitText(sockServer, buffer, sizeof(buffer), &addrClient, &length, 60);
            if (readyRecvN <= 0 || strcmp(buffer, "READY_RECV") != 0) {
                const char* fail = "ECHO_FAIL_READY_RECV";
                sendto(sockServer, fail, static_cast<int>(strlen(fail)) + 1, 0, (SOCKADDR*)&addrClient, sizeof(SOCKADDR));
                continue;
            }

            bool okSend = sr::sendBuffer(sockServer, addrClient, localFileData, "SR-SERVER-TX");
            if (!okSend) {
                const char* fail = "ECHO_FAIL_SEND";
                sendto(sockServer, fail, static_cast<int>(strlen(fail)) + 1, 0, (SOCKADDR*)&addrClient, sizeof(SOCKADDR));
                continue;
            }

            size_t backSize = 0;
            int readySendN = waitText(sockServer, buffer, sizeof(buffer), &addrClient, &length, 60);
            if (readySendN <= 0 || sscanf_s(buffer, "READY_SEND %zu", &backSize) != 1) {
                const char* fail = "ECHO_FAIL_READY_SEND";
                sendto(sockServer, fail, static_cast<int>(strlen(fail)) + 1, 0, (SOCKADDR*)&addrClient, sizeof(SOCKADDR));
                continue;
            }

            std::vector<char> echoed;
            bool okRecv = sr::receiveBuffer(sockServer, addrClient, backSize, echoed, "SR-SERVER-RX");
            if (!okRecv) {
                const char* fail = "ECHO_FAIL_RECV";
                sendto(sockServer, fail, static_cast<int>(strlen(fail)) + 1, 0, (SOCKADDR*)&addrClient, sizeof(SOCKADDR));
                continue;
            }

            sr::writeFile("server_sr_echo_back.bin", echoed);
            const char* done = "ECHO_DONE";
            sendto(sockServer, done, static_cast<int>(strlen(done)) + 1, 0, (SOCKADDR*)&addrClient, sizeof(SOCKADDR));
            continue;
        }

        const char* unk = "Unknown command";
        sendto(sockServer, unk, static_cast<int>(strlen(unk)) + 1, 0, (SOCKADDR*)&addrClient, sizeof(SOCKADDR));
    }

    closesocket(sockServer);
    WSACleanup();
    return 0;
}
