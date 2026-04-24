#include <WinSock2.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

#pragma comment(lib, "ws2_32.lib")

// 服务器监听端口。客户端需要把数据发到这个端口。
#define SERVER_PORT 12340
// 这里定义了服务器 IP；本程序实际使用 INADDR_ANY 绑定全部网卡。
#define SERVER_IP "0.0.0.0"

// 单个 UDP 数据包缓冲区长度。
// 约定第 1 字节存序号，后面 1024 字节存数据，因此至少需要 1025。
const int BUFFER_LENGTH = 1026;

// GBN（Go-Back-N）发送窗口大小。
// 含义：最多允许多少个“已发送但未确认”的包并发在网络中。
const int SEND_WIND_SIZE = 10;

// 序号空间大小。序号按 0~19 循环使用。
const int SEQ_SIZE = 20;

// ack[i] 的含义：
// TRUE  -> 该序号当前可用（可发送或已被确认）
// FALSE -> 该序号对应分组已发出，正在等待 ACK
BOOL ack[SEQ_SIZE];

// curSeq: 下一个准备发送的数据包序号（0~19 循环）
int curSeq;

// curAck: 当前窗口左边界（最早尚未确认的序号）
int curAck;

// totalSeq: 已尝试发送的数据分片总数（每片 1024 字节）
int totalSeq;

// totalPacket: 计划发送的总分片数
int totalPacket;

// 获取当前系统时间并写入 ptime。
// 示例：收到 "-time" 命令时会调用它。
void getCurTime(char* ptime) {
    char buffer[128];
    memset(buffer, 0, sizeof(buffer));

    time_t c_time;
    struct tm* p;
    time(&c_time);
    p = localtime(&c_time);

    sprintf_s(
        buffer,
        "%d/%d/%d %d:%d:%d",
        p->tm_year + 1900,
        p->tm_mon,
        p->tm_mday,
        p->tm_hour,
        p->tm_min,
        p->tm_sec
    );

    // 把格式化后的时间字符串复制到调用者提供的缓冲区。
    strcpy_s(ptime, sizeof(buffer), buffer);
}

// 判断当前序号 curSeq 是否允许发送。
// 允许发送需满足两个条件：
// 1) curSeq 在发送窗口内（距离 curAck 小于窗口大小）
// 2) 该序号状态是 TRUE（未被占用或已经确认）
bool seqIsAvailable() {
    // 计算 curSeq 相对 curAck 的“环形距离”。
    int step = curSeq - curAck;
    step = step >= 0 ? step : step + SEQ_SIZE;

    // 超出窗口大小，不能发送。
    if (step >= SEND_WIND_SIZE) {
        return false;
    }

    // 只有该序号可用时才允许发送。
    if (ack[curSeq]) {
        return true;
    }

    return false;
}

// 超时处理函数（GBN 核心行为之一）。
// 当连续收不到 ACK 时，将窗口内数据包标记为可重发，并回退发送游标。
void timeoutHandler() {
    printf("Timer out error.\n");

    for (int i = 0; i < SEND_WIND_SIZE; ++i) {
        int index = (i + curAck) % SEQ_SIZE;
        ack[index] = TRUE;
    }

    // 回退已发送计数和当前序号，使后续从窗口起点重发。
    totalSeq -= SEND_WIND_SIZE;
    curSeq = curAck;
}

// ACK 处理函数。
// 协议中发送时序号是“实际序号 + 1”，所以收到后要减 1 还原。
// 并且使用累计确认：收到某序号 ACK，默认其之前的也都确认了。
void ackHandler(char c) {
    unsigned char index = static_cast<unsigned char>(c) - 1;
    printf("Recv a ack of %d\n", index);

    // 情况 A：ACK 序号在 curAck 右侧（未回绕）
    if (curAck <= index) {
        for (int i = curAck; i <= index; ++i) {
            ack[i] = TRUE;
        }
        curAck = (index + 1) % SEQ_SIZE;
    } else {
        // 情况 B：ACK 发生回绕（例如 curAck 在高位，ACK 在低位）
        for (int i = curAck; i < SEQ_SIZE; ++i) {
            ack[i] = TRUE;
        }
        for (int i = 0; i <= index; ++i) {
            ack[i] = TRUE;
        }
        curAck = index + 1;
    }
}

int main(int argc, char* argv[]) {
    // 本程序不使用命令行参数，显式消除编译器未使用警告。
    (void)argc;
    (void)argv;

    // 1) 初始化 Winsock（Windows 网络 API 基础库）
    WORD wVersionRequested = MAKEWORD(2, 2);
    WSADATA wsaData;
    int err = WSAStartup(wVersionRequested, &wsaData);
    if (err != 0) {
        printf("WSAStartup failed with error: %d\n", err);
        return -1;
    }

    // 校验 Winsock 版本是否可用。
    if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2) {
        printf("Could not find a usable version of Winsock.dll\n");
        WSACleanup();
        return -1;
    }
    printf("The Winsock 2.2 dll was found okay\n");

    // 2) 创建 UDP 套接字（SOCK_DGRAM）
    SOCKET sockServer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockServer == INVALID_SOCKET) {
        printf("Could not create socket. Error code is %d\n", WSAGetLastError());
        WSACleanup();
        return -1;
    }

    // 3) 设置为非阻塞模式：没有数据时 recvfrom 立即返回，而不是阻塞等待。
    int iMode = 1;
    ioctlsocket(sockServer, FIONBIO, (u_long FAR*)&iMode);

    // 4) 绑定服务端地址和端口。
    SOCKADDR_IN addrServer;
    memset(&addrServer, 0, sizeof(addrServer));
    addrServer.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
    addrServer.sin_family = AF_INET;
    addrServer.sin_port = htons(SERVER_PORT);

    err = bind(sockServer, (SOCKADDR*)&addrServer, sizeof(SOCKADDR));
    if (err) {
        err = GetLastError();
        printf("Could not bind the port %d for socket. Error code is %d\n", SERVER_PORT, err);
        closesocket(sockServer);
        WSACleanup();
        return -1;
    }

    // 客户端地址信息会在 recvfrom 成功后被填充。
    SOCKADDR_IN addrClient;
    int length = sizeof(SOCKADDR);

    // 通用收发缓冲区。
    char buffer[BUFFER_LENGTH];
    ZeroMemory(buffer, sizeof(buffer));

    // 读取待发送文件（用于 GBN 测试）。
    // 这里固定读取到 1024*113 字节，不足的部分保持 0。
    std::ifstream icin("../test.txt", std::ios::binary);
    char data[1024 * 113];
    ZeroMemory(data, sizeof(data));
    if (icin.is_open()) {
        icin.read(data, sizeof(data));
        icin.close();
    }

    //每个packet只有1k
    totalPacket = sizeof(data) / 1024;
    int recvSize = 0;

    // 初始状态下所有序号都标记为可用。
    for (int i = 0; i < SEQ_SIZE; ++i) {
        ack[i] = TRUE;
    }

    // 5) 服务端主循环：不断接收客户端命令并响应。
    while (true) {
        recvSize = recvfrom(
            sockServer,
            buffer,
            BUFFER_LENGTH,
            0,
            (SOCKADDR*)&addrClient,
            &length
        );

        // 非阻塞模式下，无数据会返回负值。
        if (recvSize < 0) {
            Sleep(200);
            continue;
        }

        // 默认按文本命令处理并打印。
        printf("recv from client: %s\n", buffer);

        // 命令 1：返回服务器当前时间。
        if (strcmp(buffer, "-time") == 0) {
            getCurTime(buffer);

        // 命令 2：退出提示。
        } else if (strcmp(buffer, "-quit") == 0) {
            strcpy_s(buffer, strlen("Good bye!") + 1, "Good bye!");

        // 命令 3：进入 GBN 文件传输测试。
        } else if (strcmp(buffer, "-testgbn") == 0) {
            ZeroMemory(buffer, sizeof(buffer));
            int waitCount = 0;
            printf("Begin to test GBN protocol, please don't abort the process\n");
            printf("Shake hands stage\n");

            // stage 是一个简化状态机：
            // 0 -> 服务端发送 205（我准备好了）
            // 1 -> 等待客户端发送 200（我也准备好了）
            // 2 -> 按 GBN 发送文件分片并处理 ACK/超时
            int stage = 0;
            bool runFlag = true;

            while (runFlag) {
                switch (stage) {
                    case 0:
                        // 握手包：单字节 205
                        buffer[0] = static_cast<char>(205);
                        sendto(
                            sockServer,
                            buffer,
                            1,
                            0,
                            (SOCKADDR*)&addrClient,
                            sizeof(SOCKADDR)
                        );
                        Sleep(100);
                        stage = 1;
                        break;

                    case 1:
                        // 等待客户端回应 200
                        recvSize = recvfrom(
                            sockServer,
                            buffer,
                            BUFFER_LENGTH,
                            0,
                            (SOCKADDR*)&addrClient,
                            &length
                        );

                        if (recvSize < 0) {
                            ++waitCount;
                            if (waitCount > 20) {
                                runFlag = false;
                                printf("Timeout error\n");
                                break;
                            }
                            Sleep(500);
                            continue;
                        }

                        // 收到 200 后，初始化发送窗口参数并进入传输阶段
                        if ((unsigned char)buffer[0] == 200) {
                            printf("Begin a file transfer\n");
                            printf(
                                "File size is %dB, each packet is 1024B and packet total num is %d\n",
                                static_cast<int>(sizeof(data)),
                                totalPacket
                            );
                            curSeq = 0;
                            curAck = 0;
                            totalSeq = 0;
                            waitCount = 0;
                            stage = 2;
                        }
                        break;

                    case 2:
                        // 窗口允许则发送新包
                        if (seqIsAvailable()) {
                            // 协议约定：发给客户端的序号 = curSeq + 1
                            buffer[0] = static_cast<char>(curSeq + 1);

                            // 置为 FALSE，表示该序号已发送，正在等待 ACK
                            ack[curSeq] = FALSE;

                            // 复制 1024 字节数据到 payload 区域（buffer[1] 开始）
                            memcpy(&buffer[1], data + 1024 * totalSeq, 1024);

                            printf("send a packet with a seq of %d\n", curSeq);
                            sendto(
                                sockServer,
                                buffer,
                                BUFFER_LENGTH,
                                0,
                                (SOCKADDR*)&addrClient,
                                sizeof(SOCKADDR)
                            );

                            ++curSeq;
                            curSeq %= SEQ_SIZE;
                            ++totalSeq;
                            Sleep(500);
                        }

                        // 接收 ACK
                        recvSize = recvfrom(
                            sockServer,
                            buffer,
                            BUFFER_LENGTH,
                            0,
                            (SOCKADDR*)&addrClient,
                            &length
                        );

                        if (recvSize < 0) {
                            waitCount++;

                            // 连续多次没等到 ACK -> 触发超时重传
                            if (waitCount > 20) {
                                timeoutHandler();
                                waitCount = 0;
                            }
                        } else {
                            // 收到 ACK -> 推进窗口
                            ackHandler(buffer[0]);
                            waitCount = 0;
                        }
                        Sleep(500);
                        break;
                }
            }
        }

        // 把处理结果发回客户端。
        // 这里按字符串长度发送，适用于 "-time" / "-quit" 这样的文本回复。
        sendto(
            sockServer,
            buffer,
            static_cast<int>(strlen(buffer)) + 1,
            0,
            (SOCKADDR*)&addrClient,
            sizeof(SOCKADDR)
        );
        Sleep(500);
    }

    // 理论上不会走到这里（while(true)），保留资源释放代码。
    closesocket(sockServer);
    WSACleanup();
    return 0;
}
