/* SPDX-License-Identifier: ISC */
/*
 * Host-testable parser for the ath10k board-2.bin container.
 *
 * board-2.bin is a two-level TLV: top-level elements (ATH10K_BD_IE_BOARD) each
 * contain a run of nested sub-elements -- one or more BOARD_NAME strings
 * followed by exactly one BOARD_DATA blob. The driver selects the board blob
 * whose name matches a key derived from the PCI/subsystem ids, e.g.
 *   "bus=pci,vendor=168c,device=003e,subsystem-vendor=11ad,subsystem-device=0807"
 *
 * Pure freestanding C11; no IOKit, no allocation, input never mutated.
 * ABI reference: src/common/ath10k_abi.h
 */
#ifndef QCA6174_BOARD_FILE_H
#define QCA6174_BOARD_FILE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "fw_file.h" /* reuse enum ath10k_parse_result + strerror */

#ifdef __cplusplus
extern "C" {
#endif

/* Upper bound on BOARD_NAME aliases buffered per board element. Names beyond
 * this are counted in summary.names_overflow and skipped (never overflow the
 * stack). Real board-2.bin files use at most a handful. */
#define ATH10K_MAX_BOARD_NAMES 32u

/* One (name -> data) association emitted by the walker. Pointers reference the
 * input buffer. */
struct ath10k_board_entry {
    const uint8_t *name;
    size_t         name_len;
    const uint8_t *data;
    size_t         data_len;
    bool           ext;        /* true if from ATH10K_BD_IE_BOARD_EXT */
};

struct ath10k_board_summary {
    unsigned board_elems;      /* top-level BOARD / BOARD_EXT elements */
    unsigned names;            /* total BOARD_NAME sub-elements */
    unsigned data_blocks;      /* total BOARD_DATA sub-elements */
    unsigned entries;          /* (name,data) pairs emitted */
    unsigned unknown_top_ies;  /* top-level ids we do not interpret */
    unsigned unknown_sub_ies;  /* sub ids we do not interpret */
    unsigned names_overflow;   /* names dropped due to ATH10K_MAX_BOARD_NAMES */
};

/* Callback invoked once per (name,data) pair. */
typedef void (*ath10k_board_entry_cb)(const struct ath10k_board_entry *e,
                                       void *ctx);

/*
 * Walk the whole container, invoking `cb` (may be NULL) for every (name,data)
 * pair and filling `*summary` (may be NULL). Returns ATH10K_PARSE_OK or an
 * enum ath10k_parse_result error. Malformed/truncated input is rejected.
 */
int ath10k_board_file_walk(const uint8_t *data, size_t len,
                           ath10k_board_entry_cb cb, void *ctx,
                           struct ath10k_board_summary *summary);

/*
 * Convenience: find the first entry whose name exactly equals
 * [key, key+key_len). Returns ATH10K_PARSE_OK and fills *found when matched;
 * returns ATH10K_PARSE_OK with found->data == NULL when the container parsed
 * cleanly but held no match; returns an error code on malformed input.
 */
int ath10k_board_file_find(const uint8_t *data, size_t len,
                           const char *key, size_t key_len,
                           struct ath10k_board_entry *found);

#ifdef __cplusplus
}
#endif

#endif /* QCA6174_BOARD_FILE_H */
