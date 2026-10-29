/* SPDX-License-Identifier: ISC */
/*
 * QCA6174 hw3.0 register map + MMIO bounds-checking helpers.
 *
 * !! VERIFICATION STATUS !!
 * -------------------------
 * The numeric register offsets below are pinned to upstream Linux ath10k as
 * recorded in docs/ATTRIBUTION.md. Host tests verify arithmetic and bounds;
 * direct chip-id MMIO remains disabled until its HIF/BAR access path is
 * independently validated on QCA6174.
 *
 * What IS verified here is the ARITHMETIC AND BOUNDS logic in qca6174_regs.c:
 * address computation, width/alignment rules, and the overflow-safe window
 * check are unit-tested on the host and are correct independent of the exact
 * base-address values.
 *
 * Pure freestanding C11; shared verbatim between host tests and the kext.
 */
#ifndef QCA6174_REGS_H
#define QCA6174_REGS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- device identity (PCI) --------------------------------------------- */
#define QCA6174_PCI_VENDOR_ID      0x168cu
#define QCA6174_PCI_DEVICE_ID      0x003eu
#define QCA6174_SUBSYS_VENDOR_ID   0x11adu /* Lite-On */
#define QCA6174_SUBSYS_DEVICE_ID   0x0807u

/* ath10k target/BMI hardware-version signatures (hw.h). These are not raw
 * SOC chip-id register values. */
#define QCA6174_HW_1_0_VERSION     0x05000000u
#define QCA6174_HW_1_1_VERSION     0x05000001u
#define QCA6174_HW_1_3_VERSION     0x05000003u
#define QCA6174_HW_2_1_VERSION     0x05010000u
#define QCA6174_HW_3_0_VERSION     0x05020000u
#define QCA6174_HW_3_2_VERSION     0x05030000u

/* ---- register window / BAR0 -------------------------------------------- */
/* The target-memory access window is 1 MiB. This is a conservative lower
 * bound for BAR0 mapping validation, not a claim that every offset below it is
 * a safe host-visible register. */
#define QCA6174_REG_WINDOW_SIZE    0x00100000u

/* QCA6174 qca6174_regs in upstream ath10k hw.c. Verified against Linux
 * Code Browser revision v6.19-rc8-185-g2687c848e578 on 2026-10-29. The exact
 * host access path still must be verified before enabling this read in M0. */
#define QCA6174_SOC_CHIP_ID_OFFSET 0x000000f0u

/* Do not derive ath10k's target/BMI hardware-version signature from the raw
 * SOC chip-id register. They are different identifiers. */

/* ---- Copy Engine (CE) layout ------------------------------------------- */
/* qca6174_regs: CE0..CE7 are contiguous 0x400-byte blocks. Verified against
 * the same upstream revision. */
#define QCA6174_CE_COUNT           8u
#define QCA6174_CE_WRAPPER_BASE    0x00034000u
#define QCA6174_CE0_BASE           0x00034400u
#define QCA6174_CE_STRIDE          0x00000400u

/* qcax_ce_regs offsets from upstream ath10k hw.c. */
#define QCA6174_CE_CTRL_ADDR       0x0010u
#define QCA6174_CE_SRC_WR_INDEX    0x003cu
#define QCA6174_CE_DST_WR_INDEX    0x0040u

/* ---- bounds-checking helpers (logic is verified, see qca6174_regs.c) ---- */

/* True iff a BAR of `bar_len` bytes is large enough to hold the register
 * window the driver will touch. */
bool qca6174_bar_window_ok(uint64_t bar_len);

/*
 * Compute the BAR0 offset of per-CE register `reg_off` for Copy Engine
 * `ce_id`. Returns true and writes *out on success; false if ce_id is out of
 * range or the computation would overflow 32 bits.
 */
bool qca6174_ce_reg_addr(unsigned ce_id, uint32_t reg_off, uint32_t *out);

/*
 * The single gate every MMIO access must pass before touching the mapping.
 * Enforces: width in {1,2,4}; natural alignment; and addr+width within
 * bar_len with no integer overflow. Returns true iff the access is safe.
 */
bool qca6174_mmio_access_ok(uint64_t bar_len, uint32_t addr, uint32_t width);

/* Placeholder: raw SOC chip-id is not the BMI target-version signature.
 * Returns 0 until the raw register bitfield is pinned. */
uint32_t qca6174_chip_id_to_hw_version(uint32_t chip_id);

#ifdef __cplusplus
}
#endif

#endif /* QCA6174_REGS_H */
