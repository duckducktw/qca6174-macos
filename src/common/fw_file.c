/* SPDX-License-Identifier: ISC */
#include "fw_file.h"
#include "ath10k_abi.h"

#include <string.h> /* memcmp */

const char *ath10k_parse_strerror(int code)
{
    switch (code) {
    case ATH10K_PARSE_OK:       return "ok";
    case ATH10K_PARSE_E_NULL:   return "null argument";
    case ATH10K_PARSE_E_SHORT:  return "buffer shorter than header";
    case ATH10K_PARSE_E_MAGIC:  return "bad magic";
    case ATH10K_PARSE_E_TRUNC:  return "information element truncated";
    case ATH10K_PARSE_E_BADLEN: return "fixed-size element wrong length";
    default:                    return "unknown error";
    }
}

/* Little-endian 32-bit read. Caller guarantees at least 4 bytes available. */
static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

int ath10k_fw_file_parse(const uint8_t *data, size_t len,
                         struct ath10k_fw_file *out)
{
    if (!data || !out)
        return ATH10K_PARSE_E_NULL;

    memset(out, 0, sizeof(*out));

    /* Header: magic string + NUL, then padded up to a 4-byte boundary. */
    const size_t magic_hdr = ath10k_align4(ATH10K_FW_MAGIC_LEN + 1u); /* = 12 */
    if (len < magic_hdr)
        return ATH10K_PARSE_E_SHORT;
    if (memcmp(data, ATH10K_FW_MAGIC, ATH10K_FW_MAGIC_LEN) != 0 ||
        data[ATH10K_FW_MAGIC_LEN] != '\0')
        return ATH10K_PARSE_E_MAGIC;

    const uint8_t *p = data + magic_hdr;
    size_t remaining = len - magic_hdr;

    /* Walk the IE list. Each IE: le32 id, le32 len, data[len], pad to 4. */
    while (remaining > 0) {
        if (remaining < 8) {
            /* Dangling bytes that cannot form an IE header. */
            memset(out, 0, sizeof(*out));
            return ATH10K_PARSE_E_TRUNC;
        }

        uint32_t id  = rd_le32(p);
        uint32_t ielen = rd_le32(p + 4);

        /* remaining >= 8 here, so (remaining - 8) cannot underflow. */
        if (ielen > remaining - 8) {
            memset(out, 0, sizeof(*out));
            return ATH10K_PARSE_E_TRUNC;
        }

        const uint8_t *body = p + 8;
        out->n_ies++;

        switch (id) {
        case ATH10K_FW_IE_FW_VERSION:
            out->version = body;
            out->version_len = ielen;
            break;
        case ATH10K_FW_IE_TIMESTAMP:
            if (ielen != 4) { memset(out, 0, sizeof(*out)); return ATH10K_PARSE_E_BADLEN; }
            out->has_timestamp = true;
            out->timestamp = rd_le32(body);
            break;
        case ATH10K_FW_IE_FEATURES:
            out->features = body;
            out->features_len = ielen;
            break;
        case ATH10K_FW_IE_FW_IMAGE:
            out->fw_image = body;
            out->fw_image_len = ielen;
            break;
        case ATH10K_FW_IE_OTP_IMAGE:
            out->otp_image = body;
            out->otp_image_len = ielen;
            break;
        case ATH10K_FW_IE_FW_CODE_SWAP_IMAGE:
            out->codeswap_image = body;
            out->codeswap_len = ielen;
            break;
        case ATH10K_FW_IE_WMI_OP_VERSION:
            if (ielen != 4) { memset(out, 0, sizeof(*out)); return ATH10K_PARSE_E_BADLEN; }
            out->has_wmi_op_version = true;
            out->wmi_op_version = rd_le32(body);
            break;
        case ATH10K_FW_IE_HTT_OP_VERSION:
            if (ielen != 4) { memset(out, 0, sizeof(*out)); return ATH10K_PARSE_E_BADLEN; }
            out->has_htt_op_version = true;
            out->htt_op_version = rd_le32(body);
            break;
        default:
            out->n_unknown_ies++;
            break;
        }

        /* Advance past header + body, then over 4-byte padding. The last IE in
         * a file may legally omit its trailing padding, so clamp. */
        size_t step = 8u + (size_t)ielen;
        p += step;
        remaining -= step;

        size_t pad = ath10k_align4((size_t)ielen) - (size_t)ielen; /* 0..3 */
        if (pad > remaining)
            pad = remaining;
        p += pad;
        remaining -= pad;
    }

    return ATH10K_PARSE_OK;
}
