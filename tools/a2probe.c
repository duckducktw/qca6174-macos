// Dump ref+0x48..0x68 after Open, after Bind, and after every getter that the
// ioctl fallback allows, to find which request (if any) makes the framework fill
// the private driver-flags qword at ref+0x50 (bit 0x40 of +0x51 is what
// Apple80211Associate2 gates the join on).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
typedef int (*get_t)(void *, int, int, void *, unsigned long);

static void dump(const char *tag, void *ref)
{
    unsigned char *b = (unsigned char *)ref;
    printf("%-14s", tag);
    for (int i = 0x48; i <= 0x60; ++i)
        printf(" %02x", b[i]);
    printf("   +51&40=%d\n", (b[0x51] & 0x40) ? 1 : 0);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const int types[] = { 12, 43, 44, 9, 27, 97, 103, 13, 15, 21, 50, 96 };
    const int ntypes = (int)(sizeof(types) / sizeof(types[0]));
    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 2; }
    open_t O = (open_t)dlsym(h, "Apple80211Open");
    bind_t B = (bind_t)dlsym(h, "Apple80211BindToInterface");
    get_t  G = (get_t)dlsym(h, "Apple80211Get");
    void *ref = 0;
    if (O(&ref) || !ref) { printf("open failed\n"); return 3; }
    dump("open", ref);
    CFStringRef name = CFStringCreateWithCString(NULL, "en1", kCFStringEncodingUTF8);
    int brc = B(ref, name);
    printf("bind rc=%d\n", brc);
    dump("after-bind", ref);
    for (int i = 0; i < ntypes; ++i) {
        unsigned char buf[1024];
        memset(buf, 0, sizeof buf);
        int rc = G(ref, types[i], 0, buf, sizeof buf);
        char tag[32];
        snprintf(tag, sizeof tag, "get%d(%d)", types[i], rc);
        dump(tag, ref);
    }
    return 0;
}
