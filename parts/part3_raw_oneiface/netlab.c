#include "netlab.h"

#include <arpa/inet.h>
#include <linux/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

//ip校验和
uint16_t ip_checksum(const void *data, size_t len) {
    const uint8_t *buf = (const uint8_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += ((uint16_t)buf[0] << 8) | buf[1];
        buf += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += ((uint16_t)buf[0] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xffffU) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

int parse_mac(const char *text, uint8_t mac[MAC_LEN]) {
    unsigned int v[MAC_LEN];
    if (sscanf(text, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return -1;
    }
    for (int i = 0; i < MAC_LEN; i++) {
        if (v[i] > 0xffU) return -1;
        mac[i] = (uint8_t)v[i];
    }
    return 0;
}

void print_mac(const uint8_t mac[MAC_LEN]) {
    printf("%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int get_iface_index(int sockfd, const char *ifname) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(sockfd, SIOCGIFINDEX, &ifr) < 0) {
        perror("ioctl(SIOCGIFINDEX)");
        return -1;
    }
    return ifr.ifr_ifindex;
}

int get_iface_mac(int sockfd, const char *ifname, uint8_t mac[MAC_LEN]) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(sockfd, SIOCGIFHWADDR, &ifr) < 0) {
        perror("ioctl(SIOCGIFHWADDR)");
        return -1;
    }
    memcpy(mac, ifr.ifr_hwaddr.sa_data, MAC_LEN);
    return 0;
}

uint32_t parse_ipv4_or_die(const char *text) {
    struct in_addr addr;
    if (inet_pton(AF_INET, text, &addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", text);
        exit(EXIT_FAILURE);
    }
    return addr.s_addr;
}

int parse_cidr(const char *cidr, uint32_t *network_be, uint32_t *mask_be) {
    char ip[64];
    const char *slash = strchr(cidr, '/');
    if (!slash) return -1;
    size_t n = (size_t)(slash - cidr);
    if (n == 0 || n >= sizeof(ip)) return -1;
    memcpy(ip, cidr, n);
    ip[n] = '\0';

    char *endptr = NULL;
    long prefix = strtol(slash + 1, &endptr, 10);
    if (*endptr != '\0' || prefix < 0 || prefix > 32) return -1;

    uint32_t addr_be = parse_ipv4_or_die(ip);
    uint32_t mask_host = prefix == 0 ? 0 : (0xffffffffU << (32 - prefix));
    uint32_t mask = htonl(mask_host);
    *mask_be = mask;
    *network_be = addr_be & mask;
    return 0;
}
