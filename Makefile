CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -O2 -g -Iinclude
LDFLAGS :=
BIN_DIR := bin
COMMON := src/netlab.c

PROGRAMS := udp_sender udp_forwarder udp_receiver raw_udp_sender raw_udp_receiver raw_router_oneiface raw_router_twoiface raw_router_table raw_udp_duplex_src raw_udp_duplex_dst
TARGETS := $(addprefix $(BIN_DIR)/,$(PROGRAMS))

DOCKER_IMAGE := hitcsnet:local
DOCKERFILE := Dockerfile
DOCKER_NET := hitcsnet_lan
DOCKER_SUBNET := 192.168.1.0/24
DOCKER_GATEWAY := 192.168.1.254

# 这些部分是考虑到他要使用双网卡,所以需要制作好多个网络环境,模拟在不同的局域网内
DOCKER_NET_A := hitcsnet_lan_a
DOCKER_SUBNET_A := 192.168.1.0/24
DOCKER_GATEWAY_A := 192.168.1.254
DOCKER_NET_B := hitcsnet_lan_b
DOCKER_SUBNET_B := 192.168.2.0/24
DOCKER_GATEWAY_B := 192.168.2.254
DOCKER_NET_C := hitcsnet_lan_c
DOCKER_SUBNET_C := 192.168.3.0/24
DOCKER_GATEWAY_C := 192.168.3.254
DOCKER_NET_D := hitcsnet_lan_d
DOCKER_SUBNET_D := 192.168.4.0/24
DOCKER_GATEWAY_D := 192.168.4.254

.PHONY: all clean run-help docker-image docker-image-rm docker-net docker-net4 abc-run abc-run2 abc-run3 abc-run4 env-clean

all: $(BIN_DIR) $(TARGETS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/udp_sender: src/udp_sender.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/udp_forwarder: src/udp_forwarder.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/udp_receiver: src/udp_receiver.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(BIN_DIR)/raw_udp_sender: src/raw_udp_sender.c $(COMMON) include/netlab.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ src/raw_udp_sender.c $(COMMON) $(LDFLAGS)

$(BIN_DIR)/raw_udp_receiver: src/raw_udp_receiver.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ src/raw_udp_receiver.c $(LDFLAGS)

$(BIN_DIR)/raw_router_oneiface: src/raw_router_oneiface.c $(COMMON) include/netlab.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ src/raw_router_oneiface.c $(COMMON) $(LDFLAGS)

$(BIN_DIR)/raw_router_twoiface: src/raw_router_twoiface.c $(COMMON) include/netlab.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ src/raw_router_twoiface.c $(COMMON) $(LDFLAGS)

$(BIN_DIR)/raw_router_table: src/raw_router_table.c $(COMMON) include/netlab.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ src/raw_router_table.c $(COMMON) $(LDFLAGS)

$(BIN_DIR)/raw_udp_duplex_src: src/raw_udp_duplex.c $(COMMON) include/netlab.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -pthread \
		-DNODE_NAME=\"raw3-src\" -DNODE_IFACE=\"eth0\" -DNODE_LISTEN_PORT=12346 \
		-DNODE_NEXT_HOP_MAC=\"02:42:ac:1e:02:01\" -DNODE_SRC_IP=\"192.168.1.1\" -DNODE_DST_IP=\"192.168.4.2\" \
		-DNODE_SRC_PORT=12345 -DNODE_DST_PORT=12345 \
		-DNODE_SEND_DELAY_SEC=5 -DNODE_SEND_INTERVAL_MS=2000 -DNODE_SEND_COUNT=5 \
		-o $@ src/raw_udp_duplex.c $(COMMON) $(LDFLAGS)

$(BIN_DIR)/raw_udp_duplex_dst: src/raw_udp_duplex.c $(COMMON) include/netlab.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -pthread \
		-DNODE_NAME=\"raw3-dst\" -DNODE_IFACE=\"eth0\" -DNODE_LISTEN_PORT=12345 \
		-DNODE_NEXT_HOP_MAC=\"02:42:ac:1e:04:01\" -DNODE_SRC_IP=\"192.168.4.2\" -DNODE_DST_IP=\"192.168.1.1\" \
		-DNODE_SRC_PORT=12345 -DNODE_DST_PORT=12346 \
		-DNODE_SEND_DELAY_SEC=8 -DNODE_SEND_INTERVAL_MS=2000 -DNODE_SEND_COUNT=5 \
		-o $@ src/raw_udp_duplex.c $(COMMON) $(LDFLAGS)

run-help:
	@echo "Build binaries:      make"
	@echo "Build image:         make docker-image"
	@echo "Remove image:        make docker-image-rm"
	@echo "Run UDP forward:     make abc-run"
	@echo "Run raw 1-router:    make abc-run2"
	@echo "Run raw 3-router:    make abc-run3"
	@echo "Run raw table 5-host:make abc-run4"
	@echo "Clean docker env:    make env-clean"

clean:
	rm -rf $(BIN_DIR)

docker-image:
	docker build -t $(DOCKER_IMAGE) -f $(DOCKERFILE) .

docker-image-rm:
	@docker rmi -f $(DOCKER_IMAGE) >/dev/null 2>&1 || true

docker-net:
	@docker network inspect $(DOCKER_NET) >/dev/null 2>&1 || \
		docker network create --driver bridge --subnet $(DOCKER_SUBNET) --gateway $(DOCKER_GATEWAY) $(DOCKER_NET)

docker-net4:
	@docker network inspect $(DOCKER_NET_A) >/dev/null 2>&1 || \
		docker network create --driver bridge --subnet $(DOCKER_SUBNET_A) --gateway $(DOCKER_GATEWAY_A) $(DOCKER_NET_A)
	@docker network inspect $(DOCKER_NET_B) >/dev/null 2>&1 || \
		docker network create --driver bridge --subnet $(DOCKER_SUBNET_B) --gateway $(DOCKER_GATEWAY_B) $(DOCKER_NET_B)
	@docker network inspect $(DOCKER_NET_C) >/dev/null 2>&1 || \
		docker network create --driver bridge --subnet $(DOCKER_SUBNET_C) --gateway $(DOCKER_GATEWAY_C) $(DOCKER_NET_C)
	@docker network inspect $(DOCKER_NET_D) >/dev/null 2>&1 || \
		docker network create --driver bridge --subnet $(DOCKER_SUBNET_D) --gateway $(DOCKER_GATEWAY_D) $(DOCKER_NET_D)

env-clean:
	@docker rm -f client router server raw-src raw-rtr raw-dst raw3-src raw3-r1 raw3-r2 raw3-r3 raw3-dst raw4-src raw4-r1 raw4-r2 raw4-r3 raw4-dst >/dev/null 2>&1 || true
	@docker network rm $(DOCKER_NET) $(DOCKER_NET_A) $(DOCKER_NET_B) $(DOCKER_NET_C) $(DOCKER_NET_D) >/dev/null 2>&1 || true

abc-run: all docker-image docker-net
	@docker rm -f client router server >/dev/null 2>&1 || true
	@if command -v gnome-terminal >/dev/null 2>&1; then \
		gnome-terminal --title="abc-run client-recv" -- bash -lc "docker run --rm -it --name client --hostname client --network $(DOCKER_NET) --ip 192.168.1.3 -v '$(CURDIR)':/work -w /work $(DOCKER_IMAGE) ./bin/udp_receiver; echo; echo '[client exited] Press Enter to close'; read _"; \
		gnome-terminal --title="abc-run router-fwd" -- bash -lc "docker run --rm -it --name router --hostname router --network $(DOCKER_NET) --ip 192.168.1.2 -v '$(CURDIR)':/work -w /work $(DOCKER_IMAGE) ./bin/udp_forwarder; echo; echo '[router exited] Press Enter to close'; read _"; \
		gnome-terminal --title="abc-run server-send" -- bash -lc "docker run --rm -it --name server --hostname server --network $(DOCKER_NET) --ip 192.168.1.1 -v '$(CURDIR)':/work -w /work $(DOCKER_IMAGE) ./bin/udp_sender 192.168.1.2 12345; echo; echo '[server exited] Press Enter to close'; read _"; \
	else \
		echo "No supported terminal found. Please install gnome-terminal."; \
		exit 1; \
	fi

abc-run2: all docker-image docker-net
	@docker rm -f raw-src raw-rtr raw-dst >/dev/null 2>&1 || true
	@if command -v gnome-terminal >/dev/null 2>&1; then \
		gnome-terminal --title="abc-run2 dst-recv" -- bash -lc "docker run --rm -it --name raw-dst --hostname dst --network $(DOCKER_NET) --ip 192.168.1.3 --mac-address 02:42:ac:1e:00:31 --cap-add NET_RAW --cap-add NET_ADMIN -v '$(CURDIR)':/work -w /work $(DOCKER_IMAGE) ./bin/udp_receiver; echo; echo '[dst exited] Press Enter to close'; read _"; \
		gnome-terminal --title="abc-run2 router-oneiface" -- bash -lc "docker run --rm -it --name raw-rtr --hostname rtr --network $(DOCKER_NET) --ip 192.168.1.2 --mac-address 02:42:ac:1e:00:32 --cap-add NET_RAW --cap-add NET_ADMIN -v '$(CURDIR)':/work -w /work $(DOCKER_IMAGE) ./bin/raw_router_oneiface; echo; echo '[rtr exited] Press Enter to close'; read _"; \
		gnome-terminal --title="abc-run2 src-send" -- bash -lc "sleep 1; docker run --rm -it --name raw-src --hostname src --network $(DOCKER_NET) --ip 192.168.1.1 --mac-address 02:42:ac:1e:00:33 --cap-add NET_RAW --cap-add NET_ADMIN -v '$(CURDIR)':/work -w /work $(DOCKER_IMAGE) ./bin/raw_udp_sender; echo; echo '[src exited] Press Enter to close'; read _"; \
	else \
		echo "No supported terminal found. Please install gnome-terminal."; \
		exit 1; \
	fi

abc-run3: all docker-image docker-net4
	@docker rm -f raw3-src raw3-r1 raw3-r2 raw3-r3 raw3-dst >/dev/null 2>&1 || true
	@docker run -d --name raw3-src --hostname raw3-src --network $(DOCKER_NET_A) --ip 192.168.1.1 --mac-address 02:42:ac:1e:01:10 --cap-add NET_RAW --cap-add NET_ADMIN -v '$(CURDIR)':/work -w /work $(DOCKER_IMAGE) sleep infinity >/dev/null
	@docker run -d --name raw3-r1 --hostname raw3-r1 --network $(DOCKER_NET_A) --ip 192.168.1.2 --mac-address 02:42:ac:1e:01:11 --cap-add NET_RAW --cap-add NET_ADMIN -v '$(CURDIR)':/work -w /work $(DOCKER_IMAGE) sleep infinity >/dev/null
	@docker run -d --name raw3-r2 --hostname raw3-r2 --network $(DOCKER_NET_B) --ip 192.168.2.2 --mac-address 02:42:ac:1e:02:12 --cap-add NET_RAW --cap-add NET_ADMIN -v '$(CURDIR)':/work -w /work $(DOCKER_IMAGE) sleep infinity >/dev/null
	@docker run -d --name raw3-r3 --hostname raw3-r3 --network $(DOCKER_NET_C) --ip 192.168.3.2 --mac-address 02:42:ac:1e:03:13 --cap-add NET_RAW --cap-add NET_ADMIN -v '$(CURDIR)':/work -w /work $(DOCKER_IMAGE) sleep infinity >/dev/null
	@docker run -d --name raw3-dst --hostname raw3-dst --network $(DOCKER_NET_D) --ip 192.168.4.2 --mac-address 02:42:ac:1e:04:10 --cap-add NET_RAW --cap-add NET_ADMIN -v '$(CURDIR)':/work -w /work $(DOCKER_IMAGE) sleep infinity >/dev/null
	@docker network connect --ip 192.168.2.1 $(DOCKER_NET_B) raw3-r1
	@docker network connect --ip 192.168.3.1 $(DOCKER_NET_C) raw3-r2
	@docker network connect --ip 192.168.4.1 $(DOCKER_NET_D) raw3-r3
	@if command -v gnome-terminal >/dev/null 2>&1; then \
		gnome-terminal --title="abc-run3 SRC" -- bash -lc "R1_B=\$$(docker inspect -f '{{.NetworkSettings.Networks.$(DOCKER_NET_B).MacAddress}}' raw3-r1); docker exec -e NODE_NEXT_HOP_MAC=\"\$$R1_B\" -e NODE_MESSAGE=\"Hello, this is a test message.\" -it raw3-src ./bin/raw_udp_duplex_src; echo; echo '[raw3-src exited] Press Enter to close'; read _"; \
		gnome-terminal --title="abc-run3 R1" -- bash -lc "R2_B=\$$(docker inspect -f '{{.NetworkSettings.Networks.$(DOCKER_NET_B).MacAddress}}' raw3-r2); IF2=\$$(docker exec raw3-r1 sh -lc \"ls /sys/class/net | grep -v '^eth0\$$' | grep -v '^lo\$$' | head -n1\"); docker exec -e RTR_IFACE1=eth0 -e RTR_CIDR1=192.168.1.0/24 -e RTR_NEXTMAC1=02:42:ac:1e:01:10 -e RTR_IFACE2=\"\$$IF2\" -e RTR_CIDR2=192.168.4.0/24 -e RTR_NEXTMAC2=\"\$$R2_B\" -it raw3-r1 ./bin/raw_router_twoiface; echo; echo '[raw3-r1 exited] Press Enter to close'; read _"; \
		gnome-terminal --title="abc-run3 R2" -- bash -lc "R1_B=\$$(docker inspect -f '{{.NetworkSettings.Networks.$(DOCKER_NET_B).MacAddress}}' raw3-r1); R3_C=\$$(docker inspect -f '{{.NetworkSettings.Networks.$(DOCKER_NET_C).MacAddress}}' raw3-r3); IF2=\$$(docker exec raw3-r2 sh -lc \"ls /sys/class/net | grep -v '^eth0\$$' | grep -v '^lo\$$' | head -n1\"); docker exec -e RTR_IFACE1=eth0 -e RTR_CIDR1=192.168.1.0/24 -e RTR_NEXTMAC1=\"\$$R1_B\" -e RTR_IFACE2=\"\$$IF2\" -e RTR_CIDR2=192.168.4.0/24 -e RTR_NEXTMAC2=\"\$$R3_C\" -it raw3-r2 ./bin/raw_router_twoiface; echo; echo '[raw3-r2 exited] Press Enter to close'; read _"; \
		gnome-terminal --title="abc-run3 R3" -- bash -lc "R2_C=\$$(docker inspect -f '{{.NetworkSettings.Networks.$(DOCKER_NET_C).MacAddress}}' raw3-r2); DST_D=\$$(docker inspect -f '{{.NetworkSettings.Networks.$(DOCKER_NET_D).MacAddress}}' raw3-dst); IF2=\$$(docker exec raw3-r3 sh -lc \"ls /sys/class/net | grep -v '^eth0\$$' | grep -v '^lo\$$' | head -n1\"); docker exec -e RTR_IFACE1=eth0 -e RTR_CIDR1=192.168.1.0/24 -e RTR_NEXTMAC1=\"\$$R2_C\" -e RTR_IFACE2=\"\$$IF2\" -e RTR_CIDR2=192.168.4.0/24 -e RTR_NEXTMAC2=\"\$$DST_D\" -it raw3-r3 ./bin/raw_router_twoiface; echo; echo '[raw3-r3 exited] Press Enter to close'; read _"; \
		gnome-terminal --title="abc-run3 DST" -- bash -lc "R3_D=\$$(docker inspect -f '{{.NetworkSettings.Networks.$(DOCKER_NET_D).MacAddress}}' raw3-r3); docker exec -e NODE_NEXT_HOP_MAC=\"\$$R3_D\" -e NODE_MESSAGE=\"Hello! Got your message loud and clear.\" -it raw3-dst ./bin/raw_udp_duplex_dst; echo; echo '[raw3-dst exited] Press Enter to close'; read _"; \
	else \
		echo "No supported terminal found. Please install gnome-terminal."; \
		exit 1; \
	fi

abc-run4: all docker-image docker-net
	@docker rm -f raw4-src raw4-r1 raw4-r2 raw4-r3 raw4-dst >/dev/null 2>&1 || true
	@docker run -d --name raw4-src --hostname raw4-src --network $(DOCKER_NET) --ip 192.168.1.1 --mac-address 02:42:ac:1e:00:41 --cap-add NET_RAW --cap-add NET_ADMIN -v '$(CURDIR)':/work -w /work $(DOCKER_IMAGE) sleep infinity >/dev/null
	@docker run -d --name raw4-r1 --hostname raw4-r1 --network $(DOCKER_NET) --ip 192.168.1.2 --mac-address 02:42:ac:1e:00:42 --cap-add NET_RAW --cap-add NET_ADMIN -v '$(CURDIR)':/work -w /work $(DOCKER_IMAGE) sleep infinity >/dev/null
	@docker run -d --name raw4-r2 --hostname raw4-r2 --network $(DOCKER_NET) --ip 192.168.1.3 --mac-address 02:42:ac:1e:00:43 --cap-add NET_RAW --cap-add NET_ADMIN -v '$(CURDIR)':/work -w /work $(DOCKER_IMAGE) sleep infinity >/dev/null
	@docker run -d --name raw4-r3 --hostname raw4-r3 --network $(DOCKER_NET) --ip 192.168.1.4 --mac-address 02:42:ac:1e:00:44 --cap-add NET_RAW --cap-add NET_ADMIN -v '$(CURDIR)':/work -w /work $(DOCKER_IMAGE) sleep infinity >/dev/null
	@docker run -d --name raw4-dst --hostname raw4-dst --network $(DOCKER_NET) --ip 192.168.1.5 --mac-address 02:42:ac:1e:00:45 --cap-add NET_RAW --cap-add NET_ADMIN -v '$(CURDIR)':/work -w /work $(DOCKER_IMAGE) sleep infinity >/dev/null
	@if command -v gnome-terminal >/dev/null 2>&1; then \
		gnome-terminal --title="abc-run4 DST-RECV" -- bash -lc "docker exec -it raw4-dst ./bin/udp_receiver; echo; echo '[raw4-dst exited] Press Enter to close'; read _"; \
		gnome-terminal --title="abc-run4 R3-TABLE" -- bash -lc "docker exec -it raw4-r3 sh -lc 'printf \"192.168.1.5 02:42:ac:1e:00:45\\n\" > /tmp/route.tbl; ./bin/raw_router_table'; echo; echo '[raw4-r3 exited] Press Enter to close'; read _"; \
		gnome-terminal --title="abc-run4 R2-TABLE" -- bash -lc "docker exec -it raw4-r2 sh -lc 'printf \"192.168.1.5 02:42:ac:1e:00:44\\n\" > /tmp/route.tbl; ./bin/raw_router_table'; echo; echo '[raw4-r2 exited] Press Enter to close'; read _"; \
		gnome-terminal --title="abc-run4 R1-TABLE" -- bash -lc "docker exec -it raw4-r1 sh -lc 'printf \"192.168.1.5 02:42:ac:1e:00:43\\n\" > /tmp/route.tbl; ./bin/raw_router_table'; echo; echo '[raw4-r1 exited] Press Enter to close'; read _"; \
		gnome-terminal --title="abc-run4 SRC-SEND" -- bash -lc "sleep 1; docker exec -it raw4-src ./bin/raw_udp_sender; echo; echo '[raw4-src exited] Press Enter to close'; read _"; \
	else \
		echo "No supported terminal found. Please install gnome-terminal."; \
		exit 1; \
	fi
