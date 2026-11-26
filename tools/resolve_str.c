/* Resolve the __cfstring / selref targets of rip-relative loads in a slice of
   otool -tV output.  Input on stdin: "<insn_va_hex> <disp_hex>" pairs.

   Both the movq and leaq rip-relative forms used here have a 7-byte encoding, so
   target = insn_va + 7 + disp.  At the target sits a pointer into __DATA; that
   pointer is either a __CFConstantString object (isa/flags/cstr/len) or a
   selector reference (a bare C string pointer). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static unsigned char *g;      /* start of the selected mach-o slice */
static size_t gSize;
static size_t gSliceOff;      /* slice offset inside the file */

static int va2off(uint64_t va, size_t *out)
{
    const uint32_t magic = *(uint32_t *)g;
    if (magic != 0xfeedfacf) { fprintf(stderr, "not 64-bit mach-o: %x\n", magic); return 0; }
    /* winning: nothing to do for thin slices */
    const uint32_t ncmds = *(uint32_t *)(g + 16);
    const unsigned char *lc = g + 32;
    for (uint32_t i = 0; i < ncmds; i++) {
        const uint32_t cmd = *(uint32_t *)lc;
        const uint32_t cmdsize = *(uint32_t *)(lc + 4);
        if (cmd == 0x19) { /* LC_SEGMENT_64 */
            const uint64_t vmaddr = *(uint64_t *)(lc + 24);
            const uint64_t vmsize = *(uint64_t *)(lc + 32);
            const uint64_t fileoff = *(uint64_t *)(lc + 40);
            if (va >= vmaddr && va < vmaddr + vmsize) {
                *out = gSliceOff + (size_t)(fileoff + (va - vmaddr));
                return 1;
            }
        }
        lc += cmdsize;
    }
    return 0;
}

/* Accept only a plausible C string: printable ASCII, terminated within 200 bytes. */
static int printable(uint64_t va)
{
    size_t off;
    if (!va2off(va, &off) || off >= gSize) return 0;
    for (size_t i = 0; i < 200 && off + i < gSize; i++) {
        const unsigned char c = g[off + i];
        if (c == 0) return i > 0;
        if (c < 0x20 || c > 0x7e) return 0;
    }
    return 0;
}

/* __DATA pointers in this binary are DYLD_CHAINED_PTR_64: low 36 bits hold the
   target offset from the image base, the high bits hold the chain link. Reading
   them as plain VAs is what made every lookup come back as "?". */
static uint64_t decode_ptr(uint64_t p)
{
    if (p == 0) return 0;
    if (p & 0x8000000000000000ULL) return 0;         /* bind, not a rebase */
    return 0x100000000ULL + (p & 0xFFFFFFFFFULL);
}

static uint64_t rd64(uint64_t va)
{
    size_t off;
    if (!va2off(va, &off) || off + 8 > gSize) return 0;
    return *(uint64_t *)&g[off];
}

static const char *cstr(uint64_t va)
{
    size_t off;
    if (!va2off(va, &off) || off >= gSize) return "?";
    const char *s = (const char *)&g[off];
    if (!memchr(s, 0, gSize - off > 256 ? 256 : gSize - off)) return "?";
    return s;
}

int main(void)
{
    FILE *f = fopen("/usr/libexec/airportd", "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END);
    gSize = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    g = malloc(gSize);
    if (fread(g, 1, gSize, f) != gSize) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);
    /* Universal binary: pick the x86_64 slice and shift every lookup by its offset. */
    {
        const uint32_t be = *(uint32_t *)g;
        if (be == 0xcafebabe || be == 0xbebafeca) {
            const uint32_t n = __builtin_bswap32(*(uint32_t *)(g + 4));
            for (uint32_t i = 0; i < n; i++) {
                const unsigned char *a = g + 8 + i * 20;
                const uint32_t cputype = __builtin_bswap32(*(uint32_t *)a);
                const uint32_t off = __builtin_bswap32(*(uint32_t *)(a + 8));
                if (cputype == 0x01000007) {   /* CPU_TYPE_X86_64 */
                    gSliceOff = off;
                    g += off;
                    gSize -= off;
                    break;
                }
            }
        }
    }

    char a[64], b[64];
    while (scanf("%63s %63s", a, b) == 2) {
        const uint64_t va = strtoull(a, NULL, 16);
        const uint64_t disp = strtoull(b, NULL, 16);
        const uint64_t target = va + 7 + disp;
        const uint64_t p = decode_ptr(rd64(target));
        const char *res = "?";
        if (p) {
            if (printable(p)) {
                res = cstr(p);                 // selector reference: chars inline
            } else {
                const uint64_t c = decode_ptr(rd64(p + 16));   // __CFConstantString.cstr
                if (c && printable(c)) res = cstr(c);
            }
        }
        printf("%llx -> %llx  %s\n", (unsigned long long)va, (unsigned long long)target, res);
    }
    return 0;
}
