#define _POSIX_C_SOURCE 200809L
#include "netlab.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BUFSIZE 1518
#define DEFAULT_IFACE "eth0"
#define DEFAULT_DST_MAC "02:42:ac:1e:00:42"
#define DEFAULT_SRC_IP "192.168.1.1"
#define DEFAULT_DST_IP "192.168.1.5"
#define DEFAULT_SRC_PORT 12345
#define DEFAULT_DST_PORT 12345
#define DEFAULT_MESSAGE "Hello, this is a test message."
#define DEFAULT_INTERVAL_MS 1000
#define DEFAULT_COUNT 1

/*
实现第二个部分的内容,从底层封装网络帧
*/

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: sudo %s <iface> <dst_mac> <src_ip> <dst_ip> <src_port> <dst_port> [message] [interval_ms] [count]\n"
            "Example: sudo %s ens33 00:0c:29:aa:bb:cc 192.168.1.2 192.168.1.3 12345 12345 hello 1000 1\n",
            prog, prog);
}

static uint16_t udp_checksum_ipv4(uint32_t src_ip_be, uint32_t dst_ip_be,
                                  const struct udphdr *udp,
                                  const uint8_t *payload, size_t payload_len) {
    uint32_t sum = 0;
    const uint8_t *p = NULL;
    size_t len = 0;

    // IPv4 pseudo header (treat addresses as network-order byte streams)
    p = (const uint8_t *)&src_ip_be;
    sum += ((uint16_t)p[0] << 8) | p[1];
    sum += ((uint16_t)p[2] << 8) | p[3];
    p = (const uint8_t *)&dst_ip_be;
    sum += ((uint16_t)p[0] << 8) | p[1];
    sum += ((uint16_t)p[2] << 8) | p[3];
    sum += (uint16_t)IPPROTO_UDP;
    sum += ntohs(udp->len);

    // UDP header
    p = (const uint8_t *)udp;
    len = sizeof(struct udphdr);
    while (len > 1) {
        sum += ((uint16_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }

    // UDP payload
    p = payload;
    len = payload_len;
    while (len > 1) {
        sum += ((uint16_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += ((uint16_t)p[0] << 8);
    }

    while (sum >> 16) {
        sum = (sum & 0xffffU) + (sum >> 16);
    }

    uint16_t csum = (uint16_t)(~sum);
    if (csum == 0) {
        csum = 0xffff; // RFC768: 0 means "not used", so map computed zero to 0xffff.
    }
    return csum;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    const char *iface = DEFAULT_IFACE;
    //解析目的地址的mac 00:0c:29:aa:bb:cc会变成6个字节
    //mac在定义上是标识网卡的
    uint8_t dst_mac[MAC_LEN];
    const char *dst_mac_text = DEFAULT_DST_MAC;
    const char *src_ip_text = DEFAULT_SRC_IP;
    const char *dst_ip_text = DEFAULT_DST_IP;
    int src_port = DEFAULT_SRC_PORT;
    int dst_port = DEFAULT_DST_PORT;
    const char *msg = DEFAULT_MESSAGE;
    int interval_ms = DEFAULT_INTERVAL_MS;
    int count = DEFAULT_COUNT;

    if (argc >= 7 && argc <= 10) {
        iface = argv[1];
        dst_mac_text = argv[2];
        src_ip_text = argv[3];
        dst_ip_text = argv[4];
        src_port = atoi(argv[5]);
        dst_port = atoi(argv[6]);
        msg = (argc >= 8) ? argv[7] : DEFAULT_MESSAGE;
        interval_ms = (argc >= 9) ? atoi(argv[8]) : DEFAULT_INTERVAL_MS;
        count = (argc >= 10) ? atoi(argv[9]) : -1;
    }

    if (parse_mac(dst_mac_text, dst_mac) != 0) {
        fprintf(stderr, "Invalid dst_mac: %s\n", dst_mac_text);
        return 1;
    }

    //解析ip地址,ip其实对应的是32的整型
    uint32_t src_ip = parse_ipv4_or_die(src_ip_text);
    uint32_t dst_ip = parse_ipv4_or_die(dst_ip_text);

    size_t msg_len = strlen(msg);
    if (interval_ms <= 0) {
        fprintf(stderr, "interval_ms must be > 0\n");
        return 1;
    }
    if (count == 0 || count < -1) {
        fprintf(stderr, "count must be -1 (infinite) or a positive integer\n");
        return 1;
    }

    if (msg_len > 1400) {
        fprintf(stderr, "Message too long for this simple Ethernet frame example\n");
        return 1;
    }
    //创建socket ,都使用最底层的选项
    //AF_PACKET: 以太网帧
    //SOCK_RAW: 自己构造mac等底层数据
    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sockfd < 0) {
        perror("socket(AF_PACKET)");
        return 1;
    }

    //获取网卡编号
    //本来网卡是ens33,在系统上他是一个整型
    int ifindex = get_iface_index(sockfd, iface);

    //获取mac地址,这个是获取的本机(发送端)的网卡的mac
    uint8_t src_mac[MAC_LEN];
    if (ifindex < 0 || get_iface_mac(sockfd, iface, src_mac) < 0) {
        close(sockfd);
        return 1;
    }

    //帧缓冲区域
    unsigned char frame[BUFSIZE];
    memset(frame, 0, sizeof(frame));

    /*
    frame 起始位置
    │
    ├── struct ethhdr    以太网头
    ├── struct iphdr     IP 头
    ├── struct udphdr    UDP 头
    └── message          消息内容
    */


    //把frame的地址区域映射为ethhdr,设置以太网头
    struct ethhdr *eth = (struct ethhdr *)frame;
    memcpy(eth->h_dest, dst_mac, MAC_LEN);
    memcpy(eth->h_source, src_mac, MAC_LEN);
    //表示是ipv4数据报文
    eth->h_proto = htons(ETH_P_IP);

    struct iphdr *ip = (struct iphdr *)(frame + sizeof(struct ethhdr));
    ip->version = 4;
    ip->ihl = 5;
    ip->tos = 0;
    ip->tot_len = htons((uint16_t)(sizeof(struct iphdr) + sizeof(struct udphdr) + msg_len));
    
    //分片重组的编号
    ip->id = htons(54321);
    //不切片
    ip->frag_off = 0;

    //对应的生存时间,到时候转发部分会把这个部分减去1
    ip->ttl = 64;
    ip->protocol = IPPROTO_UDP;
    //设置两个ip
    ip->saddr = src_ip;
    ip->daddr = dst_ip;
    ip->check = 0;
    ip->check = htons(ip_checksum(ip, sizeof(struct iphdr)));


    //udp头,标识两个端口
    struct udphdr *udp = (struct udphdr *)(frame + sizeof(struct ethhdr) + sizeof(struct iphdr));
    udp->source = htons((uint16_t)src_port);
    udp->dest = htons((uint16_t)dst_port);
    udp->len = htons((uint16_t)(sizeof(struct udphdr) + msg_len));
    udp->check = 0;


    //把数据放进去
    uint8_t *payload = frame + sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr);
    memcpy(payload, msg, msg_len);
    udp->check = htons(udp_checksum_ipv4(ip->saddr, ip->daddr, udp, payload, msg_len));

    //至此frame构造完毕

    //构造socket地址,表示从 sll_ifindex 对应的网卡发送出去,然后地址网卡的mac是 sll_addr 
    struct sockaddr_ll addr;
    memset(&addr, 0, sizeof(addr));
    addr.sll_family = AF_PACKET;
    addr.sll_ifindex = ifindex;
    addr.sll_halen = ETH_ALEN;
    memcpy(addr.sll_addr, dst_mac, MAC_LEN);

    size_t frame_len = sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr) + msg_len;
    if (count < 0) {
        printf("Start periodic raw UDP sender: every %d ms on %s, Ctrl+C to stop\n", interval_ms, iface);
    } else {
        printf("Start raw UDP sender: every %d ms on %s, count=%d\n", interval_ms, iface, count);
    }
    fflush(stdout);

    int sent_count = 0;
    while (count < 0 || sent_count < count) {
        ssize_t sent = sendto(sockfd, frame, frame_len, 0, (struct sockaddr *)&addr, sizeof(addr));
        if (sent < 0) {
            perror("sendto");
        } else {
            time_t now = time(NULL);
            struct tm *tm_now = localtime(&now);
            char ts[32];
            if (tm_now != NULL) {
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_now);
            } else {
                snprintf(ts, sizeof(ts), "time_error");
            }
            printf("[%s] Sent raw UDP frame: %zd bytes on %s, ", ts, sent, iface);
            print_mac(src_mac);
            printf(" -> ");
            print_mac(dst_mac);
            printf("\n");
            fflush(stdout);
            sent_count++;
        }
        if (count < 0 || sent_count < count) {
            struct timespec req;
            req.tv_sec = interval_ms / 1000;
            req.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
            nanosleep(&req, NULL);
        }
    }

    close(sockfd);
    return 0;
}
