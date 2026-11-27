// ref+0x50 holds an 8-byte value; bit 8 of it lands on byte 0x51 (the 0x40 the
// join path tests). Set that bit by hand and see whether Associate2's 0x93e1 gate
// opens -- this tells us the driver must make the framework publish that bit.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>
typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
typedef int (*assoc2_t)(void *, CFDictionaryRef, CFStringRef, void *);
static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }
static CFNumberRef N(int v) { return CFNumberCreate(NULL, kCFNumberIntType, &v); }

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *ssid = argc > 1 ? argv[1] : "QCA-TEST";
    int setbit = argc > 2 ? atoi(argv[2]) : 0;
    char pass[128] = {0};
    if (!fgets(pass, sizeof pass, stdin)) return 2;
    pass[strcspn(pass, "\r\n")] = 0;

    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    open_t O = (open_t)dlsym(h, "Apple80211Open");
    bind_t B = (bind_t)dlsym(h, "Apple80211BindToInterface");
    assoc2_t A2 = (assoc2_t)dlsym(h, "Apple80211Associate2");
    void *ref = 0;
    if (O(&ref) || !ref) { printf("open failed\n"); return 1; }
    if (B(ref, S("en1"))) { printf("bind failed\n"); return 1; }

    unsigned char *p = (unsigned char *)ref;
    if (setbit) {
        p[0x51] |= 0x40;
        printf("ref+0x51 forced to 0x%02x\n", p[0x51]);
    } else {
        printf("ref+0x51 = 0x%02x (untouched)\n", p[0x51]);
    }

    CFMutableDictionaryRef rsn = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef cps = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(cps, S("MFP_REQUIRED"), kCFBooleanFalse);
    CFDictionarySetValue(rsn, S("IE_KEY_RSN_CAPS"), cps);
    CFDictionarySetValue(rsn, S("IE_KEY_RSN_VERSION"), N(1));
    CFDictionarySetValue(rsn, S("PRE_AUTH"), kCFBooleanFalse);
    CFDictionarySetValue(rsn, S("NO_PAIRWISE"), kCFBooleanFalse);
    CFDictionarySetValue(rsn, S("PTKSA_REPLAY_COUNTERS"), N(4));
    CFDictionarySetValue(rsn, S("GTKSA_REPLAY_COUNTERS"), N(1));
    CFDictionarySetValue(rsn, S("IE_KEY_RSN_AUTHSELS"),
                         CFArrayCreate(NULL, (const void *[]){ (const void *)N(2) }, 1,
                                       &kCFTypeArrayCallBacks));
    CFDictionarySetValue(rsn, S("IE_KEY_RSN_UCIPHERS"), N(5));
    CFDictionarySetValue(rsn, S("IE_KEY_RSN_MCIPHER"), N(5));

    CFMutableDictionaryRef net = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef pw = S(pass);
    CFDictionarySetValue(net, S("SSID"), CFDataCreate(NULL, (const UInt8 *)ssid, (CFIndex)strlen(ssid)));
    CFDictionarySetValue(net, S("passphrase"), pw);
    CFDictionarySetValue(net, S("password"), pw);
    CFDictionarySetValue(net, S("RSN_IE"), rsn);

    CFMutableDictionaryRef result = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int rc = A2(ref, net, pw, result);
    printf("rc=%d (0x%08x)\n", rc, (unsigned)rc);
    return 0;
}
