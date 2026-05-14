#include "netlab.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <netinet/ip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BUFSIZE 65536
#define MAX_ROUTES 256
#define DEFAULT_IFACE "eth0"
#define DEFAULT_ROUTE_FILE "/tmp/route.tbl"

struct route_entry {
    uint32_t dst_ip_be;
    uint8_t next_mac[MAC_LEN];
};

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: sudo %s <iface> <route_table_file>\n"
            "Route format (one per line): <dst_ip> <next_hop_mac>\n"
            "Example line: 192.168.1.5 02:42:ac:1e:00:45\n",
            prog);
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

static void ip_to_str(uint32_t ip_be, char out[INET_ADDRSTRLEN]) {
    struct in_addr a;
    a.s_addr = ip_be;
    inet_ntop(AF_INET, &a, out, INET_ADDRSTRLEN);
}

static void mac_to_str(const uint8_t mac[MAC_LEN], char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

static int load_routes(const char *path, struct route_entry routes[MAX_ROUTES]) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        perror("fopen(route_table_file)");
        return -1;
    }

    int count = 0;
    char line[256];
    int line_no = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        line_no++;
        trim(line);
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') {
            continue;
        }

        char ip_text[64];
        char mac_text[64];
        if (sscanf(p, "%63s %63s", ip_text, mac_text) != 2) {
            fprintf(stderr, "Invalid route line %d: %s\n", line_no, line);
            fclose(fp);
            return -1;
        }
        if (count >= MAX_ROUTES) {
            fprintf(stderr, "Too many routes (max=%d)\n", MAX_ROUTES);
            fclose(fp);
            return -1;
        }

        routes[count].dst_ip_be = parse_ipv4_or_die(ip_text);
        if (parse_mac(mac_text, routes[count].next_mac) != 0) {
            fprintf(stderr, "Invalid MAC in line %d: %s\n", line_no, mac_text);
            fclose(fp);
            return -1;
        }
        count++;
    }

    fclose(fp);
    return count;
}

static const struct route_entry *lookup_route(uint32_t dst_ip_be,
                                               const struct route_entry routes[MAX_ROUTES],
                                               int route_count) {
    for (int i = 0; i < route_count; i++) {
        if (routes[i].dst_ip_be == dst_ip_be) {
            return &routes[i];
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *iface = DEFAULT_IFACE;
    const char *route_file = DEFAULT_ROUTE_FILE;
    if (argc == 3) {
        iface = argv[1];
        route_file = argv[2];
    }

    struct route_entry routes[MAX_ROUTES];
    int route_count = load_routes(route_file, routes);
    if (route_count <= 0) {
        fprintf(stderr, "No valid routes loaded from %s\n", route_file);
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

    printf("Table router on %s, loaded %d route(s) from %s\n", iface, route_count, route_file);

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

        const struct route_entry *route = lookup_route(ip->daddr, routes, route_count);
        if (route == NULL) {
            printf("[%s] Drop: no route for dst=%s\n", ts, dst_ip);
            continue;
        }
        if (ip->ttl <= 1) {
            printf("[%s] Drop: TTL expired\n", ts);
            continue;
        }

        uint8_t old_ttl = ip->ttl;
        ip->ttl -= 1;
        ip->check = 0;
        ip->check = htons(ip_checksum(ip, (size_t)ip->ihl * 4));

        memcpy(eth->h_dest, route->next_mac, MAC_LEN);
        memcpy(eth->h_source, iface_mac, MAC_LEN);

        struct sockaddr_ll send_addr;
        memset(&send_addr, 0, sizeof(send_addr));
        send_addr.sll_family = AF_PACKET;
        send_addr.sll_ifindex = ifindex;
        send_addr.sll_halen = ETH_ALEN;
        memcpy(send_addr.sll_addr, route->next_mac, MAC_LEN);

        if (sendto(sockfd, buf, (size_t)n, 0, (struct sockaddr *)&send_addr, sizeof(send_addr)) < 0) {
            perror("sendto");
            continue;
        }

        char out_src_mac[18], out_dst_mac[18], nh_mac[18];
        mac_to_str(eth->h_source, out_src_mac);
        mac_to_str(eth->h_dest, out_dst_mac);
        mac_to_str(route->next_mac, nh_mac);
        now_str(ts);
        printf("[%s] TX IPv4: MAC %s -> %s, IP %s -> %s, TTL=%u, next-hop=%s\n",
               ts, out_src_mac, out_dst_mac, src_ip, dst_ip, old_ttl - 1, nh_mac);
    }
}
