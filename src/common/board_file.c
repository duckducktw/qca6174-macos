/* SPDX-License-Identifier: ISC */
#include "board_file.h"
#include "ath10k_abi.h"

#include <string.h> /* memcmp */

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/*
 * Generic single-level IE stepper. On entry pp/prem describe the remaining
 * buffer. On success, writes the IE id/body/body_len and advances pp/prem
 * past the IE (including 4-byte padding, clamped for a final unpadded IE).
 * Returns ATH10K_PARSE_OK, ATH10K_PARSE_E_TRUNC, or 1 to signal "clean end"
 * (no bytes left). A dangling 1..7 byte tail is reported as TRUNC.
 */
static int next_ie(const uint8_t **pp, size_t *prem,
                   uint32_t *id, const uint8_t **body, uint32_t *body_len)
{
    size_t remaining = *prem;
    if (remaining == 0)
        return 1; /* clean end */
    if (remaining < 8)
        return ATH10K_PARSE_E_TRUNC;

    const uint8_t *p = *pp;
    uint32_t ie_id  = rd_le32(p);
    uint32_t ie_len = rd_le32(p + 4);

    if (ie_len > remaining - 8)
        return ATH10K_PARSE_E_TRUNC;

    *id = ie_id;
    *body = p + 8;
    *body_len = ie_len;

    size_t step = 8u + (size_t)ie_len;
    p += step;
    remaining -= step;

    size_t pad = ath10k_align4((size_t)ie_len) - (size_t)ie_len; /* 0..3 */
    if (pad > remaining)
        pad = remaining;
    p += pad;
    remaining -= pad;

    *pp = p;
    *prem = remaining;
    return ATH10K_PARSE_OK;
}

/* Walk the sub-elements of one board element body. */
static int walk_board_body(const uint8_t *body, size_t body_len, bool ext,
                           ath10k_board_entry_cb cb, void *ctx,
                           struct ath10k_board_summary *s)
{
    const uint8_t *names[ATH10K_MAX_BOARD_NAMES];
    size_t name_lens[ATH10K_MAX_BOARD_NAMES];
    unsigned n_names = 0;

    const uint8_t *p = body;
    size_t remaining = body_len;

    for (;;) {
        uint32_t id, len;
        const uint8_t *sub;
        int r = next_ie(&p, &remaining, &id, &sub, &len);
        if (r == 1)
            break;
        if (r != ATH10K_PARSE_OK)
            return r;

        switch (id) {
        case ATH10K_BD_IE_BOARD_NAME:
            if (s) s->names++;
            if (n_names < ATH10K_MAX_BOARD_NAMES) {
                names[n_names] = sub;
                name_lens[n_names] = len;
                n_names++;
            } else if (s) {
                s->names_overflow++;
            }
            break;
        case ATH10K_BD_IE_BOARD_DATA:
            if (s) s->data_blocks++;
            /* Emit one entry per buffered name, then reset the name set so a
             * subsequent name/data run in the same element is independent. */
            for (unsigned i = 0; i < n_names; i++) {
                if (s) s->entries++;
                if (cb) {
                    struct ath10k_board_entry e = {
                        .name = names[i], .name_len = name_lens[i],
                        .data = sub,      .data_len = len,
                        .ext = ext,
                    };
                    cb(&e, ctx);
                }
            }
            n_names = 0;
            break;
        default:
            if (s) s->unknown_sub_ies++;
            break;
        }
    }
    return ATH10K_PARSE_OK;
}

int ath10k_board_file_walk(const uint8_t *data, size_t len,
                           ath10k_board_entry_cb cb, void *ctx,
                           struct ath10k_board_summary *summary)
{
    struct ath10k_board_summary local;
    struct ath10k_board_summary *s = summary ? summary : &local;
    memset(s, 0, sizeof(*s));

    if (!data)
        return ATH10K_PARSE_E_NULL;

    const size_t magic_hdr = ath10k_align4(ATH10K_BOARD_MAGIC_LEN + 1u); /* = 20 */
    if (len < magic_hdr)
        return ATH10K_PARSE_E_SHORT;
    if (memcmp(data, ATH10K_BOARD_MAGIC, ATH10K_BOARD_MAGIC_LEN) != 0 ||
        data[ATH10K_BOARD_MAGIC_LEN] != '\0')
        return ATH10K_PARSE_E_MAGIC;

    const uint8_t *p = data + magic_hdr;
    size_t remaining = len - magic_hdr;

    for (;;) {
        uint32_t id, blen;
        const uint8_t *body;
        int r = next_ie(&p, &remaining, &id, &body, &blen);
        if (r == 1)
            break;
        if (r != ATH10K_PARSE_OK) {
            memset(s, 0, sizeof(*s));
            return r;
        }

        if (id == ATH10K_BD_IE_BOARD || id == ATH10K_BD_IE_BOARD_EXT) {
            s->board_elems++;
            int rr = walk_board_body(body, blen,
                                     id == ATH10K_BD_IE_BOARD_EXT,
                                     cb, ctx, s);
            if (rr != ATH10K_PARSE_OK) {
                memset(s, 0, sizeof(*s));
                return rr;
            }
        } else {
            s->unknown_top_ies++;
        }
    }
    return ATH10K_PARSE_OK;
}

struct find_ctx {
    const char *key;
    size_t key_len;
    struct ath10k_board_entry *found;
    bool done;
};

static void find_cb(const struct ath10k_board_entry *e, void *ctxp)
{
    struct find_ctx *c = (struct find_ctx *)ctxp;
    if (c->done)
        return;
    if (e->name_len == c->key_len &&
        memcmp(e->name, c->key, c->key_len) == 0) {
        *c->found = *e;
        c->done = true;
    }
}

int ath10k_board_file_find(const uint8_t *data, size_t len,
                           const char *key, size_t key_len,
                           struct ath10k_board_entry *found)
{
    if (!found || !key)
        return ATH10K_PARSE_E_NULL;

    memset(found, 0, sizeof(*found));
    struct find_ctx c = { .key = key, .key_len = key_len,
                          .found = found, .done = false };

    int r = ath10k_board_file_walk(data, len, find_cb, &c, NULL);
    if (r != ATH10K_PARSE_OK) {
        memset(found, 0, sizeof(*found));
        return r;
    }
    if (!c.done)
        memset(found, 0, sizeof(*found)); /* clean parse, no match */
    return ATH10K_PARSE_OK;
}
