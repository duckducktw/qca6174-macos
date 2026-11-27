// Which byte gates the bind-time capability handshake? Dump ref+0x40..0x70 and
// the surrounding "policy" bytes right after Open and after Bind, so the field the
// framework consults ([rdi+0x5d] / [rbx+0x169]) can be matched to a driver answer.
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>
typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }
static void dump(const char *tag, void *ref) {
    unsigned char *p = (unsigned char *)ref;
    printf("%-10s", tag);
    for (int i = 0x48; i <= 0x68; i++) printf(" %02x", p[i]);
    printf("\n");
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    open_t O = (open_t)dlsym(h, "Apple80211Open");
    bind_t B = (bind_t)dlsym(h, "Apple80211BindToInterface");
    void *ref = 0;
    if (O(&ref) || !ref) { printf("open failed\n"); return 1; }
    printf("offsets   ");
    for (int i = 0x48; i <= 0x68; i++) printf(" %02x", i);
    printf("\n");
    dump("open", ref);
    if (B(ref, S("en1"))) { printf("bind failed\n"); return 1; }
    dump("bind", ref);
    return 0;
}
