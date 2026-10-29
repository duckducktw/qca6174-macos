/* SPDX-License-Identifier: ISC */
#include "hw_params.h"
#include "qca6174_regs.h"

#include <string.h> /* strlen */

/* The target board for this project: QCA6174 hw3.0 (firmware-6 API). The hw2.1
 * entry is listed for completeness/matching but this project targets hw3.0. */
static const struct qca6174_hw_params k_table[] = {
    {
        .hw_version   = QCA6174_HW_3_0_VERSION,
        .name         = "qca6174 hw3.0",
        .fw_dir       = "ath10k/QCA6174/hw3.0",
        .preferred_fw = "firmware-6.bin",
        .board_file   = "board-2.bin",
        .ce_count     = QCA6174_CE_COUNT,
    },
    {
        .hw_version   = QCA6174_HW_3_2_VERSION,
        .name         = "qca6174 hw3.2",
        .fw_dir       = "ath10k/QCA6174/hw3.0", /* shares hw3.0 fw dir upstream */
        .preferred_fw = "firmware-6.bin",
        .board_file   = "board-2.bin",
        .ce_count     = QCA6174_CE_COUNT,
    },
    {
        .hw_version   = QCA6174_HW_2_1_VERSION,
        .name         = "qca6174 hw2.1",
        .fw_dir       = "ath10k/QCA6174/hw2.1",
        .preferred_fw = "firmware-5.bin",
        .board_file   = "board-2.bin",
        .ce_count     = QCA6174_CE_COUNT,
    },
};

const struct qca6174_hw_params *
qca6174_hw_params_for_version(uint32_t hw_version)
{
    for (size_t i = 0; i < sizeof(k_table) / sizeof(k_table[0]); i++) {
        if (k_table[i].hw_version == hw_version)
            return &k_table[i];
    }
    return NULL;
}

/* Append literal `s` to buf at *pos (buflen total). Returns false on overflow. */
static bool put_str(char *buf, size_t buflen, size_t *pos, const char *s)
{
    size_t n = strlen(s);
    if (n > buflen || *pos > buflen - n)
        return false;
    memcpy(buf + *pos, s, n);
    *pos += n;
    return true;
}

/* Append `v` as exactly 4 lowercase hex digits. Returns false on overflow. */
static bool put_hex16(char *buf, size_t buflen, size_t *pos, uint16_t v)
{
    static const char hexd[] = "0123456789abcdef";
    if (buflen < 4u || *pos > buflen - 4u) /* buflen<4 guard avoids underflow */
        return false;
    buf[(*pos)++] = hexd[(v >> 12) & 0xf];
    buf[(*pos)++] = hexd[(v >> 8) & 0xf];
    buf[(*pos)++] = hexd[(v >> 4) & 0xf];
    buf[(*pos)++] = hexd[v & 0xf];
    return true;
}

size_t qca6174_build_board_key(char *buf, size_t buflen,
                               uint16_t vendor, uint16_t device,
                               uint16_t subsys_vendor, uint16_t subsys_device)
{
    if (!buf || buflen == 0)
        return (size_t)-1;

    size_t pos = 0;
    bool ok = true;
    ok = ok && put_str(buf, buflen, &pos, "bus=pci,vendor=");
    ok = ok && put_hex16(buf, buflen, &pos, vendor);
    ok = ok && put_str(buf, buflen, &pos, ",device=");
    ok = ok && put_hex16(buf, buflen, &pos, device);
    ok = ok && put_str(buf, buflen, &pos, ",subsystem-vendor=");
    ok = ok && put_hex16(buf, buflen, &pos, subsys_vendor);
    ok = ok && put_str(buf, buflen, &pos, ",subsystem-device=");
    ok = ok && put_hex16(buf, buflen, &pos, subsys_device);

    if (!ok || pos >= buflen) /* need room for NUL */
        return (size_t)-1;

    buf[pos] = '\0';
    return pos;
}
