#ifndef SR_HPP
#define SR_HPP

#include <WinSock2.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace sr {

constexpr int kPayloadSize = 1024;
constexpr int kHeaderSize = 8;
constexpr int kBufferLength = kHeaderSize + kPayloadSize;
constexpr int kSendWindowSize = 10;
constexpr int kSeqSize = 20;
constexpr int kTimeoutRounds = 20;
constexpr int kPacketTimeoutMs = 300;
constexpr double kDataDropRate = 0.2;

inline bool shouldDropDataPacket() {
    static thread_local std::mt19937 rng(static_cast<unsigned int>(GetTickCount()));
    static thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng) < kDataDropRate;
}

inline void writeU32(char* p, unsigned int v) {
    p[0] = static_cast<char>(v & 0xFF);
    p[1] = static_cast<char>((v >> 8) & 0xFF);
    p[2] = static_cast<char>((v >> 16) & 0xFF);
    p[3] = static_cast<char>((v >> 24) & 0xFF);
}

inline unsigned int readU32(const char* p) {
    return static_cast<unsigned int>(static_cast<unsigned char>(p[0])) |
           (static_cast<unsigned int>(static_cast<unsigned char>(p[1])) << 8) |
           (static_cast<unsigned int>(static_cast<unsigned char>(p[2])) << 16) |
           (static_cast<unsigned int>(static_cast<unsigned char>(p[3])) << 24);
}

inline void writeU16(char* p, unsigned short v) {
    p[0] = static_cast<char>(v & 0xFF);
    p[1] = static_cast<char>((v >> 8) & 0xFF);
}

inline unsigned short readU16(const char* p) {
    return static_cast<unsigned short>(static_cast<unsigned char>(p[0])) |
           static_cast<unsigned short>(static_cast<unsigned char>(p[1]) << 8);
}

inline void sendAck(SOCKET sock, const SOCKADDR_IN& peer, unsigned int pktIndex) {
    char ackBuf[5];
    ackBuf[0] = 'A';
    writeU32(&ackBuf[1], pktIndex);
    sendto(sock, ackBuf, 5, 0, reinterpret_cast<const SOCKADDR*>(&peer), sizeof(SOCKADDR));
}

inline bool sendDataPacket(
    SOCKET sock,
    const SOCKADDR_IN& peer,
    const std::vector<char>& data,
    int totalPacket,
    int pktIndex,
    const char* logPrefix,
    bool isRetrans = false,
    int retry = 0
) {
    int offset = pktIndex * kPayloadSize;
    int left = static_cast<int>(data.size()) - offset;
    int payloadLen = std::min(kPayloadSize, left);
    int seq = pktIndex % kSeqSize;

    char buf[kBufferLength];
    buf[0] = 'D';
    buf[1] = static_cast<char>(seq + 1);
    writeU32(&buf[2], static_cast<unsigned int>(pktIndex));
    writeU16(&buf[6], static_cast<unsigned short>(payloadLen));
    memcpy(&buf[kHeaderSize], &data[offset], payloadLen);

    bool dropped = shouldDropDataPacket();
    if (!dropped) {
        sendto(
            sock,
            buf,
            kHeaderSize + payloadLen,
            0,
            reinterpret_cast<const SOCKADDR*>(&peer),
            sizeof(SOCKADDR)
        );
    }

    if (logPrefix != nullptr) {
        if (dropped) {
            printf(
                "[%s] %s pkt=%d/%d seq=%d bytes=%d%s\n",
                logPrefix,
                isRetrans ? "RDROP" : "DROP",
                pktIndex + 1,
                totalPacket,
                seq,
                payloadLen,
                isRetrans ? " (retransmission loss)" : " (simulated loss)"
            );
        } else {
            printf(
                "[%s] %s pkt=%d/%d seq=%d bytes=%d%s\n",
                logPrefix,
                isRetrans ? "RTX " : "SEND",
                pktIndex + 1,
                totalPacket,
                seq,
                payloadLen,
                isRetrans ? " (retransmission)" : ""
            );
        }
    }

    return true;
}

inline bool sendBuffer(
    SOCKET sock,
    const SOCKADDR_IN& peer,
    const std::vector<char>& data,
    const char* logPrefix = nullptr
) {
    int totalPacket = static_cast<int>((data.size() + kPayloadSize - 1) / kPayloadSize);
    if (totalPacket == 0) {
        if (logPrefix != nullptr) {
            printf("[%s] no data to send\n", logPrefix);
        }
        return true;
    }

    std::vector<bool> acked(totalPacket, false);
    std::vector<bool> sent(totalPacket, false);
    std::vector<DWORD> lastSendTick(totalPacket, 0);
    std::vector<int> retryCount(totalPacket, 0);

    int base = 0;
    int next = 0;
    int timeoutEvents = 0;

    char recvBuf[64];
    SOCKADDR_IN fromAddr;
    int fromLen = sizeof(fromAddr);

    while (base < totalPacket) {
        //发送整个窗口
        while (next < totalPacket && next < base + kSendWindowSize) {
            sendDataPacket(sock, peer, data, totalPacket, next, logPrefix, false, 0);
            sent[next] = true;
            lastSendTick[next] = GetTickCount();
            ++next;
        }

        int recvSize = recvfrom(
            sock,
            recvBuf,
            sizeof(recvBuf),
            0,
            reinterpret_cast<SOCKADDR*>(&fromAddr),
            &fromLen
        );

        if (recvSize > 0 &&
            fromAddr.sin_addr.S_un.S_addr == peer.sin_addr.S_un.S_addr &&
            fromAddr.sin_port == peer.sin_port &&
            recvBuf[0] == 'A' &&
            recvSize >= 5) {
            unsigned int ackIdx = readU32(&recvBuf[1]);
            if (ackIdx < static_cast<unsigned int>(totalPacket) && !acked[ackIdx]) {
                acked[ackIdx] = true;
                if (logPrefix != nullptr) {
                    printf("[%s] ACK  pkt=%u\n", logPrefix, ackIdx + 1);
                }
            }
            timeoutEvents = 0;
        }

        while (base < totalPacket && acked[base]) {
            ++base;
        }
        //对窗口里面的包再次发送
        DWORD now = GetTickCount();
        for (int i = base; i < next; ++i) {
            if (sent[i] && !acked[i] && now - lastSendTick[i] > kPacketTimeoutMs) {
                sendDataPacket(sock, peer, data, totalPacket, i, logPrefix, true, retryCount[i] + 1);
                lastSendTick[i] = now;
                ++retryCount[i];
                ++timeoutEvents;
                if (logPrefix != nullptr) {
                    printf("[%s] RTO  pkt=%d retry=%d (retransmitting this packet only)\n", logPrefix, i + 1, retryCount[i]);
                }
                if (retryCount[i] > 200 || timeoutEvents > 2000) {
                    if (logPrefix != nullptr) {
                        printf("[%s] too many retries, send failed\n", logPrefix);
                    }
                    return false;
                }
            }
        }

        Sleep(5);
    }

    if (logPrefix != nullptr) {
        printf("[%s] all packets sent and acked. total=%d\n", logPrefix, totalPacket);
    }
    return true;
}

inline bool receiveBuffer(
    SOCKET sock,
    const SOCKADDR_IN& peer,
    size_t expectedBytes,
    std::vector<char>& out,
    const char* logPrefix = nullptr
) {
    out.clear();
    out.reserve(expectedBytes);

    if (expectedBytes == 0) {
        if (logPrefix != nullptr) {
            printf("[%s] no data to receive\n", logPrefix);
        }
        return true;
    }

    int totalPacket = static_cast<int>((expectedBytes + kPayloadSize - 1) / kPayloadSize);
    std::vector<bool> received(totalPacket, false);
    std::vector<std::vector<char>> chunks(totalPacket);

    int base = 0;
    int idleTimeoutCount = 0;

    char recvBuf[kBufferLength + 32];
    SOCKADDR_IN fromAddr;
    int fromLen = sizeof(fromAddr);

    while (base < totalPacket) {
        int recvSize = recvfrom(
            sock,
            recvBuf,
            sizeof(recvBuf),
            0,
            reinterpret_cast<SOCKADDR*>(&fromAddr),
            &fromLen
        );

        if (recvSize <= 0) {
            ++idleTimeoutCount;
            if (idleTimeoutCount > kTimeoutRounds * 40) {
                if (logPrefix != nullptr) {
                    printf("[%s] receive timeout. packet_base=%d/%d\n", logPrefix, base + 1, totalPacket);
                }
                return false;
            }
            continue;
        }

        idleTimeoutCount = 0;

        if (fromAddr.sin_addr.S_un.S_addr != peer.sin_addr.S_un.S_addr || fromAddr.sin_port != peer.sin_port) {
            continue;
        }

        if (recvBuf[0] != 'D' || recvSize < kHeaderSize) {
            continue;
        }

        int seq = static_cast<int>(static_cast<unsigned char>(recvBuf[1])) - 1;
        unsigned int pktIdxU = readU32(&recvBuf[2]);
        int payloadLen = static_cast<int>(readU16(&recvBuf[6]));
        int pktIdx = static_cast<int>(pktIdxU);

        if (pktIdx < 0 || pktIdx >= totalPacket) {
            continue;
        }

        if (payloadLen < 0 || payloadLen > kPayloadSize || recvSize < kHeaderSize + payloadLen) {
            continue;
        }

        if (pktIdx < base) {
            sendAck(sock, peer, static_cast<unsigned int>(pktIdx));
            continue;
        }

        if (pktIdx >= base + kSendWindowSize) {
            continue;
        }

        if (!received[pktIdx]) {
            size_t maxForPkt = expectedBytes - static_cast<size_t>(pktIdx) * kPayloadSize;
            size_t toWrite = std::min(static_cast<size_t>(payloadLen), maxForPkt);
            chunks[pktIdx].assign(&recvBuf[kHeaderSize], &recvBuf[kHeaderSize] + toWrite);
            received[pktIdx] = true;

            if (logPrefix != nullptr) {
                printf(
                    "[%s] RECV pkt=%d/%d seq=%d bytes=%zu\n",
                    logPrefix,
                    pktIdx + 1,
                    totalPacket,
                    seq,
                    toWrite
                );
            }
        }

        sendAck(sock, peer, static_cast<unsigned int>(pktIdx));

        while (base < totalPacket && received[base]) {
            out.insert(out.end(), chunks[base].begin(), chunks[base].end());
            ++base;
        }
    }

    if (out.size() > expectedBytes) {
        out.resize(expectedBytes);
    }

    if (logPrefix != nullptr) {
        printf("[%s] receive completed. bytes=%zu\n", logPrefix, out.size());
    }
    return true;
}

inline bool readFile(const std::string& path, std::vector<char>& out) {
    FILE* fp = nullptr;
    fopen_s(&fp, path.c_str(), "rb");
    if (fp == nullptr) {
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return false;
    }
    rewind(fp);

    out.resize(static_cast<size_t>(sz));
    if (!out.empty()) {
        size_t n = fread(out.data(), 1, out.size(), fp);
        if (n != out.size()) {
            fclose(fp);
            return false;
        }
    }

    fclose(fp);
    return true;
}

inline bool writeFile(const std::string& path, const std::vector<char>& data) {
    FILE* fp = nullptr;
    fopen_s(&fp, path.c_str(), "wb");
    if (fp == nullptr) {
        return false;
    }

    if (!data.empty()) {
        size_t n = fwrite(data.data(), 1, data.size(), fp);
        if (n != data.size()) {
            fclose(fp);
            return false;
        }
    }

    fclose(fp);
    return true;
}

}  // namespace sr

#endif
