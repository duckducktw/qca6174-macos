/* SPDX-License-Identifier: ISC */
/*
 * Integration probe: run the real parsers against the real linux-firmware
 * QCA6174 hw3.0 images (decompressed by scripts/prepare_fixtures.sh) and print
 * what was actually found. Exit non-zero if a file is present but fails to
 * parse. Missing files are reported and skipped (not a hard failure) so the
 * suite still runs on hosts without the fixtures.
 *
 * Usage: fixture_probe <fixtures-dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/common/fw_file.h"
#include "../src/common/board_file.h"
#include "../src/common/hw_params.h"
#include "../src/common/qca6174_regs.h"

static int read_file(const char *path, uint8_t **buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    if (!b) { fclose(f); return -1; }
    size_t rd = fread(b, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(b); return -1; }
    *buf = b;
    *len = (size_t)sz;
    return 0;
}

static void print_ascii(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int c = p[i];
        putchar((c >= 0x20 && c < 0x7f) ? c : '.');
    }
}

static int probe_firmware(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/firmware-6.bin", dir);
    uint8_t *buf = NULL; size_t len = 0;
    if (read_file(path, &buf, &len) != 0) {
        printf("[skip] %s not present\n", path);
        return 0;
    }
    printf("[fw ] %s (%zu bytes)\n", path, len);

    struct ath10k_fw_file f;
    int r = ath10k_fw_file_parse(buf, len, &f);
    if (r != ATH10K_PARSE_OK) {
        printf("      PARSE FAILED: %s\n", ath10k_parse_strerror(r));
        free(buf);
        return 1;
    }
    printf("      version   : \"");
    print_ascii(f.version, f.version_len);
    printf("\" (%zu bytes)\n", f.version_len);
    if (f.has_timestamp)
        printf("      timestamp : 0x%08x\n", f.timestamp);
    if (f.has_wmi_op_version)
        printf("      wmi op ver: %u\n", f.wmi_op_version);
    if (f.has_htt_op_version)
        printf("      htt op ver: %u\n", f.htt_op_version);
    printf("      fw image  : %zu bytes\n", f.fw_image_len);
    printf("      otp image : %zu bytes\n", f.otp_image_len);
    printf("      codeswap  : %zu bytes\n", f.codeswap_len);
    printf("      features  : %zu bytes\n", f.features_len);
    printf("      total IEs : %u (unknown: %u)\n", f.n_ies, f.n_unknown_ies);
    free(buf);
    return 0;
}

struct name_printer { unsigned shown; unsigned limit; };
static void name_cb(const struct ath10k_board_entry *e, void *ctx)
{
    struct name_printer *np = (struct name_printer *)ctx;
    if (np->shown < np->limit) {
        printf("        [%s] ", e->ext ? "ext" : "brd");
        print_ascii(e->name, e->name_len);
        printf("  (data %zu bytes)\n", e->data_len);
    }
    np->shown++;
}

static int probe_board(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/board-2.bin", dir);
    uint8_t *buf = NULL; size_t len = 0;
    if (read_file(path, &buf, &len) != 0) {
        printf("[skip] %s not present\n", path);
        return 0;
    }
    printf("[brd] %s (%zu bytes)\n", path, len);

    struct ath10k_board_summary s;
    struct name_printer np = { 0, 12 };
    int r = ath10k_board_file_walk(buf, len, name_cb, &np, &s);
    if (r != ATH10K_PARSE_OK) {
        printf("      PARSE FAILED: %s\n", ath10k_parse_strerror(r));
        free(buf);
        return 1;
    }
    printf("      board elems: %u, names: %u, data blocks: %u, entries: %u\n",
           s.board_elems, s.names, s.data_blocks, s.entries);
    printf("      first %u name(s):\n", np.limit);
    /* (names already printed by callback above) */
    if (np.shown > np.limit)
        printf("        ... and %u more\n", np.shown - np.limit);

    /* Real lookup for the target device. */
    char key[128];
    size_t kl = qca6174_build_board_key(key, sizeof(key),
                                        QCA6174_PCI_VENDOR_ID,
                                        QCA6174_PCI_DEVICE_ID,
                                        QCA6174_SUBSYS_VENDOR_ID,
                                        QCA6174_SUBSYS_DEVICE_ID);
    struct ath10k_board_entry e;
    r = ath10k_board_file_find(buf, len, key, kl, &e);
    printf("      lookup full key: %s\n", key);
    printf("         -> %s\n", (r == ATH10K_PARSE_OK && e.data)
           ? "MATCH" : "no exact match (see notes)");

    /* Fallback generic key (vendor/device only), which QCA6174 board-2 often
     * carries as the default profile. */
    char gkey[64];
    int gl = snprintf(gkey, sizeof(gkey),
                      "bus=pci,vendor=%04x,device=%04x",
                      QCA6174_PCI_VENDOR_ID, QCA6174_PCI_DEVICE_ID);
    r = ath10k_board_file_find(buf, len, gkey, (size_t)gl, &e);
    printf("      lookup generic : %s -> %s\n", gkey,
           (r == ATH10K_PARSE_OK && e.data) ? "MATCH" : "no exact match");

    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "build/fixtures";
    printf("== QCA6174 fixture probe (dir=%s) ==\n", dir);
    int rc = 0;
    rc |= probe_firmware(dir);
    rc |= probe_board(dir);
    printf("== fixture probe %s ==\n", rc ? "FAILED" : "done");
    return rc;
}
