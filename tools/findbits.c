// Scan the mapped IO80211 image for the instruction that sets bit 0x40 of the
// private byte at ref+0x51 (the Apple80211Associate2 join gate). Print offsets
// relative to the image base so they can be disassembled offline.
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/vm_prot.h>

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
    printf("base=%#lx magic=%#x ncmds=%d\n", base, mh->magic, mh->ncmds);
    if (mh->magic != MH_MAGIC_64) { printf("not MH_MAGIC_64\n"); return 1; }

    const struct load_command *lc = (const struct load_command *)(mh + 1);
    unsigned long textAddr = 0, textSize = 0;
    for (int i = 0; i < (int)mh->ncmds; ++i) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const struct segment_command_64 *)lc;
            if (strcmp(sg->segname, "__TEXT") == 0) {
                textAddr = sg->vmaddr;
                textSize = sg->vmsize;
                printf("__TEXT vmaddr=%#llx vmsize=%#llx\n",
                       (unsigned long long)sg->vmaddr, (unsigned long long)sg->vmsize);
            }
        }
        lc = (const struct load_command *)((const char *)lc + lc->cmdsize);
    }
    if (!textAddr || !textSize) { printf("no __TEXT\n"); return 1; }

    const unsigned char *p = (const unsigned char *)(base + (textAddr - (unsigned long)0x100000000ULL));
    // Recompute properly: slide = base - __TEXT.vmaddr_preferred; for a cache image
    // the segment is mapped at base + (vmaddr - textVmaddrOfHeaderSegment).
    (void)p;
    unsigned long hdrVmaddr = 0;
    lc = (const struct load_command *)(mh + 1);
    for (int i = 0; i < (int)mh->ncmds; ++i) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const struct segment_command_64 *)lc;
            if (strcmp(sg->segname, "__TEXT") == 0) { hdrVmaddr = sg->vmaddr; break; }
        }
        lc = (const struct load_command *)((const char *)lc + lc->cmdsize);
    }
    unsigned long slide = base - hdrVmaddr;
    const unsigned char *text = (const unsigned char *)(textAddr + slide);
    printf("slide=%#lx text_at=%p\n", slide, (void *)text);

    int hits = 0;
    for (unsigned long i = 0; i + 4 < textSize; ++i) {
        const unsigned char *q = text + i;
        // or byte ptr [reg+0x51], 0x40      -> 80 4? 51 40
        // or dword ptr [reg+0x50], 0x40     -> 83 4? 50 40
        // or qword ptr [reg+0x50], 0x40     -> 48 83 4? 50 40
        if (q[0] == 0x80 && (q[1] & 0xC0) == 0x40 && q[2] == 0x51 && q[3] == 0x40) {
            printf("HIT orb [r+0x51],0x40 at off=%#lx (modrm %02x)\n", i, q[1]);
            ++hits;
        } else if (q[0] == 0x83 && (q[1] & 0xC0) == 0x40 && q[2] == 0x50 && q[3] == 0x40) {
            printf("HIT ord [r+0x50],0x40 at off=%#lx (modrm %02x)\n", i, q[1]);
            ++hits;
        } else if (q[0] == 0x48 && q[1] == 0x83 && (q[2] & 0xC0) == 0x40 && q[3] == 0x50) {
            printf("HIT orq [r+0x50],0x40? at off=%#lx (modrm %02x)\n", i, q[2]);
            ++hits;
        }
        if (hits > 40) break;
    }
    printf("total hits=%d\n", hits);
    return 0;
}
