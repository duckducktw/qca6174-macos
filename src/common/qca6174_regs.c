/* SPDX-License-Identifier: ISC */
#include "qca6174_regs.h"

bool qca6174_bar_window_ok(uint64_t bar_len)
{
    return bar_len >= (uint64_t)QCA6174_REG_WINDOW_SIZE;
}

bool qca6174_ce_reg_addr(unsigned ce_id, uint32_t reg_off, uint32_t *out)
{
    if (!out)
        return false;
    if (ce_id >= QCA6174_CE_COUNT)
        return false;

    /* base = CE0_BASE + ce_id * STRIDE + reg_off, all in 64-bit to detect
     * 32-bit overflow before narrowing. */
    uint64_t addr = (uint64_t)QCA6174_CE0_BASE
                  + (uint64_t)ce_id * (uint64_t)QCA6174_CE_STRIDE
                  + (uint64_t)reg_off;
    if (addr > 0xffffffffull)
        return false;

    *out = (uint32_t)addr;
    return true;
}

bool qca6174_mmio_access_ok(uint64_t bar_len, uint32_t addr, uint32_t width)
{
    if (width != 1u && width != 2u && width != 4u)
        return false;
    if ((addr & (width - 1u)) != 0u) /* natural alignment */
        return false;

    /* addr + width must fit within the mapping. Promote to 64-bit so the sum
     * cannot wrap. */
    uint64_t end = (uint64_t)addr + (uint64_t)width;
    return end <= bar_len;
}

/* `chip_id` is a raw SOC register value, not the ath10k BMI target-version
 * signature used by qca6174_hw_params_for_version(). Keep this helper as a
 * placeholder until the raw chip-id bitfield is pinned from upstream. */
uint32_t qca6174_chip_id_to_hw_version(uint32_t chip_id)
{
    (void)chip_id;
    return 0u;
}
