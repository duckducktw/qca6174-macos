/* Call Apple80211Associate2 with our own dictionary while interposing
   CFDictionaryGetValue, so every key the framework looks up is printed with its
   name and whether we supplied it. That answers the dictionary schema directly
   instead of guessing key names one build/deploy cycle at a time.

   Password on stdin.  Runs every variant in a forked child (an uncaught
   NSException would otherwise take the whole program down). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
typedef int (*assoc2_t)(void *, CFDictionaryRef, CFStringRef, void *);

static CFDictionaryRef gDict;
static const void *(*real_get)(CFDictionaryRef, const void *);
static char gSSID[64], gPass[128];

static void print_key(const void *key, const char *tag)
{
    char buf[200];
    buf[0] = 0;
    const void *v = real_get(gDict, key);
    if (key != NULL && CFGetTypeID((CFTypeRef)key) == CFStringGetTypeID()) {
        char s[160];
        s[0] = 0;
        CFStringGetCString((CFStringRef)key, s, sizeof s, kCFStringEncodingUTF8);
        snprintf(buf, sizeof buf, "%s key=\"%s\" %s", tag, s, v ? "hit" : "MISS");
    } else {
        snprintf(buf, sizeof buf, "%s key=<type %lu> %s", tag,
                 key ? (unsigned long)CFGetTypeID((CFTypeRef)key) : 0UL, v ? "hit" : "MISS");
    }
    printf("%s\n", buf);
    fflush(stdout);
}

const void *spy_get(CFDictionaryRef d, const void *key)
{
    if (d == gDict && key != NULL)
        print_key(key, "[lookup]");
    return real_get(d, key);
}

Boolean spy_getif(CFDictionaryRef d, const void *key, const void **value)
{
    if (d == gDict && key != NULL)
        print_key(key, "[lookup-if]");
    return CFDictionaryGetValueIfPresent(d, key, value);
}

__attribute__((used)) static struct {
    const void *replacement;
    const void *replacee;
} kInterposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void *)spy_get, (const void *)CFDictionaryGetValue },
};

static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }
static CFDataRef D(const void *p, unsigned n) { return CFDataCreate(NULL, p, n); }
static CFNumberRef N(int v) { return CFNumberCreate(NULL, kCFNumberIntType, &v); }

static int run_one(int variant)
{
    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return -1; }
    open_t O = (open_t)dlsym(h, "Apple80211Open");
    bind_t B = (bind_t)dlsym(h, "Apple80211BindToInterface");
    assoc2_t A2 = (assoc2_t)dlsym(h, "Apple80211Associate2");
    real_get = (const void *(*)(CFDictionaryRef, const void *))dlsym(RTLD_NEXT, "CFDictionaryGetValue");
    if (!real_get) { printf("no RTLD_NEXT CFDictionaryGetValue\n"); return -1; }
    if (!O || !B || !A2) { printf("symbols missing\n"); return -1; }

    void *ref = 0;
    if (O(&ref) || !ref) { printf("open failed\n"); return -1; }
    if (B(ref, S("en1"))) { printf("bind failed\n"); return -1; }

    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(d, S("SSID"), D(gSSID, (unsigned)strlen(gSSID)));
    CFStringRef pass = S(gPass);
    if (variant >= 1) CFDictionarySetValue(d, S("passphrase"), pass);
    if (variant >= 2) CFDictionarySetValue(d, S("password"), pass);
    if (variant >= 3) CFDictionarySetValue(d, S("security"), N(8));
    gDict = d;
    printf("--- variant %d, dict has %ld keys\n", variant, (long)CFDictionaryGetCount(d));
    fflush(stdout);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = A2(ref, d, pass, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("--- variant %d rc=%d (0x%08x) elapsed=%.1fs\n", variant, rc, (unsigned)rc,
           (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    return rc;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(gSSID, sizeof gSSID, "%s", argc > 1 ? argv[1] : "QCA-TEST");
    if (!fgets(gPass, sizeof gPass, stdin)) gPass[0] = 0;
    gPass[strcspn(gPass, "\r\n")] = 0;

    for (int v = 0; v < 4; v++) {
        pid_t p = fork();
        if (p == 0) { run_one(v); _exit(0); }
        int st = 0;
        waitpid(p, &st, 0);
        if (WIFSIGNALED(st)) printf("--- variant %d CRASH sig=%d\n", v, WTERMSIG(st));
    }
    return 0;
}
