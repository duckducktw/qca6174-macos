// Dump Apple80211* symbol code regions out of the dyld shared cache into a file
// so they can be disassembled on another machine.
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
    printf("%s: wrote %lu bytes to %s (start off=0x%lx)\n", name, len, outfile,
           (unsigned long)((char *)start - (char *)gBase));
    return 0;
}

int main(void)
{
    const char *path = "/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211";
    void *h = dlopen(path, RTLD_NOW);
    if (!h) { printf("dlopen %s failed: %s\n", path, dlerror()); return 1; }

    const char *names[] = {
        "Apple80211Associate2", "Apple80211Associate", "Apple80211Set", "Apple80211Get",
        "Apple80211Open", "Apple80211Close", "Apple80211BindToInterfaceWithService",
        "Apple80211GetIfListCopy", "Apple80211EventMonitoringInit2", NULL
    };
    for (int i = 0; names[i]; i++) {
        void *p = dlsym(h, names[i]);
        Dl_info info;
        memset(&info, 0, sizeof info);
        if (p && dladdr(p, &info)) {
            if (!gBase) gBase = (unsigned long)info.dli_fbase;
            printf("%-34s addr=%p off=0x%08lx base=%p img=%s\n", names[i], p,
                   (unsigned long)((char *)p - (char *)info.dli_fbase), info.dli_fbase,
                   info.dli_fname ? info.dli_fname : "?");
        } else {
            printf("%-34s NOT FOUND\n", names[i]);
        }
    }

    // Dump the region around Associate2 (it is the one we want to understand).
    void *a2 = dlsym(h, "Apple80211Associate2");
    if (a2) {
        void *p = dlsym(h, "Apple80211Set");
        dumpOne("Apple80211Set", p, "/tmp/set.bin", 0x80, 0x6000);
        dumpOne("Apple80211Associate2", a2, "/tmp/assoc2.bin", 0x40, 0x8000);
    }
    return 0;
}
