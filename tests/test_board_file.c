/* SPDX-License-Identifier: ISC */
#include "test.h"
#include "tbuild.h"
#include "suites.h"

#include "../src/common/board_file.h"
#include "../src/common/ath10k_abi.h"

static void put_board_magic(struct tbuf *t)
{
    tb_str(t, ATH10K_BOARD_MAGIC);
    tb_u8(t, 0);     /* NUL */
    tb_pad4(t);      /* -> 20 bytes */
}

/* Build a nested BOARD element body with N names then one data blob. */
static void tb_board_elem(struct tbuf *outer, uint32_t top_id,
                          const char **names, unsigned n_names,
                          const void *data, size_t data_len)
{
    struct tbuf inner; tb_reset(&inner);
    for (unsigned i = 0; i < n_names; i++)
        tb_ie(&inner, ATH10K_BD_IE_BOARD_NAME, names[i], strlen(names[i]));
    tb_ie(&inner, ATH10K_BD_IE_BOARD_DATA, data, data_len);
    tb_ie(outer, top_id, inner.b, inner.len);
}

struct count_ctx { unsigned n; };
static void count_cb(const struct ath10k_board_entry *e, void *c)
{
    (void)e;
    ((struct count_ctx *)c)->n++;
}

static void test_single_board(void)
{
    struct tbuf t; tb_reset(&t);
    put_board_magic(&t);
    const char *names[] = { "bus=pci,vendor=168c,device=003e" };
    uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    tb_board_elem(&t, ATH10K_BD_IE_BOARD, names, 1, data, sizeof(data));

    struct ath10k_board_summary s;
    struct count_ctx c = { 0 };
    int r = ath10k_board_file_walk(t.b, t.len, count_cb, &c, &s);
    CHECK_EQ_U(r, ATH10K_PARSE_OK);
    CHECK_EQ_U(s.board_elems, 1u);
    CHECK_EQ_U(s.names, 1u);
    CHECK_EQ_U(s.data_blocks, 1u);
    CHECK_EQ_U(s.entries, 1u);
    CHECK_EQ_U(c.n, 1u);
}

static void test_aliases(void)
{
    /* Two names sharing one data blob -> two entries. */
    struct tbuf t; tb_reset(&t);
    put_board_magic(&t);
    const char *names[] = { "name-a", "name-b" };
    uint8_t data[4] = { 9, 9, 9, 9 };
    tb_board_elem(&t, ATH10K_BD_IE_BOARD, names, 2, data, sizeof(data));

    struct ath10k_board_summary s;
    int r = ath10k_board_file_walk(t.b, t.len, NULL, NULL, &s);
    CHECK_EQ_U(r, ATH10K_PARSE_OK);
    CHECK_EQ_U(s.names, 2u);
    CHECK_EQ_U(s.data_blocks, 1u);
    CHECK_EQ_U(s.entries, 2u);
}

static void test_find(void)
{
    struct tbuf t; tb_reset(&t);
    put_board_magic(&t);
    const char *na[] = { "alpha" };
    uint8_t da[3] = { 0xA, 0xA, 0xA };
    tb_board_elem(&t, ATH10K_BD_IE_BOARD, na, 1, da, sizeof(da));
    const char *nb[] = { "beta" };
    uint8_t db[5] = { 0xB, 0xB, 0xB, 0xB, 0xB };
    tb_board_elem(&t, ATH10K_BD_IE_BOARD, nb, 1, db, sizeof(db));

    struct ath10k_board_entry e;
    int r = ath10k_board_file_find(t.b, t.len, "beta", 4, &e);
    CHECK_EQ_U(r, ATH10K_PARSE_OK);
    CHECK_EQ_U(e.data_len, 5u);
    CHECK(e.data && e.data[0] == 0xB);

    /* Miss: clean parse, NULL data. */
    r = ath10k_board_file_find(t.b, t.len, "gamma", 5, &e);
    CHECK_EQ_U(r, ATH10K_PARSE_OK);
    CHECK(e.data == NULL);
}

static void test_board_ext(void)
{
    struct tbuf t; tb_reset(&t);
    put_board_magic(&t);
    const char *n[] = { "ext-board" };
    uint8_t d[2] = { 7, 7 };
    tb_board_elem(&t, ATH10K_BD_IE_BOARD_EXT, n, 1, d, sizeof(d));

    struct ath10k_board_entry e;
    int r = ath10k_board_file_find(t.b, t.len, "ext-board", 9, &e);
    CHECK_EQ_U(r, ATH10K_PARSE_OK);
    CHECK(e.ext);
    CHECK_EQ_U(e.data_len, 2u);
}

static void test_bad_magic(void)
{
    struct tbuf t; tb_reset(&t);
    tb_str(&t, "QCA-WRONG-MAGIC!"); tb_u8(&t, 0); tb_pad4(&t);
    struct ath10k_board_summary s;
    CHECK_EQ_U(ath10k_board_file_walk(t.b, t.len, NULL, NULL, &s),
               ATH10K_PARSE_E_MAGIC);

    tb_reset(&t);
    tb_str(&t, ATH10K_BOARD_MAGIC);
    tb_u8(&t, 'X'); tb_pad4(&t); /* magic bytes without required NUL */
    CHECK_EQ_U(ath10k_board_file_walk(t.b, t.len, NULL, NULL, &s),
               ATH10K_PARSE_E_MAGIC);
}

static void test_truncated_nested(void)
{
    /* Nested sub-IE claims more than the parent body holds. */
    struct tbuf t; tb_reset(&t);
    put_board_magic(&t);
    struct tbuf inner; tb_reset(&inner);
    tb_le32(&inner, ATH10K_BD_IE_BOARD_DATA);
    tb_le32(&inner, 0xffffu);  /* lies */
    tb_u8(&inner, 1);
    tb_ie(&t, ATH10K_BD_IE_BOARD, inner.b, inner.len);
    struct ath10k_board_summary s;
    CHECK_EQ_U(ath10k_board_file_walk(t.b, t.len, NULL, NULL, &s),
               ATH10K_PARSE_E_TRUNC);
}

static void test_too_short(void)
{
    struct ath10k_board_summary s;
    uint8_t x[8] = {0};
    CHECK_EQ_U(ath10k_board_file_walk(x, sizeof(x), NULL, NULL, &s),
               ATH10K_PARSE_E_SHORT);
}

void suite_board_file(void)
{
    RUN(test_single_board);
    RUN(test_aliases);
    RUN(test_find);
    RUN(test_board_ext);
    RUN(test_bad_magic);
    RUN(test_truncated_nested);
    RUN(test_too_short);
}
