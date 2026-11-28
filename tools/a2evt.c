/* Associate2 with a *valid* mach receive port registered for events.
 *
 * a2mon.c used mach_task_self() as the port, which is not a receive right and
 * SIGSEGVs inside Apple80211StartMonitoringEvent (rc=139).  Here a real
 * CFMachPort is created, so registration and delivery are legal.
 *
 * Prints: ref flags after bind, event-init rc, per-message monitor rc,
 * Associate2 rc + elapsed, and how many mach messages the port received
 * afterwards (i.e. whether the driver's completion events actually arrived).
 *
 * Password comes from stdin.  Never echoes it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
typedef int (*assoc2_t)(void *, CFDictionaryRef, CFStringRef, CFMutableDictionaryRef);
typedef int (*evinit_t)(void *, mach_port_t, void *, void *);
typedef int (*evstart_t)(void *, unsigned);

static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }
static CFNumberRef N(int v) { return CFNumberCreate(NULL, kCFNumberIntType, &v); }

static int drain(mach_port_t port, int budgetMs)
{
    int count = 0;
    for (;;) {
        char buf[2048];
        mach_msg_header_t *hdr = (mach_msg_header_t *)buf;
        mach_msg_return_t r = mach_msg(hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                                       0, sizeof buf, port, budgetMs, MACH_PORT_NULL);
        if (r != MACH_MSG_SUCCESS)
            break;
        ++count;
        if (hdr->msgh_local_port)
            mach_port_deallocate(mach_task_self(), hdr->msgh_local_port);
    }
    return count;
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
    evinit_t EI = (evinit_t)dlsym(h, "Apple80211EventMonitoringInit2");
    evstart_t ES = (evstart_t)dlsym(h, "Apple80211StartMonitoringEvent");
    printf("syms open=%p bind=%p assoc2=%p evinit=%p evstart=%p\n",
           (void *)O, (void *)B, (void *)A2, (void *)EI, (void *)ES);
    if (!O || !B || !A2) return 1;

    void *ref = 0;
    if (O(&ref) || !ref) { printf("open failed\n"); return 1; }
    if (B(ref, S("en1"))) { printf("bind failed\n"); return 1; }

    /* A real receive right: CFMachPort owns the port for us. */
    mach_port_t port = MACH_PORT_NULL;
    CFMachPortRef cfport = CFMachPortCreate(NULL, NULL, NULL, NULL);
    if (cfport != NULL)
        port = CFMachPortGetPort(cfport);
    printf("cfport=%p port=%u\n", (void *)cfport, (unsigned)port);

    if (EI != NULL && port != MACH_PORT_NULL) {
        int rc = EI(ref, port, NULL, NULL);
        printf("eventinit rc=%d (0x%08x)\n", rc, (unsigned)rc);
        const int skipMonitor = (argc > 3 && argv[3][0] == 'n');
        if (rc == 0 && ES != NULL && !skipMonitor) {
            const unsigned msgs[] = { 4, 9, 12, 21, 40 };
            for (unsigned i = 0; i < sizeof msgs / sizeof msgs[0]; ++i) {
                int sr = ES(ref, msgs[i]);
                printf("  monitor msg=%u rc=%d\n", msgs[i], sr);
            }
        }
    }

    CFMutableDictionaryRef net = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(net, S("SSID"), CFDataCreate(NULL, (const UInt8 *)ssid, (CFIndex)strlen(ssid)));
    CFDictionarySetValue(net, S("CHANNEL"), N(chan));
    CFStringRef pw = S(pass);
    CFDictionarySetValue(net, S("passphrase"), pw);
    CFDictionarySetValue(net, S("password"), pw);
    CFMutableDictionaryRef out = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = A2(ref, net, pw, out);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("assoc2 rc=%d (0x%08x) elapsed=%.2fs\n", rc, (unsigned)rc, el);

    int got = drain(port, 300);
    printf("events received=%d\n", got);
    return rc == 0 ? 0 : 1;
}
