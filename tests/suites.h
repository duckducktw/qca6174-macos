/* SPDX-License-Identifier: ISC */
#ifndef QCA6174_TEST_SUITES_H
#define QCA6174_TEST_SUITES_H

/* Each suite registers its own RUN()s against the shared counters. */
void suite_fw_file(void);
void suite_board_file(void);
void suite_regs(void);
void suite_hw_params(void);
void suite_fw_load_sm(void);

#endif
