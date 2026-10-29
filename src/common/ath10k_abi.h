/* SPDX-License-Identifier: ISC */
/*
 * ath10k firmware/board container ABI constants for QCA6174.
 *
 * SOURCE / ATTRIBUTION
 * --------------------
 * These constants describe the *public, on-disk* ABI of the firmware images
 * shipped in linux-firmware (ath10k/QCA6174/hw3.0/). The layout mirrors the
 * upstream Linux ath10k driver:
 *
 *   drivers/net/wireless/ath/ath10k/core.h   (enum ath10k_fw_ie_type,
 *                                              ATH10K_FIRMWARE_MAGIC)
 *   drivers/net/wireless/ath/ath10k/core.c   (API-N container parser)
 *   drivers/net/wireless/ath/ath10k/bmi.h    (board data container)
 *   Upstream: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
 *   License : ISC (see docs/ATTRIBUTION.md)
 *
 * These specific values were ADDITIONALLY verified empirically on this host by
 * inspecting the real fixture files with xxd:
 *   /lib/firmware/ath10k/QCA6174/hw3.0/firmware-6.bin.zst
 *   /lib/firmware/ath10k/QCA6174/hw3.0/board-2.bin.zst
 * (magic strings, IE id ordering, the "WLAN.RM.4.4.1-00309-" version string).
 * See artifacts/verification/ for the captured hexdumps.
 *
 * Only the container ABI lives here. Hardware register offsets (which could NOT
 * be verified against upstream source on this host) are kept separately in
 * qca6174_regs.h and are explicitly flagged for verification before any real
 * hardware access.
 */
#ifndef QCA6174_ATH10K_ABI_H
#define QCA6174_ATH10K_ABI_H

#include <stdint.h>
#include <stddef.h>

/* ---- firmware-N.bin container (API 2+) --------------------------------- */

/* NUL-terminated magic; strlen()==10, stored length (incl. NUL) == 11,
 * 4-byte aligned header region therefore occupies 12 bytes. */
#define ATH10K_FW_MAGIC        "QCA-ATH10K"
#define ATH10K_FW_MAGIC_LEN    10u  /* strlen, excluding NUL */

/* Information-element identifiers inside firmware-N.bin.
 * core.h : enum ath10k_fw_ie_type. */
enum ath10k_fw_ie_type {
    ATH10K_FW_IE_FW_VERSION       = 0,
    ATH10K_FW_IE_TIMESTAMP        = 1,
    ATH10K_FW_IE_FEATURES         = 2,
    ATH10K_FW_IE_FW_IMAGE         = 3,
    ATH10K_FW_IE_OTP_IMAGE        = 4,
    ATH10K_FW_IE_WMI_OP_VERSION   = 5,
    ATH10K_FW_IE_HTT_OP_VERSION   = 6,
    ATH10K_FW_IE_FW_CODE_SWAP_IMAGE = 7,
};

/* ---- board-2.bin container --------------------------------------------- */

/* strlen()==16, stored length (incl. NUL) == 17, 4-byte aligned header region
 * therefore occupies 20 bytes. */
#define ATH10K_BOARD_MAGIC      "QCA-ATH10K-BOARD"
#define ATH10K_BOARD_MAGIC_LEN  16u  /* strlen, excluding NUL */

/* Top-level board container IE ids. bmi.h : enum ath10k_bd_ie_type. */
enum ath10k_bd_ie_type {
    ATH10K_BD_IE_BOARD     = 0,
    ATH10K_BD_IE_BOARD_EXT = 1,
};

/* Sub-IE ids nested inside an ATH10K_BD_IE_BOARD element.
 * bmi.h : enum ath10k_bd_ie_board_type. */
enum ath10k_bd_ie_board_type {
    ATH10K_BD_IE_BOARD_NAME = 0,
    ATH10K_BD_IE_BOARD_DATA = 1,
};

/* 4-byte alignment helper used throughout the ath10k containers. */
static inline size_t ath10k_align4(size_t x)
{
    return (x + 3u) & ~((size_t)3u);
}

#endif /* QCA6174_ATH10K_ABI_H */
