// Single Apple80211Get for one IOC type, printing pid so the driver-side
// Ath10kClientTrace can be matched: answers "did this request reach the driver,
// and what did the driver answer?" without airportd traffic confusing the view.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
typedef int (*get_t)(void *, int, int, void *, unsigned long);

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const int type = argc > 1 ? atoi(argv[1]) : 13;
    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 2; }
    open_t O = (open_t)dlsym(h, "Apple80211Open");
    bind_t B = (bind_t)dlsym(h, "Apple80211BindToInterface");
    get_t  G = (get_t)dlsym(h, "Apple80211Get");
    if (!O || !B || !G) { printf("missing symbols\n"); return 2; }
    void *ref = 0;
    int rc = O(&ref);
    printf("pid=%d open=%d ref=%p\n", (int)getpid(), rc, ref);
    if (rc || !ref) return 3;
    CFStringRef name = CFStringCreateWithCString(NULL, "en1", kCFStringEncodingUTF8);
    rc = B(ref, name);
    printf("bind=%d\n", rc);
    unsigned char buf[512];
    memset(buf, 0, sizeof buf);
    rc = G(ref, type, 0, buf, sizeof buf);
    printf("GET type=%d rc=%d data=%02x %02x %02x %02x\n", type, rc, buf[0], buf[1], buf[2], buf[3]);
    return 0;
}
