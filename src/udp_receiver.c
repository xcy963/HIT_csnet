#include <arpa/inet.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUFSIZE 2048

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <listen_port>\n", prog);
}
//构造时间的函数
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
    if (argc != 2) {
        usage(argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);

    //设置socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    int yes = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    //监听的端口设置
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sockfd);
        return 1;
    }

    printf("UDP receiver listening on port %d\n", port);
    while (1) {
        char buf[BUFSIZE];
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&src, &src_len);
        if (n < 0) {
            perror("recvfrom");
            continue;
        }
        //设置字符串结尾,不然会有很多0
        buf[n] = '\0';
        char src_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src.sin_addr, src_ip, sizeof(src_ip));
        char ts[32];
        //添加显示时间的逻辑
        now_str(ts, sizeof(ts));
        printf("[%s] Received %zd bytes from %s:%u: %s\n", ts, n, src_ip, ntohs(src.sin_port), buf);
    }
}
