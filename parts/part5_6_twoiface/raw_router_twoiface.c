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

#define BUFSIZE 65536
#define ROUTE_COUNT 2
#define DEFAULT_IFACE1 "eth0"
#define DEFAULT_CIDR1 "192.168.1.0/24"
#define DEFAULT_NEXT_MAC1 "02:42:ac:1e:01:10"
#define DEFAULT_IFACE2 "eth1"
#define DEFAULT_CIDR2 "192.168.4.0/24"
#define DEFAULT_NEXT_MAC2 "02:42:ac:1e:04:10"

typedef struct {
    //网卡名字
    const char *iface;
    //网卡编号
    int ifindex;
    //第一个网卡的mac
    uint8_t iface_mac[MAC_LEN];
    uint8_t next_mac[MAC_LEN];

    //这个部分类似 172.18.0.1/16
    //目标网段,字节顺序
    uint32_t network_be;
    //目标子网掩码
    uint32_t mask_be;
} route_t;

// static void usage(const char *prog) {
//     fprintf(stderr,
//             "Usage: sudo %s <iface1> <cidr1> <next_mac1> <iface2> <cidr2> <next_mac2>\n"
//             "Example: sudo %s eth0 192.168.1.0/24 00:0c:29:11:22:33 eth1 192.168.2.0/24 00:0c:29:44:55:66\n"
//             "next_mac is the destination host MAC on that directly connected network.\n",
//             prog, prog);
// }

static void ip_to_str(uint32_t ip_be, char out[INET_ADDRSTRLEN]) {
    struct in_addr a;
    a.s_addr = ip_be;
    inet_ntop(AF_INET, &a, out, INET_ADDRSTRLEN);
}

static void mac_to_str(const uint8_t mac[MAC_LEN], char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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
    const char *iface1 = DEFAULT_IFACE1;
    const char *cidr1 = DEFAULT_CIDR1;
    const char *next1 = DEFAULT_NEXT_MAC1;
    const char *iface2 = DEFAULT_IFACE2;
    const char *cidr2 = DEFAULT_CIDR2;
    const char *next2 = DEFAULT_NEXT_MAC2;

    //因为这个路由程序是需要复用的,所以还是读一点参数进来好
    if (argc == 7) {
        iface1 = argv[1];
        cidr1 = argv[2];
        next1 = argv[3];
        iface2 = argv[4];
        cidr2 = argv[5];
        next2 = argv[6];
    } else {
        const char *v = getenv("RTR_IFACE1"); if (v && *v) iface1 = v;
        v = getenv("RTR_CIDR1"); if (v && *v) cidr1 = v;
        v = getenv("RTR_NEXTMAC1"); if (v && *v) next1 = v;
        v = getenv("RTR_IFACE2"); if (v && *v) iface2 = v;
        v = getenv("RTR_CIDR2"); if (v && *v) cidr2 = v;
        v = getenv("RTR_NEXTMAC2"); if (v && *v) next2 = v;
    }

    //创建socket
    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
    if (sockfd < 0) {
        perror("socket(AF_PACKET)");
        return 1;
    }

    //新建一个路由表
    route_t routes[ROUTE_COUNT];
    memset(routes, 0, sizeof(routes));
    routes[0].iface = iface1;
    routes[1].iface = iface2;
    //192.168.1.0/24 解析

    if (parse_cidr(cidr1, &routes[0].network_be, &routes[0].mask_be) != 0 ||
        parse_cidr(cidr2, &routes[1].network_be, &routes[1].mask_be) != 0) {
        fprintf(stderr, "CIDR must look like 192.168.1.0/24\n");
        close(sockfd);
        return 1;
    }
    if (parse_mac(next1, routes[0].next_mac) != 0 || parse_mac(next2, routes[1].next_mac) != 0) {
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
        printf("  route %d: %s via iface %s, next MAC ", i + 1, (i == 0 ? cidr1 : cidr2), routes[i].iface);
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

        //设置日志
        char src_str[INET_ADDRSTRLEN], dst_str[INET_ADDRSTRLEN], time_str[64];
        char src_mac[18], dst_mac[18];
        ip_to_str(ip->saddr, src_str);
        ip_to_str(ip->daddr, dst_str);
        mac_to_str(eth->h_source, src_mac);
        mac_to_str(eth->h_dest, dst_mac);

        uint16_t src_port = 0;
        uint16_t dst_port = 0;
        const char *l4_proto = "OTHER";
        size_t ip_header_len = (size_t)ip->ihl * 4;
        if (n >= (ssize_t)(sizeof(struct ethhdr) + ip_header_len + sizeof(struct udphdr))) {
            const uint8_t *l4 = (const uint8_t *)ip + ip_header_len;
            if (ip->protocol == IPPROTO_UDP || ip->protocol == IPPROTO_TCP) {
                // Both UDP/TCP start with src_port (2B), dst_port (2B)
                src_port = ((uint16_t)l4[0] << 8) | l4[1];
                dst_port = ((uint16_t)l4[2] << 8) | l4[3];
                l4_proto = (ip->protocol == IPPROTO_UDP) ? "UDP" : "TCP";
            }
        }

        time_t now = time(NULL);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
        printf("[%s] RX MAC %s -> %s, IP %s -> %s, TTL %u, %s %u -> %u, in ifindex %d, out %s\n",
               time_str, src_mac, dst_mac, src_str, dst_str, ip->ttl,
               l4_proto, src_port, dst_port, recv_addr.sll_ifindex, route->iface);

        ip->ttl -= 1;
        ip->check = 0;
        ip->check = htons(ip_checksum(ip, (size_t)ip->ihl * 4));
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
