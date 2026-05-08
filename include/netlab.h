#ifndef NETLAB_H
#define NETLAB_H

#include <stdint.h>
#include <stddef.h>

#define MAC_LEN 6

uint16_t ip_checksum(const void *data, size_t len);
int parse_mac(const char *text, uint8_t mac[MAC_LEN]);
void print_mac(const uint8_t mac[MAC_LEN]);
int get_iface_index(int sockfd, const char *ifname);
int get_iface_mac(int sockfd, const char *ifname, uint8_t mac[MAC_LEN]);
uint32_t parse_ipv4_or_die(const char *text);
int parse_cidr(const char *cidr, uint32_t *network_be, uint32_t *mask_be);

#endif
