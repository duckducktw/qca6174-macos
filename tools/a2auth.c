// Which AUTH_SEL_OVERRIDE value reproduces airportd's -3903?
// Apple80211Associate2's jump table sends auth_sel 8 through 0x980a, which
// refuses with -3903 unless [ref+0x59] & 8. If one of these values returns
// -3903 instead of the -3905 timeout, that gate is airportd's wall.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
typedef int (*assoc2_t)(void *, CFDictionaryRef, CFStringRef, void *);

static void *H; static open_t O; static bind_t B; static assoc2_t A2;
static char gSSID[64], gPass[128];

static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }
static CFNumberRef N(int v) { return CFNumberCreate(NULL, kCFNumberIntType, &v); }

static int run_one(int authsel, int withrsn)
{
    void *ref = 0;
    if (O(&ref) || !ref) { printf("authsel=%d open failed\n", authsel); return -1; }
    if (B(ref, S("en1"))) { printf("authsel=%d bind failed\n", authsel); return -1; }

    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef pass = S(gPass);
    CFDictionarySetValue(d, S("SSID"), CFDataCreate(NULL, (const UInt8 *)gSSID, (CFIndex)strlen(gSSID)));
    CFDictionarySetValue(d, S("passphrase"), pass);
    CFDictionarySetValue(d, S("password"), pass);
    CFDictionarySetValue(d, S("AUTH_SEL_OVERRIDE"), N(authsel));
    if (withrsn) {
        static const UInt8 rsn[] = {0x30,0x14,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,
                                    0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x02,0x00,0x00};
        CFDictionarySetValue(d, S("RSN_IE"), CFDataCreate(NULL, rsn, sizeof rsn));
    }

    errno = 0;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = A2(ref, d, pass, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("authsel=%-2d rsn=%d elapsed=%.1fs rc=%d (0x%08x) errno=%d\n",
           authsel, withrsn, el, rc, (unsigned)rc, errno);
    return rc;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(gSSID, sizeof gSSID, "%s", argc > 1 ? argv[1] : "QCA-TEST");
    if (!fgets(gPass, sizeof gPass, stdin)) gPass[0] = 0;
    gPass[strcspn(gPass, "\r\n")] = 0;
    H = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!H) { printf("dlopen: %s\n", dlerror()); return 1; }
    O = (open_t)dlsym(H, "Apple80211Open");
    B = (bind_t)dlsym(H, "Apple80211BindToInterface");
    A2 = (assoc2_t)dlsym(H, "Apple80211Associate2");
    if (!O || !B || !A2) { printf("syms missing\n"); return 1; }
    int withrsn = argc > 2 ? atoi(argv[2]) : 0;
    for (int k = 1; k <= 12; k++) {
        pid_t p = fork();
        if (p == 0) { int rc = run_one(k, withrsn); _exit(rc == 0 ? 0 : 1); }
        int st = 0; waitpid(p, &st, 0);
        if (WIFSIGNALED(st)) printf("authsel=%-2d CRASH sig=%d\n", k, WTERMSIG(st));
    }
    return 0;
}
