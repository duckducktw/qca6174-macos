/* SPDX-License-Identifier: ISC */
#include "test.h"
#include "suites.h"

#include "../src/common/fw_load_sm.h"

static void test_blocked_without_parse(void)
{
    struct qca6174_fw_sm sm;
    qca6174_fw_sm_init(&sm);
    /* Cannot advance until the caller flags a successful parse. */
    CHECK_EQ_U(qca6174_fw_sm_advance(&sm), -QCA6174_ENOTSUP);
    CHECK_EQ_U(sm.state, QCA6174_FW_STATE_RESET);
}

static void test_reaches_board_then_stops(void)
{
    struct qca6174_fw_sm sm;
    qca6174_fw_sm_init(&sm);
    sm.fw_parsed_ok = true;
    sm.board_selected_ok = true;

    CHECK_EQ_U(qca6174_fw_sm_advance(&sm), 0);
    CHECK_EQ_U(sm.state, QCA6174_FW_STATE_FW_PARSED);

    CHECK_EQ_U(qca6174_fw_sm_advance(&sm), 0);
    CHECK_EQ_U(sm.state, QCA6174_FW_STATE_BOARD_SELECTED);

    /* BMI and everything past it must be ENOTSUP, and the state must NOT
     * advance past BOARD_SELECTED. This is the truthfulness guarantee: the SM
     * can never claim a connected/L2 state. */
    for (int i = 0; i < 5; i++) {
        CHECK_EQ_U(qca6174_fw_sm_advance(&sm), -QCA6174_ENOTSUP);
        CHECK_EQ_U(sm.state, QCA6174_FW_STATE_BOARD_SELECTED);
    }
    CHECK(sm.last_msg != NULL);
}

static void test_state_names(void)
{
    CHECK_STR_EQ(qca6174_fw_state_name(QCA6174_FW_STATE_RESET),
                 strlen(qca6174_fw_state_name(QCA6174_FW_STATE_RESET)), "RESET");
    CHECK_STR_EQ(qca6174_fw_state_name(QCA6174_FW_STATE_BMI_DONE),
                 strlen(qca6174_fw_state_name(QCA6174_FW_STATE_BMI_DONE)),
                 "BMI_DONE");
}

void suite_fw_load_sm(void)
{
    RUN(test_blocked_without_parse);
    RUN(test_reaches_board_then_stops);
    RUN(test_state_names);
}
