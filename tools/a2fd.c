/* Find the fd that Apple80211Associate2 submits through, then try a harmless
   GET (POWER, type 19) on each candidate fd so we know which one is the 80211
   channel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);

static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    open_t O = (open_t)dlsym(h, "Apple80211Open");
    bind_t B = (bind_t)dlsym(h, "Apple80211BindToInterface");
    void *ref = 0;
    if (O(&ref) || !ref) { printf("open failed\n"); return 1; }
    printf("ref=%p (before bind)\n", ref);
    unsigned int *w = (unsigned int *)ref;
    for (int i = 0; i < 16; i++) printf("  +0x%02x = 0x%08x (%u)\n", i * 4, w[i], w[i]);

    int brc = B(ref, S("en1"));
    printf("bind rc=%d ref=%p\n", brc, ref);
    for (int i = 0; i < 24; i++) printf("  +0x%02x = 0x%08x (%u)\n", i * 4, w[i], w[i]);

    /* struct apple80211req { char ifname[16]; int type; int val; u32 len; void *data; } */
    static unsigned char req[64];
    memcpy(req, "en1", 4);
    *(int *)(req + 16) = 19;      /* APPLE80211_IOC_POWER */
    *(int *)(req + 20) = 0;
    *(unsigned int *)(req + 24) = 16;
    static unsigned char data[64];
    *(void **)(req + 32) = data;

    for (int i = 0; i < 24; i++) {
        int fd = (int)w[i];
        if (fd < 3 || fd > 40) continue;
        if (fd == 1 || fd == 2) continue;
        errno = 0;
        int rc = ioctl(fd, 0xc02869c9UL, req);
        printf("ioctl(get 19) fd=%d (slot +0x%02x) rc=%d errno=%d val=%d\n",
               fd, i * 4, rc, errno, *(int *)(req + 20));
    }
    return 0;
}
