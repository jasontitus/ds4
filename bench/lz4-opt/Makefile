# LZ4 optimization benchmarking framework.
#
# Build per-variant under build/<VARIANT>/. Each variant is a separate compile
# of the LZ4 library + bench/verify binaries with a chosen set of LZ4_OPT_*
# flags enabled via OPT_CFLAGS.
#
# Usage:
#   make                                              # baseline (no opts)
#   make VARIANT=hc_pref OPT_CFLAGS="-DLZ4_OPT_HC_PREFETCH=1"
#   make clean                                        # wipe all variants
#   make info VARIANT=hc_pref                         # show what would build

CC ?= cc
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

LZ4_DIR := lz4/lib
LZ4_SRCS := $(LZ4_DIR)/lz4.c $(LZ4_DIR)/lz4hc.c

# Per-variant identification.
VARIANT ?= baseline
BUILD_DIR := build/$(VARIANT)

# Optimization flags — set via the command line.
OPT_CFLAGS ?=

# Architecture-aware native tuning.
ifeq ($(UNAME_S),Darwin)
NATIVE_FLAG ?= -mcpu=native
else
NATIVE_FLAG ?= -march=native
endif

# Base flags. -O3, native tune, no asserts in benchmarking. -fno-builtin-memcpy
# was considered but rejected: LZ4 relies on inline-able memcpy for the fast
# decode loop, so leave it on.
# _POSIX_C_SOURCE=200809L is required so that macOS / Darwin headers expose
# both clock_gettime (POSIX.1-2001) and snprintf (which Darwin gates behind
# this level). 199309L is enough on Linux but hides snprintf in Darwin's
# stdio.h under strict POSIX mode.
BASE_CFLAGS ?= -O3 -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter $(NATIVE_FLAG)
CFLAGS_ALL  := $(BASE_CFLAGS) $(OPT_CFLAGS) $(CFLAGS)
CPPFLAGS_ALL := -I. -I$(LZ4_DIR) $(CPPFLAGS)
LDLIBS_ALL  := -lm $(LDLIBS)

OBJ_LZ4_C   := $(BUILD_DIR)/lz4.o
OBJ_LZ4_HC  := $(BUILD_DIR)/lz4hc.o
OBJ_CORPUS  := $(BUILD_DIR)/corpus.o
OBJ_BENCH   := $(BUILD_DIR)/bench.o
OBJ_VERIFY  := $(BUILD_DIR)/verify.o
ALL_OBJS    := $(OBJ_LZ4_C) $(OBJ_LZ4_HC) $(OBJ_CORPUS) $(OBJ_BENCH) $(OBJ_VERIFY)

.PHONY: all clean clean-variant info

all: $(BUILD_DIR)/bench $(BUILD_DIR)/verify

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/bench: $(OBJ_BENCH) $(OBJ_CORPUS) $(OBJ_LZ4_C) $(OBJ_LZ4_HC)
	$(CC) $(CFLAGS_ALL) -o $@ $^ $(LDLIBS_ALL)

$(BUILD_DIR)/verify: $(OBJ_VERIFY) $(OBJ_CORPUS) $(OBJ_LZ4_C) $(OBJ_LZ4_HC)
	$(CC) $(CFLAGS_ALL) -o $@ $^ $(LDLIBS_ALL)

$(OBJ_BENCH): bench.c lz4_opts.h corpus.h timing.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_ALL) $(CPPFLAGS_ALL) -c -o $@ bench.c

$(OBJ_VERIFY): verify.c lz4_opts.h corpus.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_ALL) $(CPPFLAGS_ALL) -c -o $@ verify.c

$(OBJ_CORPUS): corpus.c corpus.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_ALL) $(CPPFLAGS_ALL) -c -o $@ corpus.c

$(OBJ_LZ4_C): $(LZ4_DIR)/lz4.c lz4_opts.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_ALL) $(CPPFLAGS_ALL) -c -o $@ $(LZ4_DIR)/lz4.c

$(OBJ_LZ4_HC): $(LZ4_DIR)/lz4hc.c lz4_opts.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_ALL) $(CPPFLAGS_ALL) -c -o $@ $(LZ4_DIR)/lz4hc.c

clean:
	rm -rf build

clean-variant:
	rm -rf $(BUILD_DIR)

info:
	@echo "VARIANT     = $(VARIANT)"
	@echo "BUILD_DIR   = $(BUILD_DIR)"
	@echo "CC          = $(CC)"
	@echo "UNAME_S     = $(UNAME_S)  UNAME_M = $(UNAME_M)"
	@echo "CFLAGS_ALL  = $(CFLAGS_ALL)"
	@echo "OPT_CFLAGS  = $(OPT_CFLAGS)"
	@echo "CPPFLAGS    = $(CPPFLAGS_ALL)"
	@echo "LDLIBS      = $(LDLIBS_ALL)"
