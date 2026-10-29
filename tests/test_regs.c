/* SPDX-License-Identifier: ISC */
#include "test.h"
#include "suites.h"

#include "../src/common/qca6174_regs.h"

static void test_mmio_access_ok(void)
{
    const uint64_t bar = QCA6174_REG_WINDOW_SIZE; /* 1 MiB */

    /* Good aligned accesses. */
    CHECK(qca6174_mmio_access_ok(bar, 0, 4));
    CHECK(qca6174_mmio_access_ok(bar, 4, 4));
    CHECK(qca6174_mmio_access_ok(bar, 2, 2));
    CHECK(qca6174_mmio_access_ok(bar, 1, 1));
    CHECK(qca6174_mmio_access_ok(bar, (uint32_t)bar - 4, 4)); /* last word */

    /* Bad width. */
    CHECK(!qca6174_mmio_access_ok(bar, 0, 3));
    CHECK(!qca6174_mmio_access_ok(bar, 0, 8));
    CHECK(!qca6174_mmio_access_ok(bar, 0, 0));

    /* Misaligned. */
    CHECK(!qca6174_mmio_access_ok(bar, 1, 4));
    CHECK(!qca6174_mmio_access_ok(bar, 2, 4));
    CHECK(!qca6174_mmio_access_ok(bar, 1, 2));

    /* Out of bounds (including the last byte straddle). */
    CHECK(!qca6174_mmio_access_ok(bar, (uint32_t)bar, 4));
    CHECK(!qca6174_mmio_access_ok(bar, (uint32_t)bar - 2, 4));

    /* Overflow guard: with a 1 MiB BAR, addr+width would wrap to 0 under
     * 32-bit math and wrongly pass. It must be rejected (proves 64-bit sum). */
    CHECK(!qca6174_mmio_access_ok(bar, 0xfffffffcu, 4));
    /* With a full 4 GiB BAR the same top word genuinely fits exactly. */
    CHECK(qca6174_mmio_access_ok((uint64_t)0x100000000ull, 0xfffffffcu, 4));
}

static void test_bar_window(void)
{
    CHECK(qca6174_bar_window_ok(QCA6174_REG_WINDOW_SIZE));
    CHECK(qca6174_bar_window_ok(QCA6174_REG_WINDOW_SIZE + 1));
    CHECK(!qca6174_bar_window_ok(QCA6174_REG_WINDOW_SIZE - 1));
    CHECK(!qca6174_bar_window_ok(0));
}

static void test_ce_reg_addr(void)
{
    uint32_t a = 0;
    CHECK(qca6174_ce_reg_addr(0, QCA6174_CE_CTRL_ADDR, &a));
    CHECK_EQ_U(a, QCA6174_CE0_BASE + QCA6174_CE_CTRL_ADDR);

    CHECK(qca6174_ce_reg_addr(1, 0, &a));
    CHECK_EQ_U(a, QCA6174_CE0_BASE + QCA6174_CE_STRIDE);

    CHECK(qca6174_ce_reg_addr(QCA6174_CE_COUNT - 1, QCA6174_CE_DST_WR_INDEX, &a));
    CHECK_EQ_U(a, QCA6174_CE0_BASE
                  + (QCA6174_CE_COUNT - 1) * QCA6174_CE_STRIDE
                  + QCA6174_CE_DST_WR_INDEX);

    /* Out-of-range CE index rejected. */
    CHECK(!qca6174_ce_reg_addr(QCA6174_CE_COUNT, 0, &a));
    CHECK(!qca6174_ce_reg_addr(999, 0, &a));

    /* Every CE register must land inside the mapped window. */
    for (unsigned ce = 0; ce < QCA6174_CE_COUNT; ce++) {
        CHECK(qca6174_ce_reg_addr(ce, QCA6174_CE_DST_WR_INDEX, &a));
        CHECK(qca6174_mmio_access_ok(QCA6174_REG_WINDOW_SIZE, a, 4));
    }

    /* NULL out rejected. */
    CHECK(!qca6174_ce_reg_addr(0, 0, NULL));
}

static void test_chip_id_decode(void)
{
    /* Raw SOC chip-id is not the BMI target-version signature. Until the raw
     * field layout is pinned, the helper must refuse to fabricate a match. */
    CHECK_EQ_U(qca6174_chip_id_to_hw_version(0), 0);
    CHECK_EQ_U(qca6174_chip_id_to_hw_version(0xffffffffu), 0);
    CHECK_EQ_U(qca6174_chip_id_to_hw_version(QCA6174_HW_3_0_VERSION | 0x32), 0);
}

void suite_regs(void)
{
    RUN(test_mmio_access_ok);
    RUN(test_bar_window);
    RUN(test_ce_reg_addr);
    RUN(test_chip_id_decode);
}
