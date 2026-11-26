/* Call the *user-space* API airportd itself uses for "click to join":
   Apple80211Associate2(ref, netDict, password, out).  Print the return code and
   see whether the kernel (our driver) receives ASSOCIATE(20) at all.

   Password comes from stdin.  Each attempt runs in a forked child so a crash
   only kills that attempt. */
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

static void *H;
static open_t O; static bind_t B; static assoc2_t A2;
static char gSSID[64], gPass[128];

static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }
static CFNumberRef N(int v) { return CFNumberCreate(NULL, kCFNumberIntType, &v); }

static int run_one(const char *label, int variant)
{
    void *ref = 0;
    if (O(&ref) || !ref) { printf("%-22s open failed\n", label); return -1; }
    int brc = B(ref, S("en1"));
    if (brc) { printf("%-22s bind rc=%d\n", label, brc); return -1; }
    printf("%-22s bind ok ref=%p\n", label, ref);

    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef ssid = S(gSSID);
    CFStringRef pass = S(gPass);
    CFDictionarySetValue(d, S("SSID"), CFDataCreate(NULL, (const UInt8 *)gSSID, (CFIndex)strlen(gSSID)));
    if (variant >= 0) {
        CFDictionarySetValue(d, S("passphrase"), pass);
        CFDictionarySetValue(d, S("password"), pass);
    }
    if (variant >= 2) {
        CFDictionarySetValue(d, S("CHANNEL"), N(6));
    }

    errno = 0;
    struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
    int rc = A2(ref, d, pass, NULL);
    clock_gettime(CLOCK_MONOTONIC,&t1);
    double el = (t1.tv_sec-t0.tv_sec) + (t1.tv_nsec-t0.tv_nsec)/1e9;
    printf("elapsed=%.1fs ", el);
    printf("%-22s rc=%d (0x%08x) errno=%d\n", label, rc, (unsigned)rc, errno);
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
    O  = (open_t)dlsym(H, "Apple80211Open");
    B  = (bind_t)dlsym(H, "Apple80211BindToInterface");
    A2 = (assoc2_t)dlsym(H, "Apple80211Associate2");
    printf("syms: open=%p bind=%p assoc2=%p  ssid=%s\n", (void *)O, (void *)B, (void *)A2, gSSID);
    if (!O || !B || !A2) return 1;

    const char *labels[] = {"ssid+pass", "ssid+pass+ch", "ssid+pass+ch2"};
    for (int v = 0; v < 3; v++) {
        pid_t p = fork();
        if (p == 0) { int rc = run_one(labels[v], v); _exit(rc == 0 ? 0 : 1); }
        int st = 0; waitpid(p, &st, 0);
        if (WIFSIGNALED(st)) printf("%-22s CRASH sig=%d\n", labels[v], WTERMSIG(st));
    }
    return 0;
}
