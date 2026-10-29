/* SPDX-License-Identifier: ISC */
#include "test.h"
#include "suites.h"

#include "../src/common/hw_params.h"
#include "../src/common/qca6174_regs.h"

static void test_lookup(void)
{
    const struct qca6174_hw_params *p =
        qca6174_hw_params_for_version(QCA6174_HW_3_0_VERSION);
    CHECK(p != NULL);
    if (p) {
        CHECK_STR_EQ(p->fw_dir, strlen(p->fw_dir), "ath10k/QCA6174/hw3.0");
        CHECK_STR_EQ(p->preferred_fw, strlen(p->preferred_fw), "firmware-6.bin");
        CHECK_STR_EQ(p->board_file, strlen(p->board_file), "board-2.bin");
        CHECK_EQ_U(p->ce_count, QCA6174_CE_COUNT);
    }

    /* Unknown version -> NULL (no fake support). */
    CHECK(qca6174_hw_params_for_version(0xdeadbeef) == NULL);
}

static void test_board_key(void)
{
    char buf[128];
    size_t n = qca6174_build_board_key(buf, sizeof(buf),
                                       QCA6174_PCI_VENDOR_ID,
                                       QCA6174_PCI_DEVICE_ID,
                                       QCA6174_SUBSYS_VENDOR_ID,
                                       QCA6174_SUBSYS_DEVICE_ID);
    const char *expect =
        "bus=pci,vendor=168c,device=003e,subsystem-vendor=11ad,subsystem-device=0807";
    CHECK(n != (size_t)-1);
    CHECK_STR_EQ(buf, n, expect);
    CHECK_EQ_U(buf[n], 0); /* NUL-terminated */
}

static void test_board_key_overflow(void)
{
    char small[10];
    size_t n = qca6174_build_board_key(small, sizeof(small),
                                       0x168c, 0x003e, 0x11ad, 0x0807);
    CHECK_EQ_U(n, (size_t)-1); /* refuses to overflow */

    /* Zero-length buffer rejected. */
    CHECK_EQ_U(qca6174_build_board_key(small, 0, 1, 2, 3, 4), (size_t)-1);

    /* A spread of tiny buffers must all cleanly refuse (ASAN would trap any
     * out-of-bounds write in the hand-rolled formatter). */
    char tiny[32];
    for (size_t cap = 1; cap <= 20; cap++)
        CHECK_EQ_U(qca6174_build_board_key(tiny, cap, 0x168c, 0x003e,
                                           0x11ad, 0x0807), (size_t)-1);
}

void suite_hw_params(void)
{
    RUN(test_lookup);
    RUN(test_board_key);
    RUN(test_board_key_overflow);
}
