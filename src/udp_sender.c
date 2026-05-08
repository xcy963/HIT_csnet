#include <arpa/inet.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUFSIZE 2048

/*
向某一个ip地址和端口发送对应的数据
*/

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <dest_ip> <dest_port>\n", prog);
}

static void now_str(char *out, size_t out_sz) {
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    if (tm_now == NULL) {
        snprintf(out, out_sz, "time_error");
        return;
    }
    strftime(out, out_sz, "%F %T", tm_now);
}

int main(int argc, char **argv) {
    if (argc != 3) {
        usage(argv[0]);
        return 1;
    }

    const char *dest_ip = argv[1];
    int dest_port = atoi(argv[2]);

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

    printf("UDP sender ready: destination %s:%d\n", dest_ip, dest_port);
    printf("Input one message per line, Ctrl+D to exit.\n");
    while (1) {
        char buf[BUFSIZE];
        if (fgets(buf, sizeof(buf), stdin) == NULL) {
            break;
        }
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') {
            buf[len - 1] = '\0';
            len--;
        }
        if (len == 0) {
            continue;
        }

        ssize_t sent = sendto(sockfd, buf, len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (sent < 0) {
            perror("sendto");
            continue;
        }

        char ts[32];
        now_str(ts, sizeof(ts));
        printf("[%s] Sent %zd bytes to %s:%d: %s\n", ts, sent, dest_ip, dest_port, buf);
    }

    close(sockfd);
    return 0;
}
