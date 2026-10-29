# SPDX-License-Identifier: ISC
# Host-side build for QCA6174 common parsers + tests.
#
# This Makefile builds and runs the HOST-TESTABLE portion ONLY (src/common +
# tests). It deliberately does NOT build the kext: that requires Xcode and the
# macOS kernel SDK, which do not exist on Linux. See docs/BUILD.md.

# Use clang by default (matches the intended macOS toolchain). Override on the
# command line with `make CC=gcc`.
CC       := clang
CXX      := clang++
CSTD     ?= -std=c11
CXXSTD   ?= -std=c++17
# Shared warnings. -Wstrict-prototypes is C-only, kept out of CXXWARN.
WARN     := -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion \
            -Wpointer-arith
CWARN    := $(WARN) -Wstrict-prototypes
OPT      ?= -O2 -g
# Sanitizers catch overflow/OOB in the parsers; override with SAN= to disable.
SAN      ?= -fsanitize=address,undefined -fno-sanitize-recover=all
INC      := -Isrc/common
CFLAGS   := $(CSTD) $(CWARN) $(OPT) $(SAN) $(INC)
CXXFLAGS := $(CXXSTD) $(WARN) $(OPT) $(SAN) $(INC)

BUILD    := build
OBJ      := $(BUILD)/obj
FIXTURES := $(BUILD)/fixtures

COMMON_SRC := \
    src/common/fw_file.c \
    src/common/board_file.c \
    src/common/qca6174_regs.c \
    src/common/hw_params.c \
    src/common/fw_load_sm.c
COMMON_OBJ := $(patsubst src/common/%.c,$(OBJ)/%.o,$(COMMON_SRC))

UNIT_SRC := \
    tests/run_unit.c \
    tests/test_fw_file.c \
    tests/test_board_file.c \
    tests/test_regs.c \
    tests/test_hw_params.c \
    tests/test_fw_load_sm.c

.PHONY: all test unit fixtures fixture-probe cxx-smoke clean

all: test

$(BUILD) $(OBJ):
	mkdir -p $@

# Common C sources -> objects (compiled as C, reused by C and C++ links).
$(OBJ)/%.o: src/common/%.c | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- unit tests (no external fixtures) ----
$(BUILD)/run_unit: $(UNIT_SRC) $(COMMON_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) $(UNIT_SRC) $(COMMON_OBJ) -o $@

unit: $(BUILD)/run_unit
	$(BUILD)/run_unit

# ---- fixtures (decompress system firmware) ----
fixtures:
	./scripts/prepare_fixtures.sh $(FIXTURES)

# ---- fixture probe (parse real firmware) ----
$(BUILD)/fixture_probe: tests/fixture_probe.c $(COMMON_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) tests/fixture_probe.c $(COMMON_OBJ) -o $@

fixture-probe: $(BUILD)/fixture_probe fixtures
	$(BUILD)/fixture_probe $(FIXTURES)

# ---- C++ glue boundary smoke (NOT the kext; see docs/BUILD.md) ----
$(BUILD)/cxx_glue_smoke: tests/cxx_glue_smoke.cpp $(COMMON_OBJ) | $(BUILD)
	$(CXX) $(CXXFLAGS) tests/cxx_glue_smoke.cpp $(COMMON_OBJ) -o $@

cxx-smoke: $(BUILD)/cxx_glue_smoke
	$(BUILD)/cxx_glue_smoke

# ---- everything ----
test: unit fixture-probe cxx-smoke

clean:
	rm -rf $(BUILD)
