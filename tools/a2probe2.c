// Find out whether any Apple80211 getter makes the framework fill the private
// driver-flags qword at ref+0x50 (bit 0x40 of +0x51 is what Apple80211Associate2
// gates the join on). One fork per type keeps a SIGSEGV in the family's own
// dispatcher from losing the whole table.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/wait.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
typedef int (*get_t)(void *, int, int, void *, unsigned long);

static const char *kOut = "/tmp/a2probe.out";

int main(int argc, char **argv)
{
    const int types[] = { 12, 43, 44, 9, 27, 97, 103, 14, 19, 1, 2, 4, 13, 15, 21, 50, 96 };
    const int ntypes = (int)(sizeof(types) / sizeof(types[0]));
    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 2; }
    open_t O = (open_t)dlsym(h, "Apple80211Open");
    bind_t B = (bind_t)dlsym(h, "Apple80211BindToInterface");
    get_t  G = (get_t)dlsym(h, "Apple80211Get");

    void *ref = 0;
    if (O(&ref) || !ref) { printf("open failed\n"); return 3; }
    CFStringRef name = CFStringCreateWithCString(NULL, "en1", kCFStringEncodingUTF8);
    int brc = B(ref, name);
    unsigned char *b = (unsigned char *)ref;
    printf("after-bind rc=%d  +50..51=%02x %02x  bit40=%d\n", brc, b[0x50], b[0x51],
           (b[0x51] & 0x40) ? 1 : 0);

    FILE *out = fopen(kOut, "w");
    if (!out) { printf("cannot open %s\n", kOut); return 4; }
    fclose(out);

    for (int i = 0; i < ntypes; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            unsigned char buf[1024];
            memset(buf, 0, sizeof buf);
            int rc = G(ref, types[i], 0, buf, sizeof buf);
            unsigned char *bb = (unsigned char *)ref;
            FILE *f = fopen(kOut, "a");
            if (f) {
                fprintf(f, "get%-4d rc=%-8d +50..53=%02x %02x %02x %02x  bit40=%d\n",
                        types[i], rc, bb[0x50], bb[0x51], bb[0x52], bb[0x53],
                        (bb[0x51] & 0x40) ? 1 : 0);
                fclose(f);
            }
            _exit(0);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        if (WIFSIGNALED(st)) {
            FILE *f = fopen(kOut, "a");
            if (f) { fprintf(f, "get%-4d CRASH sig=%d\n", types[i], WTERMSIG(st)); fclose(f); }
        }
    }
    FILE *in = fopen(kOut, "r");
    if (in) {
        char line[256];
        while (fgets(line, sizeof line, in)) fputs(line, stdout);
        fclose(in);
    }
    return 0;
}
