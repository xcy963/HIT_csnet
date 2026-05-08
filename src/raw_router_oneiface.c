#include "netlab.h"

#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <netinet/ip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

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

int main(int argc, char **argv) {
    if (argc != 5) {
        usage(argv[0]);
        return 1;
    }

    const char *iface = argv[1];
    uint32_t match_src = parse_ipv4_or_die(argv[2]);
    uint32_t match_dst = parse_ipv4_or_die(argv[3]);
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

    int ifindex = get_iface_index(sockfd, iface);
    uint8_t iface_mac[MAC_LEN];
    if (ifindex < 0 || get_iface_mac(sockfd, iface, iface_mac) < 0) {
        close(sockfd);
        return 1;
    }

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

    while (1) {
        unsigned char buf[BUFSIZE];
        struct sockaddr_ll recv_addr;
        socklen_t recv_len = sizeof(recv_addr);
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&recv_addr, &recv_len);
        if (n < (ssize_t)(sizeof(struct ethhdr) + sizeof(struct iphdr))) {
            continue;
        }
        if (recv_addr.sll_pkttype == PACKET_OUTGOING) {
            continue;
        }

        struct ethhdr *eth = (struct ethhdr *)buf;
        if (ntohs(eth->h_proto) != ETH_P_IP) {
            continue;
        }
        if (memcmp(eth->h_source, iface_mac, MAC_LEN) == 0) {
            continue;
        }

        struct iphdr *ip = (struct iphdr *)(buf + sizeof(struct ethhdr));
        if (ip->version != 4 || ip->ihl < 5) {
            continue;
        }
        if (ip->saddr != match_src || ip->daddr != match_dst) {
            continue;
        }
        if (ip->ttl <= 1) {
            printf("Drop packet: TTL expired\n");
            continue;
        }

        char src_str[INET_ADDRSTRLEN], dst_str[INET_ADDRSTRLEN], time_str[64];
        ip_to_str(ip->saddr, src_str);
        ip_to_str(ip->daddr, dst_str);
        time_t now = time(NULL);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
        printf("[%s] Forward %s -> %s, TTL %u -> %u\n", time_str, src_str, dst_str, ip->ttl, ip->ttl - 1);

        ip->ttl -= 1;
        ip->check = 0;
        ip->check = ip_checksum(ip, (size_t)ip->ihl * 4);

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
        }
    }
}
