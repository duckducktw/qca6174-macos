/* Print the Apple80211 ref bytes 0x40..0x70 before/after bind and after a
   CARD_CAPABILITIES get, to see whether the capability reply is mirrored there
   (Associate2 tests bit 3 of ref+0x59). */
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
typedef int (*get_t)(void *, int, int, void *, unsigned long);

static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }

static void dump(void *ref, const char *tag)
{
    unsigned char *p = (unsigned char *)ref;
    printf("%-14s ref+0x40:", tag);
    for (int i = 0x40; i < 0x70; i++) printf(" %02x", p[i]);
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
    void *ref = 0;
    if (O(&ref) || !ref) { printf("open failed\n"); return 1; }
    dump(ref, "after open");
    if (B(ref, S("en1"))) { printf("bind failed\n"); return 1; }
    dump(ref, "after bind");
    unsigned char caps[128];
    memset(caps, 0, sizeof caps);
    int rc = G(ref, 12 /* CARD_CAPABILITIES */, 0, caps, sizeof caps);
    printf("get12 rc=%d caps:", rc);
    for (int i = 0; i < 16; i++) printf(" %02x", caps[i]);
    printf("\n");
    dump(ref, "after caps");
    return 0;
}
