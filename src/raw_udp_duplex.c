#define _POSIX_C_SOURCE 200809L
#include "netlab.h"

#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define BUFSIZE 65536
#ifndef NODE_NAME
#define NODE_NAME "node"
#endif
#ifndef NODE_IFACE
#define NODE_IFACE "eth0"
#endif
#ifndef NODE_LISTEN_PORT
#define NODE_LISTEN_PORT 12345
#endif
#ifndef NODE_NEXT_HOP_MAC
#define NODE_NEXT_HOP_MAC "02:42:ac:1e:00:00"
#endif
#ifndef NODE_SRC_IP
#define NODE_SRC_IP "192.168.1.1"
#endif
#ifndef NODE_DST_IP
#define NODE_DST_IP "192.168.1.2"
#endif
#ifndef NODE_SRC_PORT
#define NODE_SRC_PORT 12345
#endif
#ifndef NODE_DST_PORT
#define NODE_DST_PORT 12345
#endif
#ifndef NODE_MESSAGE
#define NODE_MESSAGE "hello"
#endif
#ifndef NODE_SEND_DELAY_SEC
#define NODE_SEND_DELAY_SEC 5
#endif
#ifndef NODE_SEND_INTERVAL_MS
#define NODE_SEND_INTERVAL_MS 2000
#endif
#ifndef NODE_SEND_COUNT
#define NODE_SEND_COUNT 5
#endif

static void now_str(char out[32]) { time_t t=time(NULL); struct tm *tm_now=localtime(&t); if(!tm_now){snprintf(out,32,"time_error");return;} strftime(out,32,"%F %T",tm_now);} 
static void mac_to_str(const unsigned char mac[6], char out[18]) { snprintf(out,18,"%02x:%02x:%02x:%02x:%02x:%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]); }

static void *recv_thread(void *arg) {
    (void)arg;
    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP));
    if (sockfd < 0) { perror("recv socket"); return NULL; }

    struct sockaddr_ll bind_addr = {0};
    bind_addr.sll_family = AF_PACKET;
    bind_addr.sll_protocol = htons(ETH_P_IP);
    bind_addr.sll_ifindex = if_nametoindex(NODE_IFACE);
    if (bind_addr.sll_ifindex == 0) { perror("if_nametoindex"); close(sockfd); return NULL; }
    if (bind(sockfd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) { perror("recv bind"); close(sockfd); return NULL; }

    printf("[%s] recv on %s udp/%d\n", NODE_NAME, NODE_IFACE, NODE_LISTEN_PORT);
    while (1) {
        unsigned char buf[BUFSIZE];
        struct sockaddr_ll recv_addr; socklen_t recv_len = sizeof(recv_addr);
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&recv_addr, &recv_len);
        if (n < (ssize_t)(sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct udphdr))) continue;
        if (recv_addr.sll_pkttype == PACKET_OUTGOING) continue;

        struct ethhdr *eth = (struct ethhdr *)buf;
        if (ntohs(eth->h_proto) != ETH_P_IP) continue;
        struct iphdr *ip = (struct iphdr *)(buf + sizeof(struct ethhdr));
        if (ip->version != 4 || ip->protocol != IPPROTO_UDP || ip->ihl < 5) continue;

        size_t ihl = (size_t)ip->ihl * 4;
        if (n < (ssize_t)(sizeof(struct ethhdr) + ihl + sizeof(struct udphdr))) continue;
        struct udphdr *udp = (struct udphdr *)(buf + sizeof(struct ethhdr) + ihl);
        if (ntohs(udp->dest) != (uint16_t)NODE_LISTEN_PORT) continue;

        size_t udp_len = ntohs(udp->len); if (udp_len < sizeof(struct udphdr)) continue;
        size_t payload_len = udp_len - sizeof(struct udphdr);
        unsigned char *payload = (unsigned char *)(udp + 1);

        char src_ip[INET_ADDRSTRLEN], dst_ip[INET_ADDRSTRLEN], sm[18], dm[18], ts[32];
        inet_ntop(AF_INET, &ip->saddr, src_ip, sizeof(src_ip));
        inet_ntop(AF_INET, &ip->daddr, dst_ip, sizeof(dst_ip));
        mac_to_str(eth->h_source, sm); mac_to_str(eth->h_dest, dm); now_str(ts);
        printf("[%s][%s] RX MAC %s->%s IP %s:%u->%s:%u TTL=%u len=%zu\n", ts, NODE_NAME, sm, dm, src_ip, ntohs(udp->source), dst_ip, ntohs(udp->dest), ip->ttl, payload_len);
        if (payload_len > 0) {
            size_t show = payload_len > 200 ? 200 : payload_len; char msg[201]; memcpy(msg, payload, show); msg[show] = '\0';
            printf("[%s][%s] MSG: %s\n", ts, NODE_NAME, msg);
        }
    }
}

static void *send_thread(void *arg) {
    (void)arg;
    sleep(NODE_SEND_DELAY_SEC);
    char ts[32];

    const char *next_hop_mac = getenv("NODE_NEXT_HOP_MAC");
    if (next_hop_mac == NULL || *next_hop_mac == '\0') next_hop_mac = NODE_NEXT_HOP_MAC;
    uint8_t dst_mac[MAC_LEN];
    if (parse_mac(next_hop_mac, dst_mac) != 0) { fprintf(stderr, "[%s] bad mac\n", NODE_NAME); return NULL; }
    uint32_t src_ip = parse_ipv4_or_die(NODE_SRC_IP);
    uint32_t dst_ip = parse_ipv4_or_die(NODE_DST_IP);
    const char *iface = NODE_IFACE;
    const char *msg = getenv("NODE_MESSAGE");
    if (msg == NULL || *msg == '\0') msg = NODE_MESSAGE;
    size_t msg_len = strlen(msg);

    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sockfd < 0) { perror("send socket"); return NULL; }
    int ifindex = get_iface_index(sockfd, iface);
    uint8_t src_mac[MAC_LEN];
    if (ifindex < 0 || get_iface_mac(sockfd, iface, src_mac) < 0) { close(sockfd); return NULL; }

    unsigned char frame[1518] = {0};
    struct ethhdr *eth = (struct ethhdr *)frame;
    memcpy(eth->h_dest, dst_mac, MAC_LEN); memcpy(eth->h_source, src_mac, MAC_LEN); eth->h_proto = htons(ETH_P_IP);
    struct iphdr *ip = (struct iphdr *)(frame + sizeof(struct ethhdr));
    ip->version=4; ip->ihl=5; ip->tos=0; ip->tot_len=htons((uint16_t)(sizeof(struct iphdr)+sizeof(struct udphdr)+msg_len)); ip->id=htons(54321); ip->frag_off=0; ip->ttl=64; ip->protocol=IPPROTO_UDP; ip->saddr=src_ip; ip->daddr=dst_ip; ip->check=0; ip->check=htons(ip_checksum(ip,sizeof(struct iphdr)));
    struct udphdr *udp = (struct udphdr *)(frame + sizeof(struct ethhdr) + sizeof(struct iphdr));
    udp->source=htons((uint16_t)NODE_SRC_PORT); udp->dest=htons((uint16_t)NODE_DST_PORT); udp->len=htons((uint16_t)(sizeof(struct udphdr)+msg_len)); udp->check=0;
    uint8_t *payload = frame + sizeof(struct ethhdr)+sizeof(struct iphdr)+sizeof(struct udphdr); memcpy(payload,msg,msg_len);

    struct sockaddr_ll addr = {0};
    addr.sll_family=AF_PACKET; addr.sll_ifindex=ifindex; addr.sll_halen=ETH_ALEN; memcpy(addr.sll_addr,dst_mac,MAC_LEN);
    size_t frame_len = sizeof(struct ethhdr)+sizeof(struct iphdr)+sizeof(struct udphdr)+msg_len;

    for (int i=0;i<NODE_SEND_COUNT;i++) {
        ssize_t sent = sendto(sockfd, frame, frame_len, 0, (struct sockaddr *)&addr, sizeof(addr));
        now_str(ts);
        if (sent < 0) perror("sendto");
        else printf("[%s][%s] TX %zd bytes: %s\n", ts, NODE_NAME, sent, msg);
        if (i + 1 < NODE_SEND_COUNT) {
            struct timespec req;
            req.tv_sec = NODE_SEND_INTERVAL_MS / 1000;
            req.tv_nsec = (long)(NODE_SEND_INTERVAL_MS % 1000) * 1000000L;
            nanosleep(&req, NULL);
        }
    }
    close(sockfd);
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    pthread_t tr, ts;
    pthread_create(&tr, NULL, recv_thread, NULL);
    pthread_create(&ts, NULL, send_thread, NULL);
    pthread_join(ts, NULL);
    sleep(2);
    return 0;
}
