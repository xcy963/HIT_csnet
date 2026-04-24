#include <WinSock2.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "include/GBN.hpp"

#pragma comment(lib, "ws2_32.lib")

#define SERVER_PORT 12340
#define SERVER_IP "127.0.0.1"

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

int main(int argc, char* argv[]) {
    const char* serverIp = (argc >= 2) ? argv[1] : SERVER_IP;

    WORD wVersionRequested = MAKEWORD(2, 2);
    WSADATA wsaData;
    int err = WSAStartup(wVersionRequested, &wsaData);
    if (err != 0) {
        printf("WSAStartup failed with error: %d\n", err);
        return -1;
    }

    SOCKET sockClient = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockClient == INVALID_SOCKET) {
        printf("Could not create socket. Error code is %d\n", WSAGetLastError());
        WSACleanup();
        return -1;
    }

    SOCKADDR_IN addrServer;
    memset(&addrServer, 0, sizeof(addrServer));
    addrServer.sin_family = AF_INET;
    addrServer.sin_port = htons(SERVER_PORT);
    addrServer.sin_addr.S_un.S_addr = inet_addr(serverIp);

    if (addrServer.sin_addr.S_un.S_addr == INADDR_NONE) {
        printf("Invalid server IP: %s\n", serverIp);
        closesocket(sockClient);
        WSACleanup();
        return -1;
    }

    int timeoutMs = 500;
    setsockopt(sockClient, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

    printf("UDP client started. Server = %s:%d\n", serverIp, SERVER_PORT);
    printf("Input command: -time | -quit | -echofile\n");

    char sendBuf[2048];
    char recvBuf[2048];
    SOCKADDR_IN fromAddr;
    int fromLen = sizeof(fromAddr);

    while (true) {
        printf("\n> ");
        fflush(stdout);

        if (!fgets(sendBuf, sizeof(sendBuf), stdin)) {
            break;
        }

        size_t n = strlen(sendBuf);
        if (n > 0 && sendBuf[n - 1] == '\n') {
            sendBuf[n - 1] = '\0';
        }

        if (strlen(sendBuf) == 0) {
            continue;
        }

        if (strcmp(sendBuf, "-time") == 0 || strcmp(sendBuf, "-quit") == 0) {
            sendto(
                sockClient,
                sendBuf,
                static_cast<int>(strlen(sendBuf)) + 1,
                0,
                (SOCKADDR*)&addrServer,
                sizeof(addrServer)
            );

            int recvSize = waitText(sockClient, recvBuf, sizeof(recvBuf), &fromAddr, &fromLen, 20);
            if (recvSize > 0) {
                printf("server: %s\n", recvBuf);
            } else {
                printf("No response (timeout)\n");
            }

            if (strcmp(sendBuf, "-quit") == 0) {
                break;
            }
            continue;
        }

        if (strcmp(sendBuf, "-echofile") == 0) {
            sendto(
                sockClient,
                sendBuf,
                static_cast<int>(strlen(sendBuf)) + 1,
                0,
                (SOCKADDR*)&addrServer,
                sizeof(addrServer)
            );

            int infoN = waitText(sockClient, recvBuf, sizeof(recvBuf), &fromAddr, &fromLen, 60);
            size_t fileSize = 0;
            if (infoN <= 0 || sscanf_s(recvBuf, "FILE_INFO %zu", &fileSize) != 1) {
                printf("server response error: %s\n", infoN > 0 ? recvBuf : "<timeout>");
                continue;
            }

            sendto(
                sockClient,
                "READY_RECV",
                static_cast<int>(strlen("READY_RECV")) + 1,
                0,
                (SOCKADDR*)&addrServer,
                sizeof(addrServer)
            );

            std::vector<char> recvData;
            bool okRecv = gbn::receiveBuffer(sockClient, addrServer, fileSize, recvData, "CLIENT-RX");
            if (!okRecv) {
                printf("receive from server failed\n");
                continue;
            }

            if (!gbn::writeFile("client_recv.bin", recvData)) {
                printf("failed to save client_recv.bin\n");
                continue;
            }

            char readySend[256];
            sprintf_s(readySend, "READY_SEND %zu", recvData.size());
            sendto(
                sockClient,
                readySend,
                static_cast<int>(strlen(readySend)) + 1,
                0,
                (SOCKADDR*)&addrServer,
                sizeof(addrServer)
            );

            bool okSend = gbn::sendBuffer(sockClient, addrServer, recvData, "CLIENT-TX");
            if (!okSend) {
                printf("send back to server failed\n");
                continue;
            }

            int doneN = waitText(sockClient, recvBuf, sizeof(recvBuf), &fromAddr, &fromLen, 60);
            if (doneN > 0) {
                printf("server: %s\n", recvBuf);
            } else {
                printf("no ECHO_DONE from server\n");
            }
            continue;
        }

        sendto(
            sockClient,
            sendBuf,
            static_cast<int>(strlen(sendBuf)) + 1,
            0,
            (SOCKADDR*)&addrServer,
            sizeof(addrServer)
        );

        int recvSize = waitText(sockClient, recvBuf, sizeof(recvBuf), &fromAddr, &fromLen, 20);
        if (recvSize > 0) {
            printf("server: %s\n", recvBuf);
        } else {
            printf("No response (timeout)\n");
        }
    }

    closesocket(sockClient);
    WSACleanup();
    return 0;
}
