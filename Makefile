PROJ_ROOT := $(shell pwd)
BUILD_DIR := $(PROJ_ROOT)/build
OUT_DIR   := $(BUILD_DIR)/out
OUT_KEXT  := $(OUT_DIR)/Ath10kMac.kext
OUT_BIN   := $(OUT_KEXT)/Contents/MacOS/Ath10kMac

SDK   = $(shell xcrun --show-sdk-path)
PYTHON ?= python3
MKSDK := $(PROJ_ROOT)/MacKernelSDK
ARCH  := -arch x86_64
MINOS := -mmacosx-version-min=11.0
CC    := xcrun clang
CXX   := xcrun clang++
LD    := xcrun clang++

HT_VHT ?= 1
MAX_WIDTH ?= 80
MAX_NSS ?= 2
TX_SLOTS ?= 64
AMPDU_LIMIT ?= 64
AMSDU_LIMIT ?= 3
OPT ?= -O2
PAYLOAD_CACHE ?= 1
RUNTIME_IRQ ?= 1
RX_BATCH ?= 1
RX_RESERVE ?= 1
KFLAGS = -fstack-usage -Wframe-larger-than=4096 -Werror=frame-larger-than $(OPT) -DATH10K_PAYLOAD_CACHE=$(PAYLOAD_CACHE) -DATH10K_RUNTIME_IRQ=$(RUNTIME_IRQ) -DATH10K_RX_BATCH=$(RX_BATCH) -DATH10K_RX_RESERVE=$(RX_RESERVE) -DATH10K_MAX_WIDTH=$(MAX_WIDTH) -DATH10K_MAX_NSS=$(MAX_NSS) -DATH10K_AMPDU_LIMIT=$(AMPDU_LIMIT) -DATH10K_AMSDU_LIMIT=$(AMSDU_LIMIT) -DATH10K_HT_VHT=$(HT_VHT) -DATH10K_DATA_TX_SLOTS=$(TX_SLOTS) -fno-stack-check -fno-stack-protector -mkernel $(ARCH) $(MINOS) \
          -isysroot $(SDK) \
          -I$(MKSDK)/Headers \
          -I$(SDK)/System/Library/Frameworks/Kernel.framework/Headers

CXXFLAGS = $(KFLAGS) -fapple-kext -fno-exceptions -fno-rtti -std=c++17 \
            -DKERNEL -Wno-deprecated-declarations -Wno-nullability-completeness

LDFLAGS = $(ARCH) -static -nostdlib -Xlinker -kext \
           $(MKSDK)/Library/x86_64/libkmod.a \
           $(MKSDK)/Library/universal/libkmodc++.a \
           -F$(SDK)/System/Library/Frameworks

OBJS := $(BUILD_DIR)/Ath10kUserClient.o $(BUILD_DIR)/Ath10kNetwork.o $(BUILD_DIR)/Ath10kRuntime.o $(BUILD_DIR)/Ath10kPower.o $(BUILD_DIR)/Ath10kPCIDevice.o $(BUILD_DIR)/kmod_info.o

.PHONY: all kext clean check-sdk prepare-firmware
all: kext
kext: check-sdk $(OUT_BIN)
	$(PYTHON) scripts/check-kext-symbols.py "$(OUT_BIN)"
	@echo "Built and symbol-checked: $(OUT_KEXT)"

# Always validate source containers, even when a generated header exists.
# The header dependency serializes preparation before compiling with make -j.
prepare-firmware:
	$(PYTHON) scripts/prepare-firmware.py

src/kext/Ath10kPayload.hpp: prepare-firmware
	@test -f "$@"

check-sdk:
	@test -f "$(MKSDK)/Library/x86_64/libkmod.a" || \
	  (echo "ERROR: MacKernelSDK missing. Run: git submodule update --init --recursive"; exit 1)

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/Ath10kPCIDevice.o: src/kext/Ath10kPCIDevice.cpp src/kext/Ath10kPCIDevice.hpp src/kext/Ath10kHw.hpp src/kext/Ath10kRegisterIO.hpp src/kext/Ath10kTarget.hpp src/kext/Ath10kCE.hpp src/kext/Ath10kCERing.hpp src/kext/Ath10kDMA.hpp src/kext/Ath10kBMI.hpp src/kext/Ath10kPLL.hpp src/kext/Ath10kBoot.hpp src/kext/Ath10kHTC.hpp src/kext/Ath10kWMI.hpp src/kext/Ath10kRxSetup.hpp src/kext/Ath10kScanPlan.hpp src/kext/Ath10kRegulatory.hpp src/kext/Ath10kRegulatoryData.hpp src/kext/Ath10kBSS.hpp src/kext/Ath10kConnection.hpp src/kext/Ath10kRX.hpp src/kext/Ath10kPerformance.hpp src/kext/Ath10kRXRate.hpp src/kext/Ath10kPayload.hpp src/kext/Ath10kPayloadProfile.hpp src/kext/Ath10kInPlace.hpp src/kext/Ath10kPower.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/Ath10kNetwork.o: src/kext/Ath10kNetwork.cpp $(wildcard src/kext/*.hpp src/shared/*.hpp) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/Ath10kRuntime.o: src/kext/Ath10kRuntime.cpp $(wildcard src/kext/*.hpp src/shared/*.hpp) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/Ath10kPower.o: src/kext/Ath10kPower.cpp $(wildcard src/kext/*.hpp src/shared/*.hpp) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/Ath10kUserClient.o: src/kext/Ath10kUserClient.cpp $(wildcard src/kext/*.hpp src/shared/*.hpp) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/kmod_info.o: src/kext/kmod_info.c | $(BUILD_DIR)
	$(CC) $(KFLAGS) -c $< -o $@

# Rebuild all objects when experiment flags change, even without make clean.
.PHONY: FORCE
FORCE:
$(BUILD_DIR)/experiment-config: FORCE | $(BUILD_DIR)
	@printf '%s\n' 'OPT=$(OPT) PAYLOAD_CACHE=$(PAYLOAD_CACHE) RUNTIME_IRQ=$(RUNTIME_IRQ) RX_BATCH=$(RX_BATCH) RX_RESERVE=$(RX_RESERVE) HT_VHT=$(HT_VHT) MAX_WIDTH=$(MAX_WIDTH) MAX_NSS=$(MAX_NSS) TX_SLOTS=$(TX_SLOTS) AMPDU_LIMIT=$(AMPDU_LIMIT) AMSDU_LIMIT=$(AMSDU_LIMIT)' > "$@.tmp"
	@cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"
	@rm -f "$@.tmp"

$(OBJS): Makefile $(BUILD_DIR)/experiment-config
$(OBJS): src/kext/Ath10kMulticast.hpp src/kext/Ath10kConnection.hpp src/shared/Ath10kControl.hpp
$(OBJS): src/kext/Ath10kRuntimePower.hpp src/kext/Ath10kFirmwareStats.hpp

$(OUT_BIN): Makefile $(OBJS) Ath10kMac.kext/Contents/Info.plist
	@mkdir -p "$(OUT_KEXT)/Contents/MacOS"
	@cp Ath10kMac.kext/Contents/Info.plist "$(OUT_KEXT)/Contents/Info.plist"
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: app
app:
	sh ./scripts/build-connectivity.sh
