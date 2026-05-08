CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -O2 -g -Iinclude
LDFLAGS :=
BIN_DIR := bin
COMMON := src/netlab.c

PROGRAMS := udp_sender udp_forwarder udp_receiver raw_udp_sender raw_router_oneiface raw_router_twoiface
TARGETS := $(addprefix $(BIN_DIR)/,$(PROGRAMS))
DOCKER_IMAGE := ubuntu:22.04
DOCKER_NET := hitcsnet_lan
DOCKER_SUBNET := 172.30.0.0/24

.PHONY: all clean run-help docker-net vm1 vm2 vm3 vm4 vm5 vm-clean

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

$(BIN_DIR)/raw_router_oneiface: src/raw_router_oneiface.c $(COMMON) include/netlab.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ src/raw_router_oneiface.c $(COMMON) $(LDFLAGS)

$(BIN_DIR)/raw_router_twoiface: src/raw_router_twoiface.c $(COMMON) include/netlab.h | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ src/raw_router_twoiface.c $(COMMON) $(LDFLAGS)

run-help:
	@echo "Build: make"
	@echo "Task 5.1 receiver:  ./bin/udp_receiver 54321"
	@echo "Task 5.1 forwarder:  ./bin/udp_forwarder 12345 192.168.79.131 54321"
	@echo "Task 5.1 sender:     ./bin/udp_sender 192.168.79.130 12345 'hello'"
	@echo "Task 5.2 raw sender: sudo ./bin/raw_udp_sender ens33 <router_mac> 192.168.1.2 192.168.1.3 12345 12345 'hello'"
	@echo "Task 5.2 router:     sudo ./bin/raw_router_oneiface ens33 192.168.1.2 192.168.1.3 <dest_host_mac>"
	@echo "Task 5.3 router:     sudo ./bin/raw_router_twoiface eth0 192.168.1.0/24 <host1_mac> eth1 192.168.2.0/24 <host2_mac>"
	@echo "Docker LAN net:      make docker-net"
	@echo "Docker vm1..vm5:     make vm1   (or vm2/vm3/vm4/vm5 in separate terminals)"
	@echo "Docker cleanup:      make vm-clean"

clean:
	rm -rf $(BIN_DIR)

docker-net:
	@docker network inspect $(DOCKER_NET) >/dev/null 2>&1 || \
		docker network create --driver bridge --subnet $(DOCKER_SUBNET) $(DOCKER_NET)

vm1: docker-net
	docker rm -f vm1 >/dev/null 2>&1 || true
	docker run --rm -it --name vm1 --hostname vm1 --network $(DOCKER_NET) --ip 172.30.0.11 $(DOCKER_IMAGE) bash

vm2: docker-net
	docker rm -f vm2 >/dev/null 2>&1 || true
	docker run --rm -it --name vm2 --hostname vm2 --network $(DOCKER_NET) --ip 172.30.0.12 $(DOCKER_IMAGE) bash

vm3: docker-net
	docker rm -f vm3 >/dev/null 2>&1 || true
	docker run --rm -it --name vm3 --hostname vm3 --network $(DOCKER_NET) --ip 172.30.0.13 $(DOCKER_IMAGE) bash

vm4: docker-net
	docker rm -f vm4 >/dev/null 2>&1 || true
	docker run --rm -it --name vm4 --hostname vm4 --network $(DOCKER_NET) --ip 172.30.0.14 $(DOCKER_IMAGE) bash

vm5: docker-net
	docker rm -f vm5 >/dev/null 2>&1 || true
	docker run --rm -it --name vm5 --hostname vm5 --network $(DOCKER_NET) --ip 172.30.0.15 $(DOCKER_IMAGE) bash

vm-clean:
	@docker rm -f vm1 vm2 vm3 vm4 vm5 >/dev/null 2>&1 || true
	@docker network rm $(DOCKER_NET) >/dev/null 2>&1 || true
