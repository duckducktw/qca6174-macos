/* SPDX-License-Identifier: ISC */
/*
 * C++ compile/link smoke for the kext<->common boundary.
 *
 * The real kext under kext/QCA6174Wifi cannot be built on Linux -- it needs
 * the macOS Kernel framework. What CAN be checked here is that the shared
 * host-tested C logic includes and links cleanly from C++ through the same
 * `extern "C"` boundary the kext uses. This catches header/linkage breakage
 * without pretending to build a kext.
 */
extern "C" {
#include "../src/common/qca6174_regs.h"
#include "../src/common/hw_params.h"
#include "../src/common/fw_load_sm.h"
}

#include <cstdio>
#include <cstring>

int main()
{
    // Mirror the exact calls QCA6174Wifi::start() makes.
    if (!qca6174_bar_window_ok(QCA6174_REG_WINDOW_SIZE)) {
        std::fprintf(stderr, "bar window check wrong\n");
        return 1;
    }
    if (!qca6174_mmio_access_ok(QCA6174_REG_WINDOW_SIZE,
                                QCA6174_SOC_CHIP_ID_OFFSET, 4)) {
        std::fprintf(stderr, "chip-id offset failed bounds\n");
        return 1;
    }

    // The common target-version lookup must remain usable from C++; raw SOC
    // chip-id decoding is intentionally not part of this boundary.
    const struct qca6174_hw_params *hw =
        qca6174_hw_params_for_version(QCA6174_HW_3_0_VERSION);
    if (!hw || std::strcmp(hw->name, "qca6174 hw3.0") != 0) {
        std::fprintf(stderr, "hw params lookup wrong\n");
        return 1;
    }

    struct qca6174_fw_sm sm;
    qca6174_fw_sm_init(&sm);
    sm.fw_parsed_ok = true;
    sm.board_selected_ok = true;
    (void)qca6174_fw_sm_advance(&sm); // -> FW_PARSED
    (void)qca6174_fw_sm_advance(&sm); // -> BOARD_SELECTED
    if (qca6174_fw_sm_advance(&sm) != -QCA6174_ENOTSUP) {
        std::fprintf(stderr, "SM should be ENOTSUP at BMI\n");
        return 1;
    }

    std::printf("cxx glue smoke: OK (hw=%s, chip offset bounds arithmetic, "
                "SM stops at BMI/ENOTSUP)\n", hw->name);
    return 0;
}
