// Dump the region around Apple80211's bind entry points so the write of the
// private driver-flags qword (ref+0x50) can be found offline.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>

static unsigned long gBase = 0;

static int dumpOne(const char *name, void *p, const char *outfile, unsigned long pre, unsigned long len)
{
    FILE *f = fopen(outfile, "wb");
    if (!f) { printf("cannot open %s\n", outfile); return 1; }
    unsigned char *start = (unsigned char *)p - pre;
    fwrite(start, 1, len, f);
    fclose(f);
    printf("%s: wrote %lu bytes to %s (start off=0x%lx base=%p)\n", name, len, outfile,
           (unsigned long)((char *)start - (char *)gBase), (void *)gBase);
    return 0;
}

int main(void)
{
    const char *path = "/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211";
    void *h = dlopen(path, RTLD_NOW);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    const char *names[] = { "Apple80211BindToInterface", "Apple80211BindToInterfaceWithService",
                            "Apple80211BindToInterfaceWithIOCTL", "Apple80211Open", NULL };
    void *bind = NULL, *binds = NULL, *bindi = NULL;
    for (int i = 0; names[i]; i++) {
        void *p = dlsym(h, names[i]);
        Dl_info info;
        memset(&info, 0, sizeof info);
        if (p && dladdr(p, &info)) {
            if (!gBase) gBase = (unsigned long)info.dli_fbase;
            printf("%-38s addr=%p off=0x%08lx\n", names[i], p,
                   (unsigned long)((char *)p - (char *)info.dli_fbase));
            if (i == 0) bind = p;
            if (i == 1) binds = p;
            if (i == 2) bindi = p;
        } else {
            printf("%-38s NOT FOUND\n", names[i]);
        }
    }
    if (bind)  dumpOne("BindToInterface", bind, "/tmp/bind.bin", 0x400, 0x20000);
    if (binds) dumpOne("BindToInterfaceWithService", binds, "/tmp/binds.bin", 0x400, 0x20000);
    if (bindi) dumpOne("BindToInterfaceWithIOCTL", bindi, "/tmp/bindi.bin", 0x400, 0x10000);
    return 0;
}
