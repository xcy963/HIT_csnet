#include "netlab.h"

#include <arpa/inet.h>
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

/*
实现第二个部分的内容,从底层封装网络帧,一个模拟路由器的发送
*/

#define BUFSIZE 65536

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: sudo %s <iface> <match_src_ip> <match_dst_ip> <next_hop_mac>\n"
            "Example: sudo %s ens33 192.168.1.2 192.168.1.3 00:0c:29:dd:ee:ff\n",
            prog, prog);
}

static void ip_to_str(uint32_t ip_be, char out[INET_ADDRSTRLEN]) {
    struct in_addr a;
    a.s_addr = ip_be;
    inet_ntop(AF_INET, &a, out, INET_ADDRSTRLEN);
}

static void mac_to_str(const uint8_t mac[MAC_LEN], char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void now_str(char out[32]) {
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    if (tm_now == NULL) {
        snprintf(out, 32, "time_error");
        return;
    }
    strftime(out, 32, "%Y-%m-%d %H:%M:%S", tm_now);
}

static int debug_enabled(void) {

    return 0;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        usage(argv[0]);
        return 1;
    }

    //iface 监听和转发使用的网卡名
    const char *iface = argv[1];
    //match_src: 要匹配的源 IP
    uint32_t match_src = parse_ipv4_or_die(argv[2]);
    // match_dst：要匹配的目的 IP
    uint32_t match_dst = parse_ipv4_or_die(argv[3]);
    
    //解析下一个地址的mac
    uint8_t next_mac[MAC_LEN];
    if (parse_mac(argv[4], next_mac) != 0) {
        fprintf(stderr, "Invalid next_hop_mac: %s\n", argv[4]);
        return 1;
    }

    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
    if (sockfd < 0) {
        perror("socket(AF_PACKET)");
        return 1;
    }

    //本机网卡的位置和mac
    int ifindex = get_iface_index(sockfd, iface);
    uint8_t iface_mac[MAC_LEN];
    if (ifindex < 0 || get_iface_mac(sockfd, iface, iface_mac) < 0) {
        close(sockfd);
        return 1;
    }

    //绑定,就是监听本地网卡上的
    /*
    sll_family   = AF_PACKET       链路层 socket
    sll_protocol = ETH_P_IP        只接收 IPv4 以太网帧
    sll_ifindex  = ifindex         只监听指定网卡
    */
    struct sockaddr_ll bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sll_family = AF_PACKET;
    bind_addr.sll_protocol = htons(ETH_P_IP);
    bind_addr.sll_ifindex = ifindex;
    if (bind(sockfd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind(AF_PACKET)");
        close(sockfd);
        return 1;
    }

    printf("One-interface router on %s. Forwarding %s -> %s to MAC ", iface, argv[2], argv[3]);
    print_mac(next_mac);
    printf("\n");
    if (debug_enabled()) {
        printf("[DEBUG] ROUTER_DEBUG enabled\n");
    }

    while (1) {
        unsigned char buf[BUFSIZE];
        struct sockaddr_ll recv_addr;
        socklen_t recv_len = sizeof(recv_addr);
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&recv_addr, &recv_len);

        /*
        之后要修改网络帧
        转发前：
        源 MAC = A 主机 MAC
        目的 MAC = B 主机 MAC

        转发后：
        源 MAC = B 主机 MAC
        目的 MAC = C 主机 MAC
        */
        if (n < (ssize_t)(sizeof(struct ethhdr) + sizeof(struct iphdr))) {
            if (debug_enabled()) {
                printf("[DEBUG] Drop: frame too short (%zd bytes)\n", n);
            }
            continue;
        }
        if (recv_addr.sll_pkttype == PACKET_OUTGOING) {
            if (debug_enabled()) {
                printf("[DEBUG] Skip: outgoing frame\n");
            }
            continue;
        }

        //修改发送 mac 改成是路由器的mac
        struct ethhdr *eth = (struct ethhdr *)buf;
        if (ntohs(eth->h_proto) != ETH_P_IP) {
            if (debug_enabled()) {
                printf("[DEBUG] Drop: non-IPv4 ethertype=0x%04x\n", ntohs(eth->h_proto));
            }
            continue;
        }

        //把目的mac改成正确的
        if (memcmp(eth->h_source, iface_mac, MAC_LEN) == 0) {
            if (debug_enabled()) {
                printf("[DEBUG] Skip: source MAC is local interface MAC\n");
            }
            continue;
        }

        struct iphdr *ip = (struct iphdr *)(buf + sizeof(struct ethhdr));
        if (ip->version != 4 || ip->ihl < 5) {
            if (debug_enabled()) {
                printf("[DEBUG] Drop: invalid IPv4 header version=%u ihl=%u\n", ip->version, ip->ihl);
            }
            continue;
        }

        //打印日志
        char ts[32];
        char src_mac_str[18], dst_mac_str[18];
        char src_ip[INET_ADDRSTRLEN], dst_ip[INET_ADDRSTRLEN];
        now_str(ts);
        mac_to_str(eth->h_source, src_mac_str);
        mac_to_str(eth->h_dest, dst_mac_str);
        ip_to_str(ip->saddr, src_ip);
        ip_to_str(ip->daddr, dst_ip);
        printf("[%s] RX IPv4: MAC %s -> %s, IP %s -> %s, TTL=%u\n",
               ts, src_mac_str, dst_mac_str, src_ip, dst_ip, ip->ttl);

        if (ip->saddr != match_src || ip->daddr != match_dst) {
            if (debug_enabled()) {
                char got_src[INET_ADDRSTRLEN], got_dst[INET_ADDRSTRLEN];
                char want_src[INET_ADDRSTRLEN], want_dst[INET_ADDRSTRLEN];
                ip_to_str(ip->saddr, got_src);
                ip_to_str(ip->daddr, got_dst);
                ip_to_str(match_src, want_src);
                ip_to_str(match_dst, want_dst);
                printf("[DEBUG] Drop: IP mismatch got %s -> %s, want %s -> %s\n",
                       got_src, got_dst, want_src, want_dst);
            }
            continue;
        }
        if (ip->ttl <= 1) {
            printf("Drop packet: TTL expired\n");
            continue;
        }

        uint8_t old_ttl = ip->ttl;

        ip->ttl -= 1;
        ip->check = 0;
        ip->check = htons(ip_checksum(ip, (size_t)ip->ihl * 4));

        memcpy(eth->h_dest, next_mac, MAC_LEN);
        memcpy(eth->h_source, iface_mac, MAC_LEN);

        struct sockaddr_ll send_addr;
        memset(&send_addr, 0, sizeof(send_addr));
        send_addr.sll_family = AF_PACKET;
        send_addr.sll_ifindex = ifindex;
        send_addr.sll_halen = ETH_ALEN;
        memcpy(send_addr.sll_addr, next_mac, MAC_LEN);

        if (sendto(sockfd, buf, (size_t)n, 0, (struct sockaddr *)&send_addr, sizeof(send_addr)) < 0) {
            perror("sendto");
        } else {
            char out_src_mac[18], out_dst_mac[18];
            mac_to_str(eth->h_source, out_src_mac);
            mac_to_str(eth->h_dest, out_dst_mac);
            now_str(ts);
            printf("[%s] TX IPv4: MAC %s -> %s, IP %s -> %s, TTL=%u\n",
                   ts, out_src_mac, out_dst_mac, src_ip, dst_ip, old_ttl - 1);
        }
    }
}
