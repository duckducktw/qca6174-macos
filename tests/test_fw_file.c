/* SPDX-License-Identifier: ISC */
#include "test.h"
#include "tbuild.h"
#include "suites.h"

#include "../src/common/fw_file.h"
#include "../src/common/ath10k_abi.h"

/* Emit the firmware magic header (magic + NUL, padded to 4). */
static void put_fw_magic(struct tbuf *t)
{
    tb_str(t, ATH10K_FW_MAGIC);
    tb_u8(t, 0);     /* NUL */
    tb_pad4(t);      /* -> 12 bytes */
}

static void test_valid_minimal(void)
{
    struct tbuf t; tb_reset(&t);
    put_fw_magic(&t);
    tb_ie(&t, ATH10K_FW_IE_FW_VERSION, "4.4.1", 5);
    uint8_t ts[4] = { 0x44, 0x33, 0x22, 0x11 };
    tb_ie(&t, ATH10K_FW_IE_TIMESTAMP, ts, 4);
    uint8_t img[2] = { 0xaa, 0xbb };
    tb_ie(&t, ATH10K_FW_IE_FW_IMAGE, img, 2);
    uint8_t wmi[4] = { 3, 0, 0, 0 };
    tb_ie(&t, ATH10K_FW_IE_WMI_OP_VERSION, wmi, 4);
    uint8_t unk[1] = { 0x7 };
    tb_ie(&t, 99, unk, 1); /* unknown id -> skipped, counted */

    struct ath10k_fw_file f;
    int r = ath10k_fw_file_parse(t.b, t.len, &f);
    CHECK_EQ_U(r, ATH10K_PARSE_OK);
    CHECK_STR_EQ(f.version, f.version_len, "4.4.1");
    CHECK(f.has_timestamp);
    CHECK_EQ_U(f.timestamp, 0x11223344u);
    CHECK_EQ_U(f.fw_image_len, 2u);
    CHECK(f.fw_image && f.fw_image[0] == 0xaa && f.fw_image[1] == 0xbb);
    CHECK(f.has_wmi_op_version);
    CHECK_EQ_U(f.wmi_op_version, 3u);
    CHECK_EQ_U(f.n_ies, 5u);
    CHECK_EQ_U(f.n_unknown_ies, 1u);
}

static void test_bad_magic(void)
{
    struct tbuf t; tb_reset(&t);
    tb_str(&t, "NOT-ATH10K");
    tb_u8(&t, 0); tb_pad4(&t);
    tb_ie(&t, ATH10K_FW_IE_FW_VERSION, "x", 1);
    struct ath10k_fw_file f;
    CHECK_EQ_U(ath10k_fw_file_parse(t.b, t.len, &f), ATH10K_PARSE_E_MAGIC);

    tb_reset(&t);
    tb_str(&t, ATH10K_FW_MAGIC);
    tb_u8(&t, 'X'); tb_pad4(&t); /* magic bytes without required NUL */
    CHECK_EQ_U(ath10k_fw_file_parse(t.b, t.len, &f), ATH10K_PARSE_E_MAGIC);
}

static void test_too_short(void)
{
    struct ath10k_fw_file f;
    uint8_t small[4] = {0};
    CHECK_EQ_U(ath10k_fw_file_parse(small, sizeof(small), &f),
               ATH10K_PARSE_E_SHORT);
}

static void test_null(void)
{
    struct ath10k_fw_file f;
    CHECK_EQ_U(ath10k_fw_file_parse(NULL, 100, &f), ATH10K_PARSE_E_NULL);
    CHECK_EQ_U(ath10k_fw_file_parse((const uint8_t *)"x", 1, NULL),
               ATH10K_PARSE_E_NULL);
}

static void test_truncated_ie(void)
{
    /* IE claims a length that runs past the buffer. */
    struct tbuf t; tb_reset(&t);
    put_fw_magic(&t);
    tb_le32(&t, ATH10K_FW_IE_FW_IMAGE);
    tb_le32(&t, 0xffffffu);   /* huge len */
    tb_u8(&t, 0x1);           /* only 1 byte of body present */
    struct ath10k_fw_file f;
    CHECK_EQ_U(ath10k_fw_file_parse(t.b, t.len, &f), ATH10K_PARSE_E_TRUNC);
    /* output must be zeroed on error */
    CHECK(f.version == NULL && f.n_ies == 0);
}

static void test_dangling_tail(void)
{
    /* 3 stray bytes after a valid IE cannot form an IE header. */
    struct tbuf t; tb_reset(&t);
    put_fw_magic(&t);
    tb_ie(&t, ATH10K_FW_IE_FW_VERSION, "v", 1);
    tb_u8(&t, 1); tb_u8(&t, 2); tb_u8(&t, 3);
    struct ath10k_fw_file f;
    CHECK_EQ_U(ath10k_fw_file_parse(t.b, t.len, &f), ATH10K_PARSE_E_TRUNC);
}

static void test_bad_fixed_len(void)
{
    /* TIMESTAMP must be exactly 4 bytes. */
    struct tbuf t; tb_reset(&t);
    put_fw_magic(&t);
    uint8_t ts[2] = { 1, 2 };
    tb_ie(&t, ATH10K_FW_IE_TIMESTAMP, ts, 2);
    struct ath10k_fw_file f;
    CHECK_EQ_U(ath10k_fw_file_parse(t.b, t.len, &f), ATH10K_PARSE_E_BADLEN);
}

static void test_zero_len_ie(void)
{
    /* A zero-length IE is legal and must be skipped cleanly. */
    struct tbuf t; tb_reset(&t);
    put_fw_magic(&t);
    tb_ie(&t, ATH10K_FW_IE_FEATURES, "", 0);
    tb_ie(&t, ATH10K_FW_IE_FW_VERSION, "ok", 2);
    struct ath10k_fw_file f;
    CHECK_EQ_U(ath10k_fw_file_parse(t.b, t.len, &f), ATH10K_PARSE_OK);
    CHECK_EQ_U(f.features_len, 0u);
    CHECK(f.features != NULL); /* present, empty */
    CHECK_STR_EQ(f.version, f.version_len, "ok");
}

static void test_empty_container(void)
{
    /* Just the magic header, no IEs: valid, nothing parsed. */
    struct tbuf t; tb_reset(&t);
    put_fw_magic(&t);
    struct ath10k_fw_file f;
    CHECK_EQ_U(ath10k_fw_file_parse(t.b, t.len, &f), ATH10K_PARSE_OK);
    CHECK_EQ_U(f.n_ies, 0u);
}

void suite_fw_file(void)
{
    RUN(test_valid_minimal);
    RUN(test_bad_magic);
    RUN(test_too_short);
    RUN(test_null);
    RUN(test_truncated_ie);
    RUN(test_dangling_tail);
    RUN(test_bad_fixed_len);
    RUN(test_zero_len_ie);
    RUN(test_empty_container);
}
