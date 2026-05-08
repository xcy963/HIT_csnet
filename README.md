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

## 3. 任务 5.1：使用虚拟机实现多主机 UDP 收发及转发

三台主机建议如下：

- A：源主机，运行 `udp_sender`
- B：中间主机，运行 `udp_forwarder`
- C：目的主机，运行 `udp_receiver`

先在 C 上运行：

```bash
./bin/udp_receiver 54321
```

再在 B 上运行，把收到的 12345 端口数据转发到 C 的 54321 端口：

```bash
./bin/udp_forwarder 12345 <C_IP> 54321
```

最后在 A 上发送给 B：

```bash
./bin/udp_sender <B_IP> 12345 "Hello, this is a UDP datagram!"
```

如果使用 Ubuntu 防火墙，按实验指导书类似方式放行端口：

```bash
sudo ufw allow 12345/udp
sudo ufw allow 54321/udp
```

## 4. 任务 5.2：基于单网口主机的 IP 数据报转发

对应的原始代码
- src/raw_udp_sender.c  构造完整以太网帧/IP包/UDP包并发送,原来的发送程序
- src/raw_router_oneiface.c     原来的转发程序升级版本
- src/udp_receiver.c            直接复用原来的接收程序

该任务使用 `AF_PACKET` 原始套接字，需要 root 权限。

示例拓扑：源主机、路由主机、目的主机在同一个二层网络中。源主机发出的 IP 目的地址是目的主机 IP，但以太网目的 MAC 可以先指向路由主机 MAC；路由主机收到后修改 TTL、重算 IP 头校验和、改写以太网源/目的 MAC，并转发给目的主机。

在目的主机运行 UDP 接收：

```bash
./bin/udp_receiver 12345
```

在路由主机运行单网口转发程序：

```bash
sudo ./bin/raw_router_oneiface <iface> <src_ip> <dst_ip> <dst_host_mac>
```

例如：

```bash
sudo ./bin/raw_router_oneiface ens33 192.168.1.2 192.168.1.3 00:0c:29:dd:ee:ff
```

在源主机运行原始 UDP 发送程序，其中 `<router_mac>` 是路由主机该网口的 MAC：

```bash
sudo ./bin/raw_udp_sender <iface> <router_mac> <src_ip> <dst_ip> 12345 12345 "Hello, raw IP forwarding"
```

例如：

```bash
sudo ./bin/raw_udp_sender ens33 00:0c:29:aa:bb:cc 192.168.1.2 192.168.1.3 12345 12345 "Hello, raw IP forwarding"
```

## 5. 任务 5.3：基于双网口主机的路由转发

示例拓扑：

- 源主机：`192.168.1.2/24`，默认网关指向路由主机网口 1：`192.168.1.1`
- 路由主机网口 1：`192.168.1.1/24`
- 路由主机网口 2：`192.168.2.1/24`
- 目的主机：`192.168.2.2/24`，默认网关指向路由主机网口 2：`192.168.2.1`

Linux 新系统可以用 `ip` 命令配置，示例：

```bash
# 路由主机
sudo ip addr add 192.168.1.1/24 dev eth0
sudo ip addr add 192.168.2.1/24 dev eth1
sudo ip link set eth0 up
sudo ip link set eth1 up

# 源主机
sudo ip addr add 192.168.1.2/24 dev eth0
sudo ip route add default via 192.168.1.1

# 目的主机
sudo ip addr add 192.168.2.2/24 dev eth0
sudo ip route add default via 192.168.2.1
```

在目的主机运行：

```bash
./bin/udp_receiver 12345
```

在路由主机运行双网口静态转发程序。这里的 `<host1_mac>` 是源网段中源主机 MAC，`<host2_mac>` 是目的网段中目的主机 MAC：

```bash
sudo ./bin/raw_router_twoiface eth0 192.168.1.0/24 <host1_mac> eth1 192.168.2.0/24 <host2_mac>
```

在源主机发送到目的主机：

```bash
./bin/udp_sender 192.168.2.2 12345 "Hello through two-interface router"
```

## 6. 常用排错

1. 原始套接字程序必须使用 `sudo`，否则会出现 `Operation not permitted`。
2. 用 `ip a` 查看接口名、IP 和 MAC；用 `ip route` 查看路由。
3. 如果虚拟机收不到包，检查虚拟网卡是否在同一网络、是否混杂模式/桥接模式配置正确。
4. `raw_router_twoiface` 为教学用静态 MAC 转发，不实现 ARP。因此需要在命令行中手动填入目标主机 MAC。
5. 抓包建议：

```bash
sudo tcpdump -i <iface> -n -e udp or ip
```

## 7. 清理

```bash
make clean
```
