/* Join through Apple80211Associate2 *with* event monitoring registered, which is
   what airportd does and what a2join is missing. Without a reader on the
   kern_event stream Associate2 always ends in -3905 after 10.2 s.

   usage: a2mon <ssid>   (password on stdin) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <mach/mach.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
typedef int (*assoc2_t)(void *, CFDictionaryRef, CFStringRef, void *);
typedef int (*evinit_t)(void *, unsigned int, void *, void *);
typedef int (*evstart_t)(void *, unsigned int);

static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }
static CFDataRef D(const void *p, unsigned n) { return CFDataCreate(NULL, p, n); }

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *ssid = argc > 1 ? argv[1] : "QCA-TEST";
    char pass[128] = {0};
    if (!fgets(pass, sizeof pass, stdin)) pass[0] = 0;
    pass[strcspn(pass, "\r\n")] = 0;

    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    open_t O = (open_t)dlsym(h, "Apple80211Open");
    bind_t B = (bind_t)dlsym(h, "Apple80211BindToInterface");
    assoc2_t A2 = (assoc2_t)dlsym(h, "Apple80211Associate2");
    evinit_t EI = (evinit_t)dlsym(h, "Apple80211EventMonitoringInit2");
    evstart_t ES = (evstart_t)dlsym(h, "Apple80211StartMonitoringEvent");
    void *halt = dlsym(h, "Apple80211EventMonitoringHalt");
    printf("syms open=%p bind=%p assoc2=%p evinit=%p evstart=%p halt=%p\n", (void *)O, (void *)B,
           (void *)A2, (void *)EI, (void *)ES, halt);
    if (!O || !B || !A2) return 1;

    void *ref = 0;
    if (O(&ref) || !ref) { printf("open failed\n"); return 1; }
    if (B(ref, S("en1"))) { printf("bind failed\n"); return 1; }

    // Event monitoring: the second argument has to be non-zero (measured at
    // 0x1d6ac: test r15,r15 / je -> -3900); the mach port stands in for whatever
    // airportd hands over.
    if (EI != NULL && ES != NULL) {
        mach_port_t port = mach_task_self();
        int rc = EI(ref, port, NULL, NULL);
        printf("eventinit rc=%d (0x%08x)\n", rc, (unsigned)rc);
        if (rc == 0) {
            for (unsigned msg = 0; msg <= 21; msg++) {
                int sr = ES(ref, msg);
                if (msg == 4 || msg == 9 || sr != 0)
                    printf("  startmonitor msg=%u rc=%d\n", msg, sr);
            }
        }
    } else {
        printf("event monitoring symbols missing\n");
    }

    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(d, S("SSID"), D(ssid, (unsigned)strlen(ssid)));
    CFStringRef pw = S(pass);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    /* 4th argument must be a real mutable dictionary (airportd passes
       [NSMutableDictionary dictionaryWithCapacity:0]); NULL hits the early
       test r14,r14 exit and returns -3903 without reaching the IOCTL. */
    CFMutableDictionaryRef out = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int rc = A2(ref, d, pw, out);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("assoc2 rc=%d (0x%08x) elapsed=%.1fs\n", rc, (unsigned)rc,
           (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    return rc == 0 ? 0 : 1;
}
