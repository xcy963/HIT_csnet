# IPv4 分组收发与转发实验代码包

本代码包面向 Linux + GCC。实验指导书中的重点是 UDP 数据报收发/转发、基于原始套接字的一网口转发、基于双网口路由表的转发。本包把不同任务拆成不同源文件，并提供 Makefile 直接编译。

## 1. 编译

```bash
sudo apt-get update
sudo apt-get install -y gcc make
make
```

编译产物在 `bin/` 目录。

## 2. 文件结构

```text
include/netlab.h              公共函数声明
src/netlab.c                  校验和、MAC/IP/接口工具函数
src/udp_sender.c              任务 5.1：UDP 发送主机程序
src/udp_forwarder.c           任务 5.1：UDP 中间转发主机程序
src/udp_receiver.c            任务 5.1 / 5.2 / 5.3：UDP 接收主机程序

src/raw_udp_sender.c          任务 5.2：构造以太网+IP+UDP 帧发送程序
src/raw_router_oneiface.c     任务 5.2：单网口原始套接字转发程序
src/raw_router_twoiface.c     任务 5.3：双网口静态路由转发程序
Makefile                      一键编译脚本
```


## 四个验收命令（对应四部分）

1. 内容1+2（UDP多主机收发与改进）：`make abc-run`
2. 内容3（单网口原始套接字转发）：`make abc-run2`
3. 内容4（不少于5主机，查表转发）：`make abc-run4`
4. 内容5+6（双网口跨网段转发+双向通信）：`make abc-run3`

> 建议每次切换实验前先执行 `make env-clean`，清理旧容器和网络。

