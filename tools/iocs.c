// Probe one IOC per forked child so a crash in one does not lose the table.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>
typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
typedef int (*get_t)(void *, int, int, void *, unsigned long);
static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }
static void *H; static open_t O; static bind_t B; static get_t G;

static void one(int type) {
    void *ref = 0;
    if (O(&ref) || !ref) { printf("ioc %3d open failed\n", type); _exit(0); }
    if (B(ref, S("en1"))) { printf("ioc %3d bind failed\n", type); _exit(0); }
    unsigned char buf[512]; memset(buf, 0, sizeof buf);
    int rc = G(ref, type, 0, buf, sizeof buf);
    printf("ioc %3d rc=%6d  %02x %02x %02x %02x\n", type, rc, buf[0], buf[1], buf[2], buf[3]);
    _exit(0);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    H = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    O = (open_t)dlsym(H, "Apple80211Open");
    B = (bind_t)dlsym(H, "Apple80211BindToInterface");
    G = (get_t)dlsym(H, "Apple80211Get");
    int types[] = {1,2,4,9,12,13,14,15,19,21,27,43,44,50,96,97,103};
    for (unsigned i = 0; i < sizeof types / sizeof types[0]; i++) {
        pid_t p = fork();
        if (p == 0) one(types[i]);
        int st = 0; waitpid(p, &st, 0);
        if (WIFSIGNALED(st)) printf("ioc %3d CRASH sig=%d\n", types[i], WTERMSIG(st));
    }
    return 0;
}
