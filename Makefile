.DEFAULT_GOAL := all
PROJ_ROOT := $(shell pwd)
BUILD_DIR := $(PROJ_ROOT)/build
NATIVE ?= 1
VENTURA ?= 0
DIAGNOSTIC_STAGE ?= 0
P256_DIAGNOSTIC ?= 0
ifeq ($(P256_DIAGNOSTIC),1)
ifneq ($(NATIVE),1)
$(error P256_DIAGNOSTIC requires NATIVE=1)
endif
ifneq ($(DIAGNOSTIC_STAGE),4)
$(error P256_DIAGNOSTIC requires DIAGNOSTIC_STAGE=4)
endif
else
ifneq ($(P256_DIAGNOSTIC),0)
$(error P256_DIAGNOSTIC must be 0 or 1)
endif
ifeq ($(DIAGNOSTIC_STAGE),4)
$(error DIAGNOSTIC_STAGE=4 requires P256_DIAGNOSTIC=1)
endif
endif
ifeq ($(NATIVE),1)
BUILD_DIR := $(PROJ_ROOT)/build/native
endif
ifeq ($(VENTURA),1)
BUILD_DIR := $(PROJ_ROOT)/build/ventura
endif
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
DIAGNOSTICS ?= 0
KFLAGS = -MMD -MP -DATH10K_P256_DIAGNOSTIC=$(P256_DIAGNOSTIC) -DATH10K_DIAGNOSTICS=$(DIAGNOSTICS) -fstack-usage -Wframe-larger-than=4096 -Werror=frame-larger-than $(OPT) -DATH10K_PAYLOAD_CACHE=$(PAYLOAD_CACHE) -DATH10K_RUNTIME_IRQ=$(RUNTIME_IRQ) -DATH10K_RX_BATCH=$(RX_BATCH) -DATH10K_RX_RESERVE=$(RX_RESERVE) -DATH10K_MAX_WIDTH=$(MAX_WIDTH) -DATH10K_MAX_NSS=$(MAX_NSS) -DATH10K_AMPDU_LIMIT=$(AMPDU_LIMIT) -DATH10K_AMSDU_LIMIT=$(AMSDU_LIMIT) -DATH10K_HT_VHT=$(HT_VHT) -DATH10K_DATA_TX_SLOTS=$(TX_SLOTS) -fno-stack-check -fno-stack-protector -mkernel $(ARCH) $(MINOS) \
          -isysroot $(SDK) \
          -I$(MKSDK)/Headers \
          -I$(SDK)/System/Library/Frameworks/Kernel.framework/Headers

CXXFLAGS = $(KFLAGS) -fapple-kext -fno-exceptions -fno-rtti -std=c++17 \
            -DKERNEL -Wno-deprecated-declarations -Wno-nullability-completeness

LDFLAGS = $(ARCH) -static -nostdlib -Xlinker -kext \
           $(MKSDK)/Library/x86_64/libkmod.a \
           $(MKSDK)/Library/universal/libkmodc++.a \
           -F$(SDK)/System/Library/Frameworks

ifeq ($(VENTURA),1)
CXXFLAGS += -DATH10K_VENTURA=1 -UATH10K_NATIVE -I$(PROJ_ROOT)/vendor/itlwm-v1 -I$(PROJ_ROOT)/vendor/itlwm-v1/Airport \
            -DATH10K_P256_DIAGNOSTIC_BINDING=1
MINOS := -mmacosx-version-min=13.0
# SAE/PMF sessions are shared with the native personality: they only need
# corecrypto (cchmac/ccsha256) plus the P-256 binding, both of which resolve in
# the Ventura personality too, so the same objects are linked here.
VENTURA_OBJECT := $(BUILD_DIR)/Ath10kVentura.o $(BUILD_DIR)/Ath10kVenturaInterface.o $(BUILD_DIR)/Ath10kVenturaUserClient.o \
                  $(BUILD_DIR)/Ath10kSAEWorker.o $(BUILD_DIR)/Ath10kSAESession.o $(BUILD_DIR)/Ath10kPMFSession.o \
                  $(BUILD_DIR)/Ath10kInfraInterface.o
endif

ifeq ($(NATIVE),1)
ifeq ($(VENTURA),1)
else
CXXFLAGS += -DATH10K_NATIVE=1 -I$(PROJ_ROOT)/vendor/itlwm
NATIVE_OBJECT := $(BUILD_DIR)/Ath10kNative.o $(BUILD_DIR)/Ath10kSAEWorker.o $(BUILD_DIR)/Ath10kSAESession.o $(BUILD_DIR)/Ath10kNativeSAEPreparation.o $(BUILD_DIR)/Ath10kP256CheckJob.o $(BUILD_DIR)/Ath10kPMFSession.o
endif
endif

ifeq ($(P256_DIAGNOSTIC),1)
# Standalone registry diagnostic: no controller, networking, firmware or DMA code.
OBJS := $(BUILD_DIR)/Ath10kSAEWorker.o $(BUILD_DIR)/Ath10kP256CheckJob.o $(BUILD_DIR)/Ath10kP256RegistryJob.o $(BUILD_DIR)/Ath10kP256DiagnosticService.o $(BUILD_DIR)/kmod_info.o
else
OBJS := $(NATIVE_OBJECT) $(VENTURA_OBJECT) $(BUILD_DIR)/Ath10kUserClient.o $(BUILD_DIR)/Ath10kNetwork.o $(BUILD_DIR)/Ath10kRuntime.o $(BUILD_DIR)/Ath10kPower.o $(BUILD_DIR)/Ath10kRecovery.o $(BUILD_DIR)/Ath10kPCIDevice.o $(BUILD_DIR)/kmod_info.o
endif
$(OBJS): $(wildcard src/shared/*.hpp)
ifeq ($(NATIVE),1)
$(OBJS): $(wildcard src/kext/native/*)
endif
ifeq ($(VENTURA),1)
$(OBJS): $(wildcard src/kext/native/Ath10kSAE*.hpp) $(wildcard src/kext/native/Ath10kPMFSession.hpp)
endif

$(BUILD_DIR)/Ath10kVentura.o: src/kext/ventura/Ath10kVentura.cpp $(wildcard src/kext/ventura/*) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/Ath10kVenturaInterface.o: src/kext/ventura/Ath10kVenturaInterface.cpp $(wildcard src/kext/ventura/*) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/Ath10kInfraInterface.o: src/kext/ventura/Ath10kInfraInterface.cpp $(wildcard src/kext/ventura/*) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/Ath10kVenturaUserClient.o: src/kext/ventura/Ath10kVenturaUserClient.cpp $(wildcard src/kext/ventura/*) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/Ath10kNative.o: src/kext/native/Ath10kNative.cpp $(wildcard src/kext/native/*) $(wildcard src/kext/*.hpp) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/Ath10kSAEWorker.o: src/kext/native/Ath10kSAEWorker.cpp src/kext/native/Ath10kSAEWorker.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/Ath10kSAESession.o: src/kext/native/Ath10kSAESession.cpp src/kext/native/Ath10kSAESession.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/Ath10kNativeSAEPreparation.o: src/kext/native/Ath10kNativeSAEPreparation.cpp src/kext/native/Ath10kNativeSAEPreparation.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/Ath10kP256CheckJob.o: src/kext/native/Ath10kP256CheckJob.cpp src/kext/native/Ath10kP256CheckJob.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/Ath10kP256RegistryJob.o: src/kext/native/Ath10kP256RegistryJob.cpp src/kext/native/Ath10kP256RegistryJob.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/Ath10kP256DiagnosticService.o: src/kext/native/Ath10kP256DiagnosticService.cpp src/kext/native/Ath10kP256RegistryJob.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/Ath10kPMFSession.o: src/kext/native/Ath10kPMFSession.cpp src/kext/native/Ath10kPMFSession.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

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

$(BUILD_DIR)/Ath10kRecovery.o: src/kext/Ath10kRecovery.cpp $(wildcard src/kext/*.hpp src/shared/*.hpp) | $(BUILD_DIR)
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
	@printf '%s\n' 'P256_DIAGNOSTIC=$(P256_DIAGNOSTIC) DIAGNOSTIC_STAGE=$(DIAGNOSTIC_STAGE) DIAGNOSTICS=$(DIAGNOSTICS) OPT=$(OPT) PAYLOAD_CACHE=$(PAYLOAD_CACHE) RUNTIME_IRQ=$(RUNTIME_IRQ) RX_BATCH=$(RX_BATCH) RX_RESERVE=$(RX_RESERVE) HT_VHT=$(HT_VHT) MAX_WIDTH=$(MAX_WIDTH) MAX_NSS=$(MAX_NSS) TX_SLOTS=$(TX_SLOTS) AMPDU_LIMIT=$(AMPDU_LIMIT) AMSDU_LIMIT=$(AMSDU_LIMIT)' > "$@.tmp"
	@cmp -s "$@.tmp" "$@" || cp "$@.tmp" "$@"
	@rm -f "$@.tmp"

$(OBJS): Makefile $(BUILD_DIR)/experiment-config
$(OBJS): src/kext/Ath10kMulticast.hpp src/kext/Ath10kConnection.hpp src/shared/Ath10kControl.hpp
$(OBJS): src/kext/Ath10kRuntimePower.hpp src/kext/Ath10kFirmwareStats.hpp
$(OBJS): $(wildcard src/shared/*.hpp)

.PHONY: test-development native-preflight
test-development:
	$(PYTHON) tests/run-development-tests.py

native-preflight:
	$(PYTHON) scripts/collect-native-preflight.py

$(OUT_BIN): Makefile $(OBJS) Ath10kMac.kext/Contents/Info.plist scripts/prepare-kext-info.py
	@mkdir -p "$(OUT_KEXT)/Contents/MacOS"
	$(PYTHON) scripts/prepare-kext-info.py $(NATIVE) "$(OUT_KEXT)/Contents/Info.plist" --diagnostic-stage $(DIAGNOSTIC_STAGE) $(if $(filter 1,$(P256_DIAGNOSTIC)),--p256-diagnostic,) $(if $(filter 1,$(VENTURA)),--ventura,)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

clean:
	rm -rf $(BUILD_DIR)

ifeq ($(NATIVE),1)
$(OBJS): $(wildcard src/kext/native/*)
endif

$(OBJS): src/kext/Ath10kDiagnostics.hpp

# Follow actual transitive project/vendor includes, including ABI-only changes.
-include $(OBJS:.o=.d)

# Offline symbol availability probe only. It is not part of the driver bundle,
# has no IOKit personality, never calls crypto, and refuses module start/stop.
CRYPTO_PROBE_DIR := $(PROJ_ROOT)/build/sae-crypto-link-probe
CRYPTO_PROBE_KEXT := $(CRYPTO_PROBE_DIR)/Ath10kCryptoLinkProbe.kext
CRYPTO_PROBE_BIN := $(CRYPTO_PROBE_KEXT)/Contents/MacOS/Ath10kCryptoLinkProbe
.PHONY: sae-crypto-link-probe
sae-crypto-link-probe: check-sdk $(CRYPTO_PROBE_BIN)
	$(PYTHON) scripts/check-kext-symbols.py "$(CRYPTO_PROBE_BIN)"

$(CRYPTO_PROBE_DIR):
	@mkdir -p $@

$(CRYPTO_PROBE_DIR)/probe.o: tools/sae/corecrypto-link-probe.c Makefile | $(CRYPTO_PROBE_DIR)
	$(CC) $(KFLAGS) -c $< -o $@

$(CRYPTO_PROBE_BIN): $(CRYPTO_PROBE_DIR)/probe.o tools/sae/CoreCryptoLinkProbe.plist
	@mkdir -p "$(CRYPTO_PROBE_KEXT)/Contents/MacOS"
	cp tools/sae/CoreCryptoLinkProbe.plist "$(CRYPTO_PROBE_KEXT)/Contents/Info.plist"
	$(LD) $(LDFLAGS) -o $@ $(CRYPTO_PROBE_DIR)/probe.o

-include $(CRYPTO_PROBE_DIR)/probe.d
-include $(CRYPTO_PROBE_DIR)/p256-kernel-compile.d

# Executes math tests in an ordinary macOS process, not in the kernel.
P256_CHECK_BIN := $(CRYPTO_PROBE_DIR)/p256-userland-check
.PHONY: sae-p256-userland-check sae-p256-kernel-compile
sae-p256-userland-check: $(P256_CHECK_BIN)
	$(P256_CHECK_BIN)

$(P256_CHECK_BIN): tools/sae/p256-userland-check.cpp src/shared/Ath10kP256CoreCrypto.hpp src/shared/Ath10kSAE.hpp src/shared/Ath10kSAEWire.hpp src/shared/Ath10kSAEExchange.hpp src/shared/Ath10kSHA256.hpp src/shared/Ath10kSecurity.hpp src/shared/Ath10kRSN.hpp tests/p256_corecrypto_fixtures.hpp Makefile | $(CRYPTO_PROBE_DIR)
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $< -o $@

sae-p256-kernel-compile: $(CRYPTO_PROBE_DIR)/p256-kernel-compile.o
	$(PYTHON) scripts/check-kext-symbols.py "$<"

$(CRYPTO_PROBE_DIR)/p256-kernel-compile.o: tools/sae/p256-kernel-compile.cpp src/shared/Ath10kP256CoreCrypto.hpp src/shared/Ath10kSecurity.hpp Makefile | $(CRYPTO_PROBE_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(CRYPTO_PROBE_DIR)/p256-kernel-compile.d

# Typed offline candidate: refuses module start; never part of the driver bundle.
TYPED_PROBE_KEXT := $(CRYPTO_PROBE_DIR)/Ath10kCryptoTypedProbe.kext
TYPED_PROBE_BIN := $(TYPED_PROBE_KEXT)/Contents/MacOS/Ath10kCryptoTypedProbe
.PHONY: sae-crypto-typed-link-probe
sae-crypto-typed-link-probe: check-sdk $(TYPED_PROBE_BIN)
	$(PYTHON) scripts/check-kext-symbols.py "$(TYPED_PROBE_BIN)"

$(CRYPTO_PROBE_DIR)/typed-probe.o: tools/sae/corecrypto-typed-link-probe.cpp src/kext/native/Ath10kP256KernelBinding.hpp src/shared/Ath10kP256BackendCheck.hpp src/shared/Ath10kP256CoreCrypto.hpp Makefile | $(CRYPTO_PROBE_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TYPED_PROBE_BIN): $(CRYPTO_PROBE_DIR)/typed-probe.o tools/sae/CoreCryptoTypedProbe.plist
	@mkdir -p "$(TYPED_PROBE_KEXT)/Contents/MacOS"
	cp tools/sae/CoreCryptoTypedProbe.plist "$(TYPED_PROBE_KEXT)/Contents/Info.plist"
	$(LD) $(LDFLAGS) -o $@ $(CRYPTO_PROBE_DIR)/typed-probe.o

-include $(CRYPTO_PROBE_DIR)/typed-probe.d
