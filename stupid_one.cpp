#include "stdafx.h"
#include <stdio.h>
#include <Windows.h>
#include <process.h>
#include <string.h>

#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif

#define MAXSIZE 65507
#define HTTP_PORT 80

struct HttpHeader
{
    char method[8];
    char url[1024];
    char host[1024];
    char cookie[1024 * 10];

    HttpHeader()
    {
        ZeroMemory(this, sizeof(HttpHeader));
    }
};

BOOL InitSocket();
void ParseHttpHead(char *buffer, HttpHeader *httpHeader);
BOOL ConnectToServer(SOCKET *serverSocket, char *host);
unsigned int __stdcall ProxyThread(LPVOID lpParameter);

SOCKET ProxyServer;
sockaddr_in ProxyServerAddr;
const int ProxyPort = 10240;

struct ProxyParam
{
    SOCKET clientSocket;
    SOCKET serverSocket;
};

int main(int /*argc*/, char * /*argv*/[])
{
    printf("proxy server starting...\n");
    if (!InitSocket())
    {
        printf("socket init failed\n");
        return -1;
    }

    printf("proxy server listening on port %d\n", ProxyPort);

    while (true)
    {
        SOCKET acceptSocket = accept(ProxyServer, NULL, NULL);
        if (acceptSocket == INVALID_SOCKET)
        {
            continue;
        }

        ProxyParam *lpProxyParam = new ProxyParam;
        if (lpProxyParam == NULL)
        {
            closesocket(acceptSocket);
            continue;
        }

        lpProxyParam->clientSocket = acceptSocket;
        lpProxyParam->serverSocket = INVALID_SOCKET;

        HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, &ProxyThread, (LPVOID)lpProxyParam, 0, 0);
        if (hThread != NULL)
        {
            CloseHandle(hThread);
        }
    }

    closesocket(ProxyServer);
    WSACleanup();
    return 0;
}

BOOL InitSocket()
{
    WORD wVersionRequested = MAKEWORD(2, 2);
    WSADATA wsaData;

    int err = WSAStartup(wVersionRequested, &wsaData);
    if (err != 0)
    {
        printf("load winsock failed: %d\n", WSAGetLastError());
        return FALSE;
    }

    if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2)
    {
        WSACleanup();
        return FALSE;
    }

    ProxyServer = socket(AF_INET, SOCK_STREAM, 0);
    if (ProxyServer == INVALID_SOCKET)
    {
        printf("create socket failed: %d\n", WSAGetLastError());
        return FALSE;
    }

    ProxyServerAddr.sin_family = AF_INET;
    ProxyServerAddr.sin_port = htons(ProxyPort);
    ProxyServerAddr.sin_addr.S_un.S_addr = INADDR_ANY;

    if (bind(ProxyServer, (SOCKADDR *)&ProxyServerAddr, sizeof(SOCKADDR)) == SOCKET_ERROR)
    {
        return FALSE;
    }

    if (listen(ProxyServer, SOMAXCONN) == SOCKET_ERROR)
    {
        return FALSE;
    }

    return TRUE;
}

unsigned int __stdcall ProxyThread(LPVOID lpParameter)
{
    ProxyParam *param = (ProxyParam *)lpParameter;
    HttpHeader *httpHeader = NULL;
    char *CacheBuffer = NULL;

    char Buffer[MAXSIZE];
    ZeroMemory(Buffer, MAXSIZE);

    int recvSize = recv(param->clientSocket, Buffer, MAXSIZE, 0);
    if (recvSize <= 0)
    {
        goto error;
    }

    httpHeader = new HttpHeader();
    CacheBuffer = new char[recvSize + 1];
    ZeroMemory(CacheBuffer, recvSize + 1);
    memcpy(CacheBuffer, Buffer, recvSize);

    ParseHttpHead(CacheBuffer, httpHeader);
    delete[] CacheBuffer;
    CacheBuffer = NULL;

    if (!ConnectToServer(&param->serverSocket, httpHeader->host))
    {
        goto error;
    }

    if (send(param->serverSocket, Buffer, recvSize, 0) <= 0)
    {
        goto error;
    }

    recvSize = recv(param->serverSocket, Buffer, MAXSIZE, 0);
    if (recvSize <= 0)
    {
        goto error;
    }

    send(param->clientSocket, Buffer, recvSize, 0);

error:
    if (CacheBuffer)
    {
        delete[] CacheBuffer;
    }
    if (httpHeader)
    {
        delete httpHeader;
    }
    if (param->clientSocket != INVALID_SOCKET)
    {
        closesocket(param->clientSocket);
    }
    if (param->serverSocket != INVALID_SOCKET)
    {
        closesocket(param->serverSocket);
    }
    delete param;
    _endthreadex(0);
    return 0;
}

void ParseHttpHead(char *buffer, HttpHeader *httpHeader)
{
    char *p;
    char *ptr;
    const char *delim = "\r\n";

    p = strtok_s(buffer, delim, &ptr);
    if (!p)
    {
        return;
    }

    if (p[0] == 'G')
    {
        memcpy(httpHeader->method, "GET", 3);
    }
    else if (p[0] == 'P')
    {
        memcpy(httpHeader->method, "POST", 4);
    }
    else if (p[0] == 'C')
    {
        memcpy(httpHeader->method, "CONNECT", 7);
    }

    p = strtok_s(NULL, delim, &ptr);
    while (p)
    {
        if (p[0] == 'H' && strncmp(p, "Host: ", 6) == 0)
        {
            strncpy_s(httpHeader->host, sizeof(httpHeader->host), p + 6, _TRUNCATE);
        }
        else if (p[0] == 'C' && strncmp(p, "Cookie:", 7) == 0)
        {
            strncpy_s(httpHeader->cookie, sizeof(httpHeader->cookie), p + 8, _TRUNCATE);
        }
        p = strtok_s(NULL, delim, &ptr);
    }
}

BOOL ConnectToServer(SOCKET *serverSocket, char *host)
{
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(HTTP_PORT);

    HOSTENT *hostent = gethostbyname(host);
    if (!hostent)
    {
        return FALSE;
    }

    in_addr inaddr = *((in_addr *)*hostent->h_addr_list);
    serverAddr.sin_addr.s_addr = inet_addr(inet_ntoa(inaddr));

    *serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (*serverSocket == INVALID_SOCKET)
    {
        return FALSE;
    }

    if (connect(*serverSocket, (SOCKADDR *)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        closesocket(*serverSocket);
        *serverSocket = INVALID_SOCKET;
        return FALSE;
    }

    return TRUE;
}
