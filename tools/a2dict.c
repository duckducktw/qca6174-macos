/* Differential probe for Apple80211Associate2's dictionary: -3905 means the request
   was submitted and the caller timed out, -3903 means it was refused *before* any
   request left userspace. airportd gets -3903 while a plain {SSID} dictionary gets
   -3905, so whichever extra key flips the result is the trigger. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
typedef int (*assoc2_t)(void *, CFDictionaryRef, CFStringRef, void *);

static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }
static CFDataRef D(const void *p, unsigned n) { return CFDataCreate(NULL, p, n); }
static CFNumberRef N(int v) { return CFNumberCreate(NULL, kCFNumberIntType, &v); }
static CFArrayRef capArray(void)
{
    const void *vals[] = { S("WEP40"), S("WEP104"), S("WEPOPENSYSTEM"), S("ALLOW_WPA2_PSK"),
                           S("ALLOW_WPA3_SAE") };
    return CFArrayCreate(NULL, vals, 5, &kCFTypeArrayCallBacks);
}

static char gSSID[64], gPass[128];

static int run(const char *label, int variant)
{
    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!h) { printf("dlopen failed\n"); return -1; }
    open_t O = (open_t)dlsym(h, "Apple80211Open");
    bind_t B = (bind_t)dlsym(h, "Apple80211BindToInterface");
    assoc2_t A2 = (assoc2_t)dlsym(h, "Apple80211Associate2");
    void *ref = 0;
    if (!O || O(&ref) || !ref) { printf("open failed\n"); return -1; }
    if (B(ref, S("en1"))) { printf("bind failed\n"); return -1; }

    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(d, S("SSID"), D(gSSID, (unsigned)strlen(gSSID)));
    switch (variant) {
    case 1: CFDictionarySetValue(d, S("capabilities"), capArray()); break;
    case 2: CFDictionarySetValue(d, S("ASSOC_FLAGS"), N(8)); break;
    case 3: CFDictionarySetValue(d, S("security"), N(8)); break;
    case 4: CFDictionarySetValue(d, S("transitionDisabledFlags"), N(1)); break;
    case 5: CFDictionarySetValue(d, S("COLOCATED_NETWORK_SCOPE_ID"), N(0)); break;
    case 6: CFDictionarySetValue(d, S("channel"), N(6)); break;
    case 7: CFDictionarySetValue(d, S("RSN_IE"), D("\x30\x14\x01\x00", 4)); break;
    case 8: CFDictionarySetValue(d, S("passphrase"), S(gPass)); break;
    case 9:
        CFDictionarySetValue(d, S("capabilities"), capArray());
        CFDictionarySetValue(d, S("ASSOC_FLAGS"), N(8));
        CFDictionarySetValue(d, S("security"), N(8));
        break;
    default: break;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = A2(ref, d, S(gPass), NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("%-28s rc=%d (0x%08x) elapsed=%.1fs\n", label, rc, (unsigned)rc,
           (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    return rc;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(gSSID, sizeof gSSID, "%s", argc > 1 ? argv[1] : "QCA-TEST");
    if (!fgets(gPass, sizeof gPass, stdin)) gPass[0] = 0;
    gPass[strcspn(gPass, "\r\n")] = 0;
    static const char *labels[] = {"SSID", "+capabilities", "+ASSOC_FLAGS", "+security",
                                   "+transitionDisabledFlags", "+COLOCATED_NETWORK_SCOPE_ID",
                                   "+channel", "+RSN_IE", "+passphrase", "+caps+flags+sec"};
    const int only = argc > 2 ? atoi(argv[2]) : -1;
    for (int v = 0; v < 10; v++) {
        if (only >= 0 && v != only) continue;
        pid_t p = fork();
        if (p == 0) { run(labels[v], v); _exit(0); }
        int st = 0;
        waitpid(p, &st, 0);
        if (WIFSIGNALED(st)) printf("%-28s CRASH sig=%d\n", labels[v], WTERMSIG(st));
    }
    return 0;
}
