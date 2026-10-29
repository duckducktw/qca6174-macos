/* SPDX-License-Identifier: ISC */
#include "fw_load_sm.h"

void qca6174_fw_sm_init(struct qca6174_fw_sm *sm)
{
    if (!sm)
        return;
    sm->state = QCA6174_FW_STATE_RESET;
    sm->fw_parsed_ok = false;
    sm->board_selected_ok = false;
    sm->last_msg = "reset";
}

const char *qca6174_fw_state_name(enum qca6174_fw_state s)
{
    switch (s) {
    case QCA6174_FW_STATE_RESET:          return "RESET";
    case QCA6174_FW_STATE_FW_PARSED:      return "FW_PARSED";
    case QCA6174_FW_STATE_BOARD_SELECTED: return "BOARD_SELECTED";
    case QCA6174_FW_STATE_BMI_DONE:       return "BMI_DONE";
    case QCA6174_FW_STATE_HTC_READY:      return "HTC_READY";
    case QCA6174_FW_STATE_WMI_READY:      return "WMI_READY";
    case QCA6174_FW_STATE_HTT_READY:      return "HTT_READY";
    default:                              return "INVALID";
    }
}

int qca6174_fw_sm_advance(struct qca6174_fw_sm *sm)
{
    if (!sm)
        return -QCA6174_ENOTSUP;

    switch (sm->state) {
    case QCA6174_FW_STATE_RESET:
        if (!sm->fw_parsed_ok) {
            sm->last_msg = "firmware container not parsed yet";
            return -QCA6174_ENOTSUP;
        }
        sm->state = QCA6174_FW_STATE_FW_PARSED;
        sm->last_msg = "firmware container parsed";
        return 0;

    case QCA6174_FW_STATE_FW_PARSED:
        if (!sm->board_selected_ok) {
            sm->last_msg = "board data not selected yet";
            return -QCA6174_ENOTSUP;
        }
        sm->state = QCA6174_FW_STATE_BOARD_SELECTED;
        sm->last_msg = "board data selected";
        return 0;

    case QCA6174_FW_STATE_BOARD_SELECTED:
        /* Next would be BMI: reset target, write firmware over BMI, etc.
         * Not implemented -- requires live MMIO + DMA. */
        sm->last_msg = "BMI not implemented (ENOTSUP): needs MMIO/DMA to device";
        return -QCA6174_ENOTSUP;

    case QCA6174_FW_STATE_BMI_DONE:
        sm->last_msg = "HTC not implemented (ENOTSUP)";
        return -QCA6174_ENOTSUP;
    case QCA6174_FW_STATE_HTC_READY:
        sm->last_msg = "WMI not implemented (ENOTSUP)";
        return -QCA6174_ENOTSUP;
    case QCA6174_FW_STATE_WMI_READY:
        sm->last_msg = "HTT not implemented (ENOTSUP)";
        return -QCA6174_ENOTSUP;
    case QCA6174_FW_STATE_HTT_READY:
    default:
        sm->last_msg = "no further stages implemented (ENOTSUP)";
        return -QCA6174_ENOTSUP;
    }
}
