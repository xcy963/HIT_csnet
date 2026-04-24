BUILD_DIR := build
TARGET := $(BUILD_DIR)/udp_server.exe
SRC := 2_server.cpp
OBJ := $(BUILD_DIR)/$(SRC:.cpp=.o)
CLIENT_TARGET := $(BUILD_DIR)/udp_client.exe
CLIENT_SRC := 2_client.cpp
CLIENT_OBJ := $(BUILD_DIR)/$(CLIENT_SRC:.cpp=.o)
SR_TARGET := $(BUILD_DIR)/udp_server_sr.exe
SR_SRC := 2_serverSR.cpp
SR_OBJ := $(BUILD_DIR)/$(SR_SRC:.cpp=.o)
SR_CLIENT_TARGET := $(BUILD_DIR)/udp_client_sr.exe
SR_CLIENT_SRC := 2_clientSR.cpp
SR_CLIENT_OBJ := $(BUILD_DIR)/$(SR_CLIENT_SRC:.cpp=.o)

CXX := g++
CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra -Wpedantic
LDFLAGS := -lws2_32

.PHONY: all clean run run-client run-sr run-client-sr gbn SR

all: $(TARGET) $(CLIENT_TARGET) $(SR_TARGET) $(SR_CLIENT_TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

$(CLIENT_TARGET): $(CLIENT_OBJ)
	$(CXX) $(CLIENT_OBJ) -o $@ $(LDFLAGS)

$(SR_TARGET): $(SR_OBJ)
	$(CXX) $(SR_OBJ) -o $@ $(LDFLAGS)

$(SR_CLIENT_TARGET): $(SR_CLIENT_OBJ)
	$(CXX) $(SR_CLIENT_OBJ) -o $@ $(LDFLAGS)

$(BUILD_DIR):
	if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"

$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	.\$(TARGET)

run-client: $(CLIENT_TARGET)
	.\$(CLIENT_TARGET)

run-sr: $(SR_TARGET)
	.\$(SR_TARGET)

run-client-sr: $(SR_CLIENT_TARGET)
	.\$(SR_CLIENT_TARGET)

gbn: all
	cmd /c start "UDP Server" cmd /k ".\build\udp_server.exe"
	cmd /c start "UDP Client" cmd /k ".\build\udp_client.exe"

SR: $(SR_TARGET) $(SR_CLIENT_TARGET)
	cmd /c start "SR Server" cmd /k ".\build\udp_server_sr.exe"
	cmd /c start "SR Client" cmd /k ".\build\udp_client_sr.exe"

clean:
	if exist "$(BUILD_DIR)" rmdir /S /Q "$(BUILD_DIR)"
