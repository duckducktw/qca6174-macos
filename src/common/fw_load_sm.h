/* SPDX-License-Identifier: ISC */
/*
 * QCA6174 firmware bring-up state machine SKELETON.
 *
 * This models the ordered stages ath10k performs to bring the chip to a usable
 * state. Only the host-side, no-hardware stages are implemented here:
 *   - parsing the firmware container  (QCA6174_FW_STATE_FW_PARSED)
 *   - selecting the board data blob    (QCA6174_FW_STATE_BOARD_SELECTED)
 *
 * Every stage that requires talking to the device over MMIO/DMA -- BMI (boot
 * loader messaging), HTC (host-target control), WMI (firmware command), and
 * HTT (host-target tx/rx) -- is deliberately NOT implemented and returns
 * -QCA6174_ENOTSUP. The state machine can NEVER report a "connected" / L2+
 * state. This is intentional per the project's truthfulness rule: we do not
 * stub unimplemented bring-up as success.
 */
#ifndef QCA6174_FW_LOAD_SM_H
#define QCA6174_FW_LOAD_SM_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Darwin/BSD ENOTSUP numeric value, used so the kext and host agree. */
#define QCA6174_ENOTSUP 45

enum qca6174_fw_state {
    QCA6174_FW_STATE_RESET = 0,
    QCA6174_FW_STATE_FW_PARSED,       /* implemented (host) */
    QCA6174_FW_STATE_BOARD_SELECTED,  /* implemented (host) */
    QCA6174_FW_STATE_BMI_DONE,        /* NOT IMPLEMENTED (needs hardware) */
    QCA6174_FW_STATE_HTC_READY,       /* NOT IMPLEMENTED */
    QCA6174_FW_STATE_WMI_READY,       /* NOT IMPLEMENTED */
    QCA6174_FW_STATE_HTT_READY,       /* NOT IMPLEMENTED */
};

struct qca6174_fw_sm {
    enum qca6174_fw_state state;
    bool  fw_parsed_ok;     /* caller sets after a successful container parse */
    bool  board_selected_ok;/* caller sets after board blob selection */
    const char *last_msg;   /* human-readable reason for the last result */
};

/* Reset to initial state. */
void qca6174_fw_sm_init(struct qca6174_fw_sm *sm);

const char *qca6174_fw_state_name(enum qca6174_fw_state s);

/*
 * Attempt to advance exactly one stage. Returns:
 *   0                 advanced into an implemented state
 *   -QCA6174_ENOTSUP  the next stage requires hardware and is not implemented
 * On -QCA6174_ENOTSUP the state is left unchanged and last_msg explains which
 * subsystem is missing. Preconditions (fw_parsed_ok / board_selected_ok) that
 * are not met also yield -QCA6174_ENOTSUP with an explanatory message.
 */
int qca6174_fw_sm_advance(struct qca6174_fw_sm *sm);

#ifdef __cplusplus
}
#endif

#endif /* QCA6174_FW_LOAD_SM_H */
