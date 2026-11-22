# ==============================================================================
# Makefile for ps5-fanContrl
# ==============================================================================

# SDK 路径（如果环境变量未设置，默认使用 /opt/ps5-payload-sdk）
PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk

# 编译器与构建参数
CC      := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
CFLAGS  := -Wall -Wextra -Os
LDFLAGS := -lkernel_sys -lkernel -lc

# 目标文件与源文件
TARGET  := ps5-fan-control.elf
SRCS    := main.c

# 默认构建目标
.PHONY: all clean send

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# 清理编译产物
clean:
	rm -f $(TARGET) *.o

# 可选便捷命令：通过网络直接推送至 PS5（用法: make send PS5_HOST=192.168.1.xxx）
PS5_HOST ?=
PS5_PORT ?= 9021

send: $(TARGET)
ifndef PS5_HOST
	$(error 请指定 PS5 的 IP 地址，例如: make send PS5_HOST=192.168.1.100)
endif
	nc -q 0 $(PS5_HOST) $(PS5_PORT) < $(TARGET)
