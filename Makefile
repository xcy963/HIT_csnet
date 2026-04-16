BUILD_DIR := build
TARGET := $(BUILD_DIR)/proxy_server.exe
SRC := 1.cpp
OBJ := $(BUILD_DIR)/$(SRC:.cpp=.o)

CXX := g++
CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra -Wpedantic
LDFLAGS := -lws2_32

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

$(BUILD_DIR):
	mkdir  $(BUILD_DIR)

$(BUILD_DIR)/%.o: %.cpp stdafx.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	.\$(TARGET)

clean:
	rmdir /S /Q $(BUILD_DIR) 2>nul || exit 0
