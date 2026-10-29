/* SPDX-License-Identifier: ISC */
/* Unit-test runner: crafted buffers only, no external fixtures required. */
#include "test.h"
#include "suites.h"

TEST_GLOBALS

int main(void)
{
    printf("== QCA6174 host unit tests ==\n");
    suite_fw_file();
    suite_board_file();
    suite_regs();
    suite_hw_params();
    suite_fw_load_sm();
    return test_summary();
}
