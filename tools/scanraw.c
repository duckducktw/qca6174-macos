/* Ask the framework for scan results exactly like airportd does (Apple80211Get, type
   11) and hexdump the first bytes of each record, so the offset the *client* sees for
   BSSID/SSID/channel is measured instead of assumed. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
typedef int (*get_t)(void *, int, int, void *, unsigned long);

static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    open_t O = (open_t)dlsym(h, "Apple80211Open");
    bind_t B = (bind_t)dlsym(h, "Apple80211BindToInterface");
    get_t G = (get_t)dlsym(h, "Apple80211Get");
    void *ref = 0;
    if (O(&ref) || !ref) { printf("open failed\n"); return 1; }
    if (B(ref, S("en1"))) { printf("bind failed\n"); return 1; }

    static unsigned char buf[0x800];
    for (int i = 0; i < 3; i++) {
        memset(buf, 0, sizeof buf);
        int rc = G(ref, 11, 0, buf, sizeof buf);
        printf("--- record %d rc=%d (0x%08x)\n", i, rc, (unsigned)rc);
        if (rc != 0)
            break;
        for (int off = 0; off < 0x70; off += 16) {
            printf("  %03x:", off);
            for (int k = 0; k < 16; k++) printf(" %02x", buf[off + k]);
            printf("   |");
            for (int k = 0; k < 16; k++) {
                unsigned char c = buf[off + k];
                putchar(c >= 0x20 && c < 0x7f ? c : '.');
            }
            printf("|\n");
        }
        // Where does the target MAC (f0:68:e3:2d:40:22) or an SSID string show up?
        const unsigned char mac[6] = {0xf0, 0x68, 0xe3, 0x2d, 0x40, 0x22};
        for (int off = 0; off + 6 < 0x120; off++)
            if (memcmp(buf + off, mac, 6) == 0) printf("  target MAC at offset 0x%x\n", off);
        for (int off = 0; off + 8 < 0x120; off++)
            if (memcmp(buf + off, "QCA-TEST", 8) == 0) printf("  SSID string at offset 0x%x\n", off);
    }
    return 0;
}
