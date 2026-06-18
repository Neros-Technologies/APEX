# APEX reference library — tiny portable build.
#
# Produces build/libapex.a from the sources under src/. No third-party deps,
# no submodules. Override CC, AR, CFLAGS to cross-compile.

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -Wall -Wextra -Wpedantic -Wno-missing-field-initializers
CFLAGS  += -Iinclude

BUILD_DIR := build
SRC_DIR   := src

SRCS := \
    $(SRC_DIR)/apex_cobs.c \
    $(SRC_DIR)/apex_crc.c \
    $(SRC_DIR)/apex_framer.c \
    $(SRC_DIR)/apex_host.c \
    $(SRC_DIR)/apex_device.c \
    $(SRC_DIR)/apex_activation.c \
    $(SRC_DIR)/apex_analog_hmi.c \
    $(SRC_DIR)/apex_repeater.c

OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
LIB  := $(BUILD_DIR)/libapex.a

.PHONY: all clean test

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

test: $(LIB)
	$(MAKE) -C test

clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C test clean
