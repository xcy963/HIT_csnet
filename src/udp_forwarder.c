#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUFSIZE 2048

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <listen_port> <dest_ip> <dest_port>\n", prog);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        usage(argv[0]);
        return 1;
    }

    int listen_port = atoi(argv[1]);
    const char *dest_ip = argv[2];
    int dest_port = atoi(argv[3]);

    //创建第一个socket,作为一个文件提供给程序访问
    //AF_INET: ipv4
    //SOCK_DGRAM: UDP数据报
    //0: UDP协议
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    //设置端口复用,可以不理解,是内核层的
    int yes = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    //构造本地的监听地址
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    //使用htons包裹,网络传输使用字节序
    local.sin_port = htons((uint16_t)listen_port);
    //监听所有网卡地址,所以只是认端口,其实就是0:0:0:0
    local.sin_addr.s_addr = INADDR_ANY;

    //socket的逻辑是bind之后,从上面的local地址发送的所有数据包都能被recvfrom读取到
    if (bind(sockfd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind");
        close(sockfd);
        return 1;
    }

    //目标的socket地址
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)dest_port);

    //把字符串的ip地址转化为二进制网络地址格式
    // 就是设置sin_addr
    if (inet_pton(AF_INET, dest_ip, &dest.sin_addr) != 1) {
        fprintf(stderr, "Invalid dest_ip: %s\n", dest_ip);
        close(sockfd);
        return 1;
    }

    printf("UDP forwarder: listen 0.0.0.0:%d -> %s:%d\n", listen_port, dest_ip, dest_port);
    while (1) {
        char buf[BUFSIZE];
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        //之前bind过了,所以这个可以被读取到,给一个src保存是发送数据的人的信息
        ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr *)&src, &src_len);
        if (n < 0) {
            perror("recvfrom");
            continue;
        }
        //解析发送方的p地址
        char src_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &src.sin_addr, src_ip, sizeof(src_ip));

        // 打印从谁发送,然后端口号也在这里了
        printf("Received %zd bytes from %s:%u, forwarding...\n", n, src_ip, ntohs(src.sin_port));
        if (sendto(sockfd, buf, (size_t)n, 0, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
            perror("sendto");
        }
    }
}
