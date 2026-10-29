/* SPDX-License-Identifier: ISC */
/* Tiny in-memory byte-buffer builder for crafting container fixtures. */
#ifndef QCA6174_TBUILD_H
#define QCA6174_TBUILD_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

struct tbuf {
    uint8_t  b[8192];
    size_t   len;
};

static inline void tb_reset(struct tbuf *t) { t->len = 0; }

static inline void tb_u8(struct tbuf *t, uint8_t v) { t->b[t->len++] = v; }

static inline void tb_le32(struct tbuf *t, uint32_t v)
{
    t->b[t->len++] = (uint8_t)(v & 0xff);
    t->b[t->len++] = (uint8_t)((v >> 8) & 0xff);
    t->b[t->len++] = (uint8_t)((v >> 16) & 0xff);
    t->b[t->len++] = (uint8_t)((v >> 24) & 0xff);
}

static inline void tb_bytes(struct tbuf *t, const void *p, size_t n)
{
    memcpy(t->b + t->len, p, n);
    t->len += n;
}

static inline void tb_str(struct tbuf *t, const char *s)
{
    tb_bytes(t, s, strlen(s));
}

static inline void tb_pad4(struct tbuf *t)
{
    while (t->len & 3u)
        t->b[t->len++] = 0;
}

/* Append a TLV information element: le32 id, le32 len, body, pad to 4. */
static inline void tb_ie(struct tbuf *t, uint32_t id, const void *body, size_t n)
{
    tb_le32(t, id);
    tb_le32(t, (uint32_t)n);
    tb_bytes(t, body, n);
    tb_pad4(t);
}

#endif /* QCA6174_TBUILD_H */
