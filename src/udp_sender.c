#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>


/*
向某一个ip地址和端口发送对应的数据
*/

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <dest_ip> <dest_port> [message]\n", prog);
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        usage(argv[0]);
        return 1;
    }

    const char *dest_ip = argv[1];
    int dest_port = atoi(argv[2]);
    const char *message = (argc == 4) ? argv[3] : "Hello, this is a UDP datagram!";

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons((uint16_t)dest_port);
    if (inet_pton(AF_INET, dest_ip, &dest_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid dest_ip: %s\n", dest_ip);
        close(sockfd);
        return 1;
    }

    ssize_t sent = sendto(sockfd, message, strlen(message), 0,
                          (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (sent < 0) {
        perror("sendto");
        close(sockfd);
        return 1;
    }

    printf("Sent %zd bytes to %s:%d\n", sent, dest_ip, dest_port);
    close(sockfd);
    return 0;
}
