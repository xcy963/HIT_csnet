#include <arpa/inet.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUFSIZE 2048
#define DEFAULT_DEST_IP "192.168.1.2"
#define DEFAULT_DEST_PORT 12345
#define DEFAULT_MESSAGE "Hello from udp_sender"

/*
向某一个ip地址和端口发送对应的数据
*/

// static void usage(const char *prog) {
//     fprintf(stderr, "Usage: %s <dest_ip> <dest_port>\n", prog);
// }

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
    const char *dest_ip = DEFAULT_DEST_IP;
    int dest_port = DEFAULT_DEST_PORT;
    // if (argc == 3) {
    //     dest_ip = argv[1];
    //     dest_port = atoi(argv[2]);
    // }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons((uint16_t)dest_port);

    //设置对应的ip 地址
    if (inet_pton(AF_INET, dest_ip, &dest_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid dest_ip: %s\n", dest_ip);
        close(sockfd);
        return 1;
    }

    if (argc == 3) {
        printf("UDP sender ready: destination %s:%d\n", dest_ip, dest_port);
        printf("Input one message per line, Ctrl+D to exit.\n");
    } else {
        printf("UDP sender default mode: %s:%d msg='%s'\n", dest_ip, dest_port, DEFAULT_MESSAGE);
    }
    while (1) {
        //获取输入内容
        char buf[BUFSIZE];
        size_t len = 0;
        if (argc == 3) {
            if (fgets(buf, sizeof(buf), stdin) == NULL) {
                break;
            }
            len = strlen(buf);
            if (len > 0 && buf[len - 1] == '\n') {
                buf[len - 1] = '\0';
                len--;
            }
            if (len == 0) {
                continue;
            }
        } else {
            strncpy(buf, DEFAULT_MESSAGE, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            len = strlen(buf);
        }

        ssize_t sent = sendto(sockfd, buf, len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (sent < 0) {
            perror("sendto");
            continue;
        }

        char ts[32];
        now_str(ts, sizeof(ts));
        printf("[%s] Sent %zd bytes to %s:%d: %s\n", ts, sent, dest_ip, dest_port, buf);
        if (argc != 3) break;
    }

    close(sockfd);
    return 0;
}
