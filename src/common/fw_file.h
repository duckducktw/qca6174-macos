/* SPDX-License-Identifier: ISC */
/*
 * Host-testable parser for the ath10k firmware-N.bin container (API 2+).
 *
 * This code is pure, freestanding C11: no IOKit, no libc beyond <string.h>
 * (memcmp). It can therefore be unit-tested on a Linux host and #included by
 * the macOS kext glue unchanged. It never allocates and never mutates the
 * input; all outputs are pointers into the caller-owned buffer.
 *
 * ABI reference: src/common/ath10k_abi.h
 */
#ifndef QCA6174_FW_FILE_H
#define QCA6174_FW_FILE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ath10k_parse_result {
    ATH10K_PARSE_OK        = 0,
    ATH10K_PARSE_E_NULL    = 1, /* NULL data or out pointer */
    ATH10K_PARSE_E_SHORT   = 2, /* buffer smaller than the fixed header */
    ATH10K_PARSE_E_MAGIC   = 3, /* magic string mismatch */
    ATH10K_PARSE_E_TRUNC   = 4, /* an IE length runs past the end of buffer */
    ATH10K_PARSE_E_BADLEN  = 5, /* a fixed-size IE (timestamp/op ver) had wrong length */
};

const char *ath10k_parse_strerror(int code);

/*
 * Parsed view of a firmware-N.bin. All pointers reference the input buffer and
 * are valid only as long as that buffer lives. A pointer is NULL and its _len
 * is 0 when the corresponding IE was absent.
 */
struct ath10k_fw_file {
    const uint8_t *version;        /* ATH10K_FW_IE_FW_VERSION (not NUL-guaranteed) */
    size_t         version_len;

    bool           has_timestamp;
    uint32_t       timestamp;      /* ATH10K_FW_IE_TIMESTAMP */

    const uint8_t *features;       /* ATH10K_FW_IE_FEATURES bitmap */
    size_t         features_len;

    const uint8_t *fw_image;       /* ATH10K_FW_IE_FW_IMAGE */
    size_t         fw_image_len;

    const uint8_t *otp_image;      /* ATH10K_FW_IE_OTP_IMAGE */
    size_t         otp_image_len;

    const uint8_t *codeswap_image; /* ATH10K_FW_IE_FW_CODE_SWAP_IMAGE */
    size_t         codeswap_len;

    bool           has_wmi_op_version;
    uint32_t       wmi_op_version; /* ATH10K_FW_IE_WMI_OP_VERSION */

    bool           has_htt_op_version;
    uint32_t       htt_op_version; /* ATH10K_FW_IE_HTT_OP_VERSION */

    unsigned       n_ies;          /* total IEs walked (incl. unknown) */
    unsigned       n_unknown_ies;  /* IEs with an id we do not interpret */
};

/*
 * Parse `len` bytes at `data` into `*out`. Returns ATH10K_PARSE_OK on success
 * or a negative-semantics error code from enum ath10k_parse_result. Unknown IE
 * ids are counted and skipped (forward compatibility), never fatal. On any
 * error `*out` is left zero-initialised.
 */
int ath10k_fw_file_parse(const uint8_t *data, size_t len,
                         struct ath10k_fw_file *out);

#ifdef __cplusplus
}
#endif

#endif /* QCA6174_FW_FILE_H */
