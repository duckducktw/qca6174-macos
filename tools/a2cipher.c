// Sweep the RSN dictionary keys so see which combination makes
// Apple80211Associate2 answer -3903 (capability gate) instead of -3905 (the
// 10.2s completion timeout we always get). The -3903 value is the one airportd
// sees, so the variant that flips is the capability our driver does not declare.
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
static CFArrayRef NA(const int *v, int n)
{
    CFNumberRef *o = calloc(n, sizeof *o);
    for (int i = 0; i < n; i++) o[i] = N(v[i]);
    CFArrayRef a = CFArrayCreate(NULL, (const void **)o, n, &kCFTypeArrayCallBacks);
    return a;
}

static const UInt8 kRsn[] = {0x30,0x14,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,
                             0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x02,0x00,0x00};

static int run_one(const char *label, const int *authsels, int nauth, int ucipher,
                   int mcipher, int with_rsn, int ciphers_as_array, int replay)
{
    void *ref = 0;
    if (O(&ref) || !ref) { printf("%-26s open failed\n", label); return -1; }
    if (B(ref, S("en1"))) { printf("%-26s bind failed\n", label); return -1; }

    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef pass = S(gPass);
    CFDictionarySetValue(d, S("SSID"), CFDataCreate(NULL, (const UInt8 *)gSSID, (CFIndex)strlen(gSSID)));
    CFDictionarySetValue(d, S("passphrase"), pass);
    CFDictionarySetValue(d, S("password"), pass);
    // "RSN_IE" is not the raw IE blob: _getRSNCapabilities does
    // CFDictionaryGetValue(<RSN_IE value>, "IE_KEY_RSN_CAPS"), so the value must be a
    // dictionary of the parsed IE keys (measured: CFData crashed with
    // -[__NSCFData objectForKey:]). The cipher/auth keys nest inside it.
    if (with_rsn) {
        CFMutableDictionaryRef rsn = CFDictionaryCreateMutable(NULL, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFMutableDictionaryRef caps = CFDictionaryCreateMutable(NULL, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(caps, S("MFP_REQUIRED"), kCFBooleanFalse);
        CFDictionarySetValue(rsn, S("IE_KEY_RSN_CAPS"), caps);
        CFDictionarySetValue(rsn, S("IE_KEY_RSN_VERSION"), N(1));
        // The PSK branch walks these four as well and bails out of the whole
        // function when any is missing (measured: leaving them out gives -3903
        // even though every cipher value was accepted).
        CFDictionarySetValue(rsn, S("PRE_AUTH"), kCFBooleanFalse);
        CFDictionarySetValue(rsn, S("NO_PAIRWISE"), kCFBooleanFalse);
        CFDictionarySetValue(rsn, S("PTKSA_REPLAY_COUNTERS"), N(replay));
        CFDictionarySetValue(rsn, S("GTKSA_REPLAY_COUNTERS"), N(replay));
        if (nauth > 0)
            CFDictionarySetValue(rsn, S("IE_KEY_RSN_AUTHSELS"), NA(authsels, nauth));
        if (ucipher > 0)
            CFDictionarySetValue(rsn, S("IE_KEY_RSN_UCIPHERS"), N(ucipher));
        if (mcipher > 0)
            CFDictionarySetValue(rsn, S("IE_KEY_RSN_MCIPHER"), N(mcipher));
        CFDictionarySetValue(d, S("RSN_IE"), rsn);
    }

    errno = 0;
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = A2(ref, d, pass, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("%-26s elapsed=%4.1fs rc=%d (0x%08x) errno=%d\n", label, el, rc, (unsigned)rc, errno);
    return rc;
}

struct v { const char *label; int authsels[2]; int nauth; int uc; int mc; int rsn; int arr; int replay; };

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

    struct v vs[] = {
        {"psk ccmp4 rp2",        {2,0}, 1, 4, 4, 1, 0, 2},
        {"psk ccmp5 rp2",        {2,0}, 1, 5, 5, 1, 0, 2},
        {"psk aes8  rp2",        {2,0}, 1, 8, 8, 1, 0, 2},
        {"psk 9/8   rp2",        {2,0}, 1, 9, 8, 1, 0, 2},
        {"psk ccmp5 rp0",        {2,0}, 1, 5, 5, 1, 0, 0},
        {"psk ccmp5 rp4",        {2,0}, 1, 5, 5, 1, 0, 4},
        {"psk nocipher rp2",     {2,0}, 1, 0, 0, 1, 0, 2},
        {"pskmix rp2",           {1,2}, 2, 5, 5, 1, 0, 2},
        {"authsels only",        {2,0}, 1, 0, 0, 0, 0, 0},
        {"no ie keys",           {0,0}, 0, 0, 0, 0, 0, 0},
    };
    int n = (int)(sizeof vs / sizeof vs[0]);
    for (int i = 0; i < n; i++) {
        pid_t p = fork();
        if (p == 0) {
            int rc = run_one(vs[i].label, vs[i].authsels, vs[i].nauth, vs[i].uc, vs[i].mc,
                             vs[i].rsn, vs[i].arr, vs[i].replay);
            _exit(rc == 0 ? 0 : 1);
        }
        int st = 0; waitpid(p, &st, 0);
        if (WIFSIGNALED(st)) printf("%-26s CRASH sig=%d\n", vs[i].label, WTERMSIG(st));
    }
    return 0;
}
