/* airportd-faithful Apple80211Associate2 call.
 *
 * The difference from tools/a2join.c: the 4th argument is a real
 * CFMutableDictionary (airportd passes [NSMutableDictionary dictionaryWithCapacity:0]).
 * Passing NULL hits the early `test r14,r14` exit and returns -3903 in 0.0 s without
 * ever reaching the capability checks, which is why earlier runs measured the wrong path.
 *
 * Prints rc, elapsed and the private ref flags (ref+0x50..0x60) after bind and after the
 * call, so the "native interface" byte (ref+0x5d) and the assoc gate byte (ref+0x51) are
 * visible in one shot.  Password comes from stdin; never echoed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
typedef int (*assoc2_t)(void *, CFDictionaryRef, CFStringRef, CFMutableDictionaryRef);

static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }
static CFNumberRef N(int v) { return CFNumberCreate(NULL, kCFNumberIntType, &v); }

static void dump(const char *tag, void *ref)
{
    unsigned char *b = (unsigned char *)ref;
    printf("%-12s +50..5f:", tag);
    for (int i = 0x50; i < 0x60; ++i) printf(" %02x", b[i]);
    printf("   b40=%d b20=%d native(5d)=%d\n",
           (b[0x51] & 0x40) ? 1 : 0, (b[0x51] & 0x20) ? 1 : 0, b[0x5d]);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *ssid = argc > 1 ? argv[1] : "QCA-TEST";
    const int chan = argc > 2 ? atoi(argv[2]) : 6;
    char pass[128] = {0};
    if (!fgets(pass, sizeof pass, stdin)) { printf("no password on stdin\n"); return 2; }
    pass[strcspn(pass, "\r\n")] = 0;

    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    open_t O = (open_t)dlsym(h, "Apple80211Open");
    bind_t B = (bind_t)dlsym(h, "Apple80211BindToInterface");
    assoc2_t A2 = (assoc2_t)dlsym(h, "Apple80211Associate2");
    if (!O || !B || !A2) { printf("missing symbols\n"); return 1; }

    void *ref = 0;
    if (O(&ref) || !ref) { printf("open failed\n"); return 1; }
    int brc = B(ref, S("en1"));
    printf("bind rc=%d ref=%p\n", brc, ref);
    if (brc) return 1;
    dump("after-bind", ref);

    /* netDict: the shape airportd hands over (scan record copy + credentials). */
    CFMutableDictionaryRef net = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(net, S("SSID"), CFDataCreate(NULL, (const UInt8 *)ssid, (CFIndex)strlen(ssid)));
    CFDictionarySetValue(net, S("CHANNEL"), N(chan));
    CFStringRef pw = S(pass);
    CFDictionarySetValue(net, S("passphrase"), pw);
    CFDictionarySetValue(net, S("password"), pw);

    /* 4th argument: airports's empty mutable result dictionary - NOT NULL. */
    CFMutableDictionaryRef out = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = A2(ref, net, pw, out);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    dump("after-call", ref);
    printf("rc=%d (0x%08x) elapsed=%.2fs outDictCount=%ld\n",
           rc, (unsigned)rc, el, out ? (long)CFDictionaryGetCount(out) : -1);
    return rc == 0 ? 0 : 1;
}
