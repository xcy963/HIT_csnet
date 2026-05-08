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
#include <unistd.h>

#define BUFSIZE 1518

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: sudo %s <iface> <dst_mac> <src_ip> <dst_ip> <src_port> <dst_port> [message]\n"
            "Example: sudo %s ens33 00:0c:29:aa:bb:cc 192.168.1.2 192.168.1.3 12345 12345 hello\n",
            prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 7 || argc > 8) {
        usage(argv[0]);
        return 1;
    }

    const char *iface = argv[1];
    uint8_t dst_mac[MAC_LEN];
    if (parse_mac(argv[2], dst_mac) != 0) {
        fprintf(stderr, "Invalid dst_mac: %s\n", argv[2]);
        return 1;
    }
    uint32_t src_ip = parse_ipv4_or_die(argv[3]);
    uint32_t dst_ip = parse_ipv4_or_die(argv[4]);
    int src_port = atoi(argv[5]);
    int dst_port = atoi(argv[6]);
    const char *msg = (argc == 8) ? argv[7] : "Hello, this is a test message.";
    size_t msg_len = strlen(msg);

    if (msg_len > 1400) {
        fprintf(stderr, "Message too long for this simple Ethernet frame example\n");
        return 1;
    }

    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sockfd < 0) {
        perror("socket(AF_PACKET)");
        return 1;
    }

    int ifindex = get_iface_index(sockfd, iface);
    uint8_t src_mac[MAC_LEN];
    if (ifindex < 0 || get_iface_mac(sockfd, iface, src_mac) < 0) {
        close(sockfd);
        return 1;
    }

    unsigned char frame[BUFSIZE];
    memset(frame, 0, sizeof(frame));

    struct ethhdr *eth = (struct ethhdr *)frame;
    memcpy(eth->h_dest, dst_mac, MAC_LEN);
    memcpy(eth->h_source, src_mac, MAC_LEN);
    eth->h_proto = htons(ETH_P_IP);

    struct iphdr *ip = (struct iphdr *)(frame + sizeof(struct ethhdr));
    ip->version = 4;
    ip->ihl = 5;
    ip->tos = 0;
    ip->tot_len = htons((uint16_t)(sizeof(struct iphdr) + sizeof(struct udphdr) + msg_len));
    ip->id = htons(54321);
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_UDP;
    ip->saddr = src_ip;
    ip->daddr = dst_ip;
    ip->check = 0;
    ip->check = ip_checksum(ip, sizeof(struct iphdr));

    struct udphdr *udp = (struct udphdr *)(frame + sizeof(struct ethhdr) + sizeof(struct iphdr));
    udp->source = htons((uint16_t)src_port);
    udp->dest = htons((uint16_t)dst_port);
    udp->len = htons((uint16_t)(sizeof(struct udphdr) + msg_len));
    udp->check = 0; /* UDP checksum is optional for IPv4. */

    memcpy(frame + sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr), msg, msg_len);

    struct sockaddr_ll addr;
    memset(&addr, 0, sizeof(addr));
    addr.sll_family = AF_PACKET;
    addr.sll_ifindex = ifindex;
    addr.sll_halen = ETH_ALEN;
    memcpy(addr.sll_addr, dst_mac, MAC_LEN);

    size_t frame_len = sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr) + msg_len;
    ssize_t sent = sendto(sockfd, frame, frame_len, 0, (struct sockaddr *)&addr, sizeof(addr));
    if (sent < 0) {
        perror("sendto");
        close(sockfd);
        return 1;
    }

    printf("Sent raw UDP frame: %zd bytes on %s, ", sent, iface);
    print_mac(src_mac);
    printf(" -> ");
    print_mac(dst_mac);
    printf("\n");
    close(sockfd);
    return 0;
}
