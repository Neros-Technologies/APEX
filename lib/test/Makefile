# APEX library unit tests — googletest harness.
#
# By default, points at a GoogleTest source tree at ./gtest (the fused/full
# source, with inc/ and src/gtest-all.cc). Override GTEST_DIR to point at your
# own copy, e.g. `make GTEST_DIR=/path/to/googletest`.

LIB_DIR    := ..
GTEST_DIR  ?= gtest
BUILD_DIR  := build

CXX        ?= c++
CXXFLAGS   ?= -O0 -g
CXXFLAGS   += -std=c++17 -Wall -Wextra -Wno-missing-field-initializers
CXXFLAGS   += -I$(LIB_DIR)/include -isystem $(GTEST_DIR)/inc -I$(GTEST_DIR)

CC         ?= cc
CFLAGS     ?= -O0 -g
CFLAGS     += -std=c99 -Wall -Wextra -Wpedantic -Wno-missing-field-initializers
CFLAGS     += -I$(LIB_DIR)/include

LIB_SRCS := \
    $(LIB_DIR)/src/apex_cobs.c \
    $(LIB_DIR)/src/apex_crc.c \
    $(LIB_DIR)/src/apex_framer.c \
    $(LIB_DIR)/src/apex_host.c \
    $(LIB_DIR)/src/apex_device.c \
    $(LIB_DIR)/src/apex_activation.c \
    $(LIB_DIR)/src/apex_analog_hmi.c \
    $(LIB_DIR)/src/apex_repeater.c

LIB_OBJS := $(patsubst $(LIB_DIR)/src/%.c,$(BUILD_DIR)/lib/%.o,$(LIB_SRCS))

GTEST_OBJS := $(BUILD_DIR)/gtest/gtest-all.o $(BUILD_DIR)/gtest/gtest_main.o

TESTS := \
    test_cobs \
    test_crc \
    test_framer \
    test_discovery \
    test_activation_walkthrough \
    test_activation_gpio \
    test_analog_hmi \
    test_repeater

TEST_BINS := $(addprefix $(BUILD_DIR)/,$(TESTS))

.PHONY: all check clean run-%

all: $(TEST_BINS)

check: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do \
		echo "==> $$t"; \
		$$t; \
	done

run-%: $(BUILD_DIR)/%
	./$<

$(BUILD_DIR)/test_%: $(BUILD_DIR)/test_%.o $(LIB_OBJS) $(GTEST_OBJS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ -lpthread

$(BUILD_DIR)/%.o: %.cc | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/lib/%.o: $(LIB_DIR)/src/%.c | $(BUILD_DIR)/lib
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/gtest/gtest-all.o: $(GTEST_DIR)/src/gtest-all.cc | $(BUILD_DIR)/gtest
	$(CXX) $(CXXFLAGS) -Wno-unused-const-variable -c $< -o $@

$(BUILD_DIR)/gtest/gtest_main.o: $(GTEST_DIR)/src/gtest_main.cc | $(BUILD_DIR)/gtest
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR) $(BUILD_DIR)/lib $(BUILD_DIR)/gtest:
	@mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
