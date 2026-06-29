# u60pro-devui - cross-compile to a single static aarch64 binary.
#
# Build (in a POSIX shell: WSL, Git-Bash, or Linux) with an aarch64 musl
# toolchain on PATH, e.g. from https://musl.cc (aarch64-linux-musl-cross):
#
#   make CROSS_COMPILE=aarch64-linux-musl-
#
# The result is a self-contained `u60pro-devui` binary with no runtime deps.
#
# SPDX-License-Identifier: MIT

CROSS_COMPILE ?= aarch64-linux-musl-
CC := $(CROSS_COMPILE)gcc

TARGET   := u60pro-devui
ROOT     := .
LVGL_DIR := third_party/lvgl

APP_SRCS  := $(wildcard src/*.c)
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name '*.c' 2>/dev/null)
OBJS      := $(APP_SRCS:.c=.o) $(LVGL_SRCS:.c=.o)

CFLAGS := -std=c11 -Os -ffunction-sections -fdata-sections \
          -Wall -Wextra -Wno-unused-parameter \
          -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE \
          -I$(ROOT) -Iinclude -I$(LVGL_DIR) -Ithird_party/stb

LDFLAGS := -static -Wl,--gc-sections -lm

.PHONY: all clean check-lvgl

all: check-lvgl $(TARGET)

check-lvgl:
	@test -f $(LVGL_DIR)/lvgl.h || { \
	  echo "ERROR: LVGL not found at $(LVGL_DIR)."; \
	  echo "Run: git submodule add -b release/v9.2 https://github.com/lvgl/lvgl.git $(LVGL_DIR)"; \
	  echo "  (or clone it there manually)"; exit 1; }

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "built $(TARGET):"
	@$(CROSS_COMPILE)size $(TARGET) 2>/dev/null || true

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
