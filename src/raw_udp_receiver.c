#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BUFSIZE 65536
#define DEFAULT_IFACE "eth0"
#define DEFAULT_LISTEN_PORT 12345

static void usage(const char *prog) {
    fprintf(stderr, "Usage: sudo %s <iface> <listen_port>\n", prog);
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

static void mac_to_str(const unsigned char mac[6], char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    const char *iface = DEFAULT_IFACE;
    int port = DEFAULT_LISTEN_PORT;
    if (argc == 3) {
        iface = argv[1];
        port = atoi(argv[2]);
    }
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid listen_port: %s\n", argv[2]);
        return 1;
    }

    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
    if (sockfd < 0) {
        perror("socket(AF_PACKET)");
        return 1;
    }

    struct sockaddr_ll bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sll_family = AF_PACKET;
    bind_addr.sll_protocol = htons(ETH_P_IP);
    bind_addr.sll_ifindex = if_nametoindex(iface);
    if (bind_addr.sll_ifindex == 0) {
        perror("if_nametoindex");
        close(sockfd);
        return 1;
    }

    if (bind(sockfd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind(AF_PACKET)");
        close(sockfd);
        return 1;
    }

    printf("Raw UDP receiver on %s, UDP port %d\n", iface, port);

    while (1) {
        unsigned char buf[BUFSIZE];
        struct sockaddr_ll recv_addr;
        socklen_t recv_len = sizeof(recv_addr);
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&recv_addr, &recv_len);
        if (n < (ssize_t)(sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr))) {
            continue;
        }
        if (recv_addr.sll_pkttype == PACKET_OUTGOING) {
            continue;
        }

        struct ethhdr *eth = (struct ethhdr *)buf;
        if (ntohs(eth->h_proto) != ETH_P_IP) {
            continue;
        }

        struct iphdr *ip = (struct iphdr *)(buf + sizeof(struct ethhdr));
        if (ip->version != 4 || ip->protocol != IPPROTO_UDP || ip->ihl < 5) {
            continue;
        }

        size_t ip_header_len = (size_t)ip->ihl * 4;
        if (n < (ssize_t)(sizeof(struct ethhdr) + ip_header_len + sizeof(struct udphdr))) {
            continue;
        }

        struct udphdr *udp = (struct udphdr *)(buf + sizeof(struct ethhdr) + ip_header_len);
        if (ntohs(udp->dest) != (uint16_t)port) {
            continue;
        }

        size_t udp_len = ntohs(udp->len);
        if (udp_len < sizeof(struct udphdr)) {
            continue;
        }
        size_t payload_len = udp_len - sizeof(struct udphdr);
        unsigned char *payload = (unsigned char *)(udp + 1);

        char src_ip[INET_ADDRSTRLEN], dst_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ip->saddr, src_ip, sizeof(src_ip));
        inet_ntop(AF_INET, &ip->daddr, dst_ip, sizeof(dst_ip));

        char src_mac[18], dst_mac[18], ts[32];
        mac_to_str(eth->h_source, src_mac);
        mac_to_str(eth->h_dest, dst_mac);
        now_str(ts);

        printf("[%s] RX: MAC %s -> %s, IP %s:%u -> %s:%u, TTL=%u, payload_len=%zu\n",
               ts,
               src_mac, dst_mac,
               src_ip, ntohs(udp->source), dst_ip, ntohs(udp->dest),
               ip->ttl, payload_len);

        if (payload_len > 0) {
            size_t show_len = payload_len > 200 ? 200 : payload_len;
            char msg[201];
            memcpy(msg, payload, show_len);
            msg[show_len] = '\0';
            printf("[%s] MSG: %s\n", ts, msg);
        }
    }
}
