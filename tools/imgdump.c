// 1) dump the IO80211 framework image __TEXT to /tmp/img.bin
// 2) read the CFString constants used by Apple80211Associate2 as dict keys
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <CoreFoundation/CoreFoundation.h>

static unsigned long offs[] = {
    0x31b61430, 0x31b614f0, 0x31b61470, 0x31b61510, 0x31b61530, 0x31b61550,
    0x31b61570, 0x31b61590, 0x31b615b0, 0x31b615d0, 0x31b615f0, 0x31b61610,
    0x31b61730, 0x31b62110, 0x31b62130, 0
};

static void showString(void *val)
{
    // __CFConstantString layout: isa(8) flags(8) cstring(8) length(8)
    char **cs = (char **)((char *)val + 16);
    unsigned long *len = (unsigned long *)((char *)val + 24);
    char *p = *cs;
    if (p && (unsigned long)p > 0x7ff000000000UL) {
        printf("cstring=\"%.*s\" len=%lu\n", 64, p, *len);
    } else {
        printf("(unexpected layout) +16=%p +24=0x%lx\n", (void *)p, *len);
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); return 1; }
    void *setp = dlsym(h, "Apple80211Set");
    Dl_info info; memset(&info, 0, sizeof info);
    dladdr(setp, &info);
    unsigned long base = (unsigned long)info.dli_fbase;
    printf("base=%p\n", info.dli_fbase);

    // find the mach header for this image
    const struct mach_header_64 *mh = NULL;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        if ((unsigned long)_dyld_get_image_header(i) == base) { mh = _dyld_get_image_header(i); break; }
    }
    if (mh) {
        const struct load_command *lc = (const struct load_command *)(mh + 1);
        for (uint32_t i = 0; i < mh->ncmds; i++) {
            if (lc->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *sg = (const struct segment_command_64 *)lc;
                printf("seg %-16s vmaddr=0x%llx vmsize=0x%llx fileoff=0x%llx filesize=0x%llx\n",
                       sg->segname, sg->vmaddr, sg->vmsize, sg->fileoff, sg->filesize);
                if (!strcmp(sg->segname, "__TEXT") && argc > 1) {
                    unsigned long n = sg->vmsize;
                    if (n > 0x400000) n = 0x400000;
                    FILE *f = fopen("/tmp/img.bin", "wb");
                    fwrite((void *)base, 1, n, f);
                    fclose(f);
                    printf("wrote /tmp/img.bin %lu bytes\n", n);
                }
            }
            lc = (const struct load_command *)((char *)lc + lc->cmdsize);
        }
    }

    for (int i = 0; offs[i]; i++) {
        unsigned long slot = base + offs[i];
        void *val = *(void **)slot;
        printf("off 0x%08lx val=%p ", offs[i], val);
        if (!val) { printf("(null)\n"); continue; }
        showString(val);
    }
    return 0;
}
