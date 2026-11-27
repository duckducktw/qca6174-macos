// Try the service-flavoured bind that airportd uses, from an ordinary process,
// and see whether it fills the private driver-flags qword at ref+0x50
// (bit 0x40 of +0x51 is the Apple80211Associate2 join gate).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind1_t)(void *, CFStringRef);
typedef int (*bind2_t)(void *, CFStringRef, CFStringRef);

static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }

static void dump(const char *tag, void *ref)
{
    unsigned char *b = (unsigned char *)ref;
    printf("%-30s +50..53=%02x %02x %02x %02x  bit40=%d\n", tag,
           b[0x50], b[0x51], b[0x52], b[0x53], (b[0x51] & 0x40) ? 1 : 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 2; }
    open_t O = (open_t)dlsym(h, "Apple80211Open");
    bind1_t B1 = (bind1_t)dlsym(h, "Apple80211BindToInterface");
    bind2_t B2 = (bind2_t)dlsym(h, "Apple80211BindToInterfaceWithService");
    printf("symbols open=%p bind=%p bindsvc=%p\n", (void *)O, (void *)B1, (void *)B2);

    struct { const char *a, *b; } combos[] = {
        { "en1", "Wi-Fi" }, { "Wi-Fi", "en1" }, { "en1", "en1" }, { "en1", "AirPort" },
    };

    for (int i = 0; i < (int)(sizeof(combos) / sizeof(combos[0])); ++i) {
        void *ref = 0;
        if (O(&ref) || !ref) { printf("open failed\n"); continue; }
        char tag[80];
        snprintf(tag, sizeof tag, "svc(%s,%s)", combos[i].a, combos[i].b);
        int rc = B2 ? B2(ref, S(combos[i].a), S(combos[i].b)) : -999;
        printf("%-30s rc=%d\n", tag, rc);
        dump("   after", ref);
        (void)B1;
    }
    return 0;
}
