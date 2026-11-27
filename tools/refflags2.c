// What fills apple80211_ref+0x51 bit 6? Dump the ref, then try the entry points
// airportd uses before the join: Apple80211Set(ref, 0x1a6, 0, &v, 4) and a
// CARD_CAPABILITIES get, printing the ref flags each time.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
typedef int (*get_t)(void *, int, int, void *, unsigned long);
typedef int (*set_t)(void *, int, int, void *, unsigned long);

static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }

static void dump(void *ref, const char *tag)
{
    unsigned char *p = (unsigned char *)ref;
    printf("%-16s 0x50: %02x %02x %02x %02x 0x54:", tag, p[0x50], p[0x51], p[0x52], p[0x53]);
    for (int i = 0x54; i < 0x64; i++) printf(" %02x", p[i]);
    printf("\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    open_t O = (open_t)dlsym(h, "Apple80211Open");
    bind_t B = (bind_t)dlsym(h, "Apple80211BindToInterface");
    get_t G = (get_t)dlsym(h, "Apple80211Get");
    set_t SE = (set_t)dlsym(h, "Apple80211Set");

    void *ref = 0;
    if (O(&ref) || !ref) { printf("open failed\n"); return 1; }
    dump(ref, "after open");
    if (B(ref, S("en1"))) { printf("bind failed\n"); return 1; }
    dump(ref, "after bind");

    unsigned char caps[128]; memset(caps, 0, sizeof caps);
    int rc = G(ref, 12, 0, caps, sizeof caps);
    printf("get12 rc=%d caps:", rc);
    for (int i = 0; i < 16; i++) printf(" %02x", caps[i]);
    printf("\n");
    dump(ref, "after get12");

    int v = 1;
    rc = SE(ref, 0x1a6, 0, &v, 4);
    printf("set 0x1a6 rc=%d\n", rc);
    dump(ref, "after set1a6");

    // assoc-related gets airportd walks before joining
    int types[] = {19, 15, 1, 2, 0, 13, 50};
    for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++) {
        unsigned char buf[256]; memset(buf, 0, sizeof buf);
        int r = G(ref, types[i], 0, buf, sizeof buf);
        printf("get %2d rc=%d first=%02x %02x\n", types[i], r, buf[0], buf[1]);
    }
    dump(ref, "after gets");
    return 0;
}
