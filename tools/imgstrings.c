// Dump printable strings from the mapped IO80211 image's __TEXT segment so the
// framework's property names can be grepped for offline (e.g. anything the bind
// path might read off the interface/service to decide the driver-flags byte).
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>

int main(void)
{
    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    void *sym = dlsym(h, "Apple80211BindToInterface");
    Dl_info info;
    memset(&info, 0, sizeof info);
    if (!sym || !dladdr(sym, &info)) { printf("dladdr failed\n"); return 1; }
    unsigned long base = (unsigned long)info.dli_fbase;
    const struct mach_header_64 *mh = (const struct mach_header_64 *)base;

    const struct load_command *lc = (const struct load_command *)(mh + 1);
    unsigned long textVm = 0, textSize = 0;
    for (int i = 0; i < (int)mh->ncmds; ++i) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const struct segment_command_64 *)lc;
            if (strcmp(sg->segname, "__TEXT") == 0) { textVm = sg->vmaddr; textSize = sg->vmsize; break; }
        }
        lc = (const struct load_command *)((const char *)lc + lc->cmdsize);
    }
    if (!textVm || !textSize) { printf("no __TEXT\n"); return 1; }
    const unsigned char *text = (const unsigned char *)(base + (textVm - textVm));

    char cur[512];
    int n = 0;
    for (unsigned long i = 0; i < textSize; ++i) {
        unsigned char c = text[i];
        if (c >= 0x20 && c < 0x7f) {
            if (n < (int)sizeof(cur) - 1) cur[n++] = (char)c;
        } else {
            if (n >= 5) { cur[n] = 0; printf("%s\n", cur); }
            n = 0;
        }
    }
    if (n >= 5) { cur[n] = 0; printf("%s\n", cur); }
    return 0;
}
