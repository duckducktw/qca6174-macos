/* SPDX-License-Identifier: ISC */
/*
 * QCA6174 hw-params table + board-data key builder.
 *
 * Maps a decoded chip hw-version to the firmware directory / file names the
 * driver would request, and builds the board-2.bin lookup key from PCI ids.
 * Pure freestanding C11 (no snprintf dependency): the key builder hand-formats
 * so it is identical on host and in the kext.
 */
#ifndef QCA6174_HW_PARAMS_H
#define QCA6174_HW_PARAMS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct qca6174_hw_params {
    uint32_t    hw_version;    /* QCA6174_HW_*_VERSION */
    const char *name;          /* human label, e.g. "qca6174 hw3.0" */
    const char *fw_dir;        /* linux-firmware subdir */
    const char *preferred_fw;  /* highest API image we target */
    const char *board_file;    /* board container file name */
    unsigned    ce_count;
};

/* Returns the matching entry, or NULL for an unknown/unsupported version. */
const struct qca6174_hw_params *
qca6174_hw_params_for_version(uint32_t hw_version);

/*
 * Build the board-2.bin lookup key for a PCI function into `buf`.
 * Produces exactly:
 *   bus=pci,vendor=VVVV,device=DDDD,subsystem-vendor=SSSS,subsystem-device=XXXX
 * with lowercase, zero-padded 4-digit hex. Returns the string length (not
 * counting the NUL) on success, or (size_t)-1 if `buf`/`buflen` cannot hold it.
 */
size_t qca6174_build_board_key(char *buf, size_t buflen,
                               uint16_t vendor, uint16_t device,
                               uint16_t subsys_vendor, uint16_t subsys_device);

#ifdef __cplusplus
}
#endif

#endif /* QCA6174_HW_PARAMS_H */
