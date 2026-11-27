// Which Apple80211 call fills the private byte at ref+0x51 (0x40 = the
// Apple80211Associate2 gate at +2209, 0x20 = the second gate at +2245)?
// Dump the byte after Open, after Bind, and after the interface-list walkers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
typedef int (*listcopy_t)(void *, CFArrayRef *);
typedef CFArrayRef (*iflist_t)(void);

static void dump(const char *tag, void *ref, int rc)
{
    unsigned char *b = (unsigned char *)ref;
    printf("%-26s rc=%-8d +50..53=%02x %02x %02x %02x  b40=%d b20=%d\n", tag, rc,
           b[0x50], b[0x51], b[0x52], b[0x53], (b[0x51] & 0x40) ? 1 : 0, (b[0x51] & 0x20) ? 1 : 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 2; }
    open_t O = (open_t)dlsym(h, "Apple80211Open");
    bind_t B = (bind_t)dlsym(h, "Apple80211BindToInterface");
    iflist_t IL = (iflist_t)dlsym(h, "Apple80211GetIfListCopy");
    listcopy_t VL = (listcopy_t)dlsym(h, "Apple80211GetVirtualIfListCopy");
    printf("symbols open=%p bind=%p iflist=%p viflist=%p\n",
           (void *)O, (void *)B, (void *)IL, (void *)VL);

    void *ref = 0;
    int rc = O(&ref);
    dump("open", ref, rc);
    CFStringRef name = CFStringCreateWithCString(NULL, "en1", kCFStringEncodingUTF8);
    rc = B(ref, name);
    dump("bind", ref, rc);
    if (IL) {
        CFArrayRef a = IL();
        long n = a ? (long)CFArrayGetCount(a) : -1;
        dump("iflist", ref, (int)n);
    }
    if (VL) {
        CFArrayRef a = NULL;
        rc = VL(ref, &a);
        long n = a ? (long)CFArrayGetCount(a) : -1;
        dump("viflist", ref, (int)n);
    }
    return 0;
}
