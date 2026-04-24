#ifndef GBN_HPP
#define GBN_HPP

#include <WinSock2.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace gbn {

constexpr int kPayloadSize = 1024;
constexpr int kBufferLength = kPayloadSize + 2;
constexpr int kSendWindowSize = 10;
constexpr int kSeqSize = 20;
constexpr int kSendTimeoutRounds = 25;
constexpr int kReceiveIdleRounds = 400;
constexpr int kSendStepDelayMs = 20;
constexpr int kLoopDelayMs = 20;
constexpr double kDataDropRate = 0.1;

inline bool shouldDropDataPacket() {
    static thread_local std::mt19937 rng(static_cast<unsigned int>(GetTickCount()));
    static thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng) < kDataDropRate;
}

inline int inFlightCount(int curSeq, int curAck) {
    int d = curSeq - curAck;
    return d >= 0 ? d : d + kSeqSize;
}

inline bool seqIsAvailable(int curSeq, int curAck, const bool ack[kSeqSize]) {
    int step = curSeq - curAck;
    step = step >= 0 ? step : step + kSeqSize;
    if (step >= kSendWindowSize) {
        return false;
    }
    return ack[curSeq];
}

//收到一个arc会把之前的都认为客户端收到了
inline int ackHandler(unsigned char c, int& curAck, bool ack[kSeqSize]) {
    int index = static_cast<int>(c) - 1;
    if (index < 0 || index >= kSeqSize) {
        return -1;
    }

    if (curAck <= index) {
        for (int i = curAck; i <= index; ++i) {
            ack[i] = true;
        }
        curAck = (index + 1) % kSeqSize;
    } else {
        for (int i = curAck; i < kSeqSize; ++i) {
            ack[i] = true;
        }
        for (int i = 0; i <= index; ++i) {
            ack[i] = true;
        }
        curAck = index + 1;
    }
    return index;
}

inline int timeoutHandler(int& curSeq, int curAck, int& totalSeq, bool ack[kSeqSize]) {
    int toResend = inFlightCount(curSeq, curAck);
    if (toResend <= 0) {
        return 0;
    }

    for (int i = 0; i < toResend; ++i) {
        int idx = (i + curAck) % kSeqSize;
        ack[idx] = true;
    }

    totalSeq -= toResend;
    if (totalSeq < 0) {
        totalSeq = 0;
    }
    curSeq = curAck;
    return toResend;
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

    bool ack[kSeqSize];
    for (int i = 0; i < kSeqSize; ++i) {
        ack[i] = true;
    }

    int curSeq = 0;
    int curAck = 0;
    int totalSeq = 0;
    int waitCount = 0;
    int timeoutEvents = 0;
    char sendBuf[kBufferLength];
    char recvBuf[kBufferLength];

    SOCKADDR_IN fromAddr;
    int fromLen = sizeof(fromAddr);

    while (true) {
        if (totalSeq >= totalPacket && inFlightCount(curSeq, curAck) == 0) {
            if (logPrefix != nullptr) {
                printf("[%s] all packets sent and acked. total=%d\n", logPrefix, totalPacket);
            }
            return true;
        }

        if (seqIsAvailable(curSeq, curAck, ack) && totalSeq < totalPacket) {
            int seq = curSeq;
            int pkt = totalSeq;
            int offset = pkt * kPayloadSize;
            int left = static_cast<int>(data.size()) - offset;
            int payloadLen = std::min(kPayloadSize, left);

            sendBuf[0] = static_cast<char>(seq + 1);
            ack[seq] = false;
            memcpy(&sendBuf[1], &data[offset], payloadLen);

            bool dropped = shouldDropDataPacket();
            if (!dropped) {
                sendto(
                    sock,
                    sendBuf,
                    payloadLen + 1,
                    0,
                    reinterpret_cast<const SOCKADDR*>(&peer),
                    sizeof(SOCKADDR)
                );
            }

            if (logPrefix != nullptr) {
                if (dropped) {
                    printf(
                        "[%s] DROP seq=%d packet=%d/%d bytes=%d (simulated loss)\n",
                        logPrefix,
                        seq,
                        pkt + 1,
                        totalPacket,
                        payloadLen
                    );
                } else {
                    printf(
                        "[%s] SEND seq=%d packet=%d/%d bytes=%d\n",
                        logPrefix,
                        seq,
                        pkt + 1,
                        totalPacket,
                        payloadLen
                    );
                }
            }

            ++curSeq;
            curSeq %= kSeqSize;
            ++totalSeq;
            Sleep(kSendStepDelayMs);
        }

        int recvSize = recvfrom(
            sock,
            recvBuf,
            kBufferLength,
            0,
            reinterpret_cast<SOCKADDR*>(&fromAddr),
            &fromLen
        );

        if (recvSize < 0) {
            ++waitCount;
            if (waitCount > kSendTimeoutRounds) {
                int resend = timeoutHandler(curSeq, curAck, totalSeq, ack);
                if (logPrefix != nullptr) {
                    printf("[%s] TIMEOUT resend=%d restart_seq=%d\n", logPrefix, resend, curSeq);
                }
                waitCount = 0;
                ++timeoutEvents;
                if (timeoutEvents > 200) {
                    if (logPrefix != nullptr) {
                        printf("[%s] too many timeouts, send failed\n", logPrefix);
                    }
                    return false;
                }
            }
        } else {
            int oldBase = curAck;
            int ackSeq = ackHandler(static_cast<unsigned char>(recvBuf[0]), curAck, ack);
            if (ackSeq >= 0 && logPrefix != nullptr) {
                if (curAck == oldBase) {
                    int lastInOrder = (curAck - 1 + kSeqSize) % kSeqSize;
                    printf(
                        "[%s] DUPACK seq=%d (client in-order up to seq=%d), waiting seq=%d\n",
                        logPrefix,
                        ackSeq,
                        lastInOrder,
                        curAck
                    );
                } else {
                    printf("[%s] ACK  seq=%d new_base=%d\n", logPrefix, ackSeq, curAck);
                }
            }
            waitCount = 0;
            timeoutEvents = 0;
        }

        Sleep(kLoopDelayMs);
    }
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

    int expectedSeq = 0;
    int lastAckedSeq = -1;
    int idleTimeoutCount = 0;

    char recvBuf[kBufferLength];
    SOCKADDR_IN fromAddr;
    int fromLen = sizeof(fromAddr);

    while (out.size() < expectedBytes) {
        int recvSize = recvfrom(
            sock,
            recvBuf,
            kBufferLength,
            0,
            reinterpret_cast<SOCKADDR*>(&fromAddr),
            &fromLen
        );

        if (recvSize <= 0) {
            ++idleTimeoutCount;
            if (idleTimeoutCount > kReceiveIdleRounds) {
                if (logPrefix != nullptr) {
                    printf("[%s] receive timeout. bytes=%zu/%zu\n", logPrefix, out.size(), expectedBytes);
                }
                return false;
            }
            continue;
        }

        idleTimeoutCount = 0;

        if (fromAddr.sin_addr.S_un.S_addr != peer.sin_addr.S_un.S_addr || fromAddr.sin_port != peer.sin_port) {
            continue;
        }

        unsigned char got = static_cast<unsigned char>(recvBuf[0]);
        int seq = static_cast<int>(got) - 1;

        if (seq == expectedSeq) {
            int payloadLen = recvSize - 1;
            if (payloadLen > 0) {
                size_t remaining = expectedBytes - out.size();
                size_t toWrite = std::min(remaining, static_cast<size_t>(payloadLen));
                out.insert(out.end(), &recvBuf[1], &recvBuf[1] + toWrite);
            }

            lastAckedSeq = seq;
            expectedSeq = (expectedSeq + 1) % kSeqSize;

            char ackCode = static_cast<char>(seq + 1);
            sendto(
                sock,
                &ackCode,
                1,
                0,
                reinterpret_cast<const SOCKADDR*>(&peer),
                sizeof(SOCKADDR)
            );

            if (logPrefix != nullptr) {
                printf("[%s] RECV seq=%d bytes=%d total=%zu/%zu\n", logPrefix, seq, payloadLen, out.size(), expectedBytes);
                printf("[%s] ACK  send ack=%d (next expected=%d)\n", logPrefix, seq, expectedSeq);
            }
        } else {
            if (lastAckedSeq >= 0) {
                char ackCode = static_cast<char>(lastAckedSeq + 1);
                sendto(
                    sock,
                    &ackCode,
                    1,
                    0,
                    reinterpret_cast<const SOCKADDR*>(&peer),
                    sizeof(SOCKADDR)
                );
                if (logPrefix != nullptr) {
                    printf(
                        "[%s] OOO  got seq=%d but expected=%d, resend ack=%d\n",
                        logPrefix,
                        seq,
                        expectedSeq,
                        lastAckedSeq
                    );
                }
            } else if (logPrefix != nullptr) {
                printf("[%s] OOO  got seq=%d but expected=%d, no ack yet\n", logPrefix, seq, expectedSeq);
            }
        }
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

}  // namespace gbn

#endif
