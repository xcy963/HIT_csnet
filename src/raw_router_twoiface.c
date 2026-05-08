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
#define ROUTE_COUNT 2

typedef struct {
    const char *iface;
    int ifindex;
    uint8_t iface_mac[MAC_LEN];
    uint8_t next_mac[MAC_LEN];
    uint32_t network_be;
    uint32_t mask_be;
} route_t;

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: sudo %s <iface1> <cidr1> <next_mac1> <iface2> <cidr2> <next_mac2>\n"
            "Example: sudo %s eth0 192.168.1.0/24 00:0c:29:11:22:33 eth1 192.168.2.0/24 00:0c:29:44:55:66\n"
            "next_mac is the destination host MAC on that directly connected network.\n",
            prog, prog);
}

static void ip_to_str(uint32_t ip_be, char out[INET_ADDRSTRLEN]) {
    struct in_addr a;
    a.s_addr = ip_be;
    inet_ntop(AF_INET, &a, out, INET_ADDRSTRLEN);
}

static route_t *lookup_route(route_t routes[ROUTE_COUNT], uint32_t dst_ip_be) {
    route_t *best = NULL;
    uint32_t best_mask = 0;
    for (int i = 0; i < ROUTE_COUNT; i++) {
        if ((dst_ip_be & routes[i].mask_be) == routes[i].network_be) {
            uint32_t mask_host = ntohl(routes[i].mask_be);
            if (!best || mask_host > best_mask) {
                best = &routes[i];
                best_mask = mask_host;
            }
        }
    }
    return best;
}

static int is_own_mac(route_t routes[ROUTE_COUNT], const uint8_t mac[MAC_LEN]) {
    for (int i = 0; i < ROUTE_COUNT; i++) {
        if (memcmp(routes[i].iface_mac, mac, MAC_LEN) == 0) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 7) {
        usage(argv[0]);
        return 1;
    }

    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
    if (sockfd < 0) {
        perror("socket(AF_PACKET)");
        return 1;
    }

    route_t routes[ROUTE_COUNT];
    memset(routes, 0, sizeof(routes));
    routes[0].iface = argv[1];
    routes[1].iface = argv[4];

    if (parse_cidr(argv[2], &routes[0].network_be, &routes[0].mask_be) != 0 ||
        parse_cidr(argv[5], &routes[1].network_be, &routes[1].mask_be) != 0) {
        fprintf(stderr, "CIDR must look like 192.168.1.0/24\n");
        close(sockfd);
        return 1;
    }
    if (parse_mac(argv[3], routes[0].next_mac) != 0 || parse_mac(argv[6], routes[1].next_mac) != 0) {
        fprintf(stderr, "Invalid MAC address\n");
        close(sockfd);
        return 1;
    }

    for (int i = 0; i < ROUTE_COUNT; i++) {
        routes[i].ifindex = get_iface_index(sockfd, routes[i].iface);
        if (routes[i].ifindex < 0 || get_iface_mac(sockfd, routes[i].iface, routes[i].iface_mac) < 0) {
            close(sockfd);
            return 1;
        }
    }

    printf("Two-interface static router started:\n");
    for (int i = 0; i < ROUTE_COUNT; i++) {
        printf("  route %d: %s via iface %s, next MAC ", i + 1, (i == 0 ? argv[2] : argv[5]), routes[i].iface);
        print_mac(routes[i].next_mac);
        printf(", iface MAC ");
        print_mac(routes[i].iface_mac);
        printf("\n");
    }

    while (1) {
        unsigned char buf[BUFSIZE];
        struct sockaddr_ll recv_addr;
        socklen_t recv_len = sizeof(recv_addr);
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&recv_addr, &recv_len);
        if (n < (ssize_t)(sizeof(struct ethhdr) + sizeof(struct iphdr))) continue;
        if (recv_addr.sll_pkttype == PACKET_OUTGOING) continue;

        struct ethhdr *eth = (struct ethhdr *)buf;
        if (ntohs(eth->h_proto) != ETH_P_IP) continue;
        if (is_own_mac(routes, eth->h_source)) continue;

        struct iphdr *ip = (struct iphdr *)(buf + sizeof(struct ethhdr));
        if (ip->version != 4 || ip->ihl < 5) continue;

        route_t *route = lookup_route(routes, ip->daddr);
        if (!route) continue;
        if (route->ifindex == recv_addr.sll_ifindex) {
            /* Already on the destination side; avoid bouncing frames. */
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
        printf("[%s] %s -> %s, in ifindex %d, out %s, TTL %u -> %u\n",
               time_str, src_str, dst_str, recv_addr.sll_ifindex, route->iface, ip->ttl, ip->ttl - 1);

        ip->ttl -= 1;
        ip->check = 0;
        ip->check = ip_checksum(ip, (size_t)ip->ihl * 4);
        memcpy(eth->h_dest, route->next_mac, MAC_LEN);
        memcpy(eth->h_source, route->iface_mac, MAC_LEN);

        struct sockaddr_ll send_addr;
        memset(&send_addr, 0, sizeof(send_addr));
        send_addr.sll_family = AF_PACKET;
        send_addr.sll_ifindex = route->ifindex;
        send_addr.sll_halen = ETH_ALEN;
        memcpy(send_addr.sll_addr, route->next_mac, MAC_LEN);

        if (sendto(sockfd, buf, (size_t)n, 0, (struct sockaddr *)&send_addr, sizeof(send_addr)) < 0) {
            perror("sendto");
        }
    }
}
