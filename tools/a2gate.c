// Which half of the 0x93e7 gate fails? Patch the conditional jumps around it:
//  0x93d9 je 0x93e7   (r14 == 0 -> refuse)
//  0x93e1 jne 0x977d  (bit 0x40 set -> continue)
// Force the "continue" path by turning 0x93e1's jne into an unconditional jmp.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
typedef int (*assoc2_t)(void *, CFDictionaryRef, CFStringRef, void *);
static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }
static CFNumberRef N(int v) { return CFNumberCreate(NULL, kCFNumberIntType, &v); }

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *ssid = argc > 1 ? argv[1] : "QCA-TEST";
    char pass[128] = {0};
    if (!fgets(pass, sizeof pass, stdin)) return 2;
    pass[strcspn(pass, "\r\n")] = 0;

    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    open_t O = (open_t)dlsym(h, "Apple80211Open");
    bind_t B = (bind_t)dlsym(h, "Apple80211BindToInterface");
    assoc2_t A2 = (assoc2_t)dlsym(h, "Apple80211Associate2");
    if (!O || !B || !A2) { printf("syms missing\n"); return 1; }

    Dl_info info; memset(&info, 0, sizeof info);
    dladdr((void *)A2, &info);
    unsigned char *fn = (unsigned char *)A2;
    unsigned char *img = (unsigned char *)info.dli_fbase;

    int mode = argc > 2 ? atoi(argv[2]) : 0;   // 1 = force jne at 0x93e1, 4 = tag -3900 stores
    vm_address_t page = (vm_address_t)fn & ~(vm_page_size - 1);
    vm_size_t len = (0x2600 / vm_page_size + 2) * vm_page_size;
    if (vm_protect(mach_task_self(), page, len, FALSE,
                   VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE) != KERN_SUCCESS) {
        printf("vm_protect failed\n"); return 1;
    }
    unsigned char *e1 = img + 0x93e1;   // 0f 85 <rel32>  (jne 0x977d)
    unsigned char *d9 = img + 0x93d9;   // 74 0c          (je 0x93e7)
    printf("bytes @93d9: %02x %02x   @93e1: %02x %02x %02x\n", d9[0], d9[1], e1[0], e1[1], e1[2]);
    if (mode == 1) {
        // replace the 6-byte jne with "jmp rel32" + nop, same target
        e1[0] = 0xe9;
        memcpy(e1 + 1, (int[1]){ 0x977d - (0x93e1 + 5) }, 4);
        e1[5] = 0x90;
        printf("forced jmp at 0x93e1\n");
    } else if (mode == 2) {
        memset(img + 0x93ed, 0, 4);       // the -3903 immediate
        printf("zeroed -3903 at 0x93ed\n");
    } else if (mode == 3) {
        d9[0] = 0x90; d9[1] = 0x90;       // nop the "je", so a null r14 falls through
        printf("nop'd je at 0x93d9\n");
    } else if (mode == 4) {
        // tag every -3900 constant, and also force the 0x93e1 branch so we reach them
        e1[0] = 0xe9;
        memcpy(e1 + 1, (int[1]){ 0x977d - (0x93e1 + 5) }, 4);
        e1[5] = 0x90;
        static const unsigned char p4[4] = {0xc4, 0xf0, 0xff, 0xff};
        int tag = 0;
        for (size_t i = 0; i + 4 < 0x2600; i++) {
            if (memcmp(fn + i, p4, 4)) continue;
            int t = ++tag;
            memcpy(fn + i, &t, 4);
        }
        printf("tagged %d -3900 stores\n", tag);
        // remember the offsets so the tag can be mapped back afterwards
        tag = 0;
        for (size_t i = 0; i + 4 < 0x2600; i++) {
            if (memcmp(fn + i, (unsigned char[4]){0,0,0,0}, 0)) break;
        }
    }

    void *ref = 0;
    if (O(&ref) || !ref) { printf("open failed\n"); return 1; }
    if (B(ref, S("en1"))) { printf("bind failed\n"); return 1; }

    CFMutableDictionaryRef rsn = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef cps = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(cps, S("MFP_REQUIRED"), kCFBooleanFalse);
    CFDictionarySetValue(rsn, S("IE_KEY_RSN_CAPS"), cps);
    CFDictionarySetValue(rsn, S("IE_KEY_RSN_VERSION"), N(1));
    CFDictionarySetValue(rsn, S("PRE_AUTH"), kCFBooleanFalse);
    CFDictionarySetValue(rsn, S("NO_PAIRWISE"), kCFBooleanFalse);
    CFDictionarySetValue(rsn, S("PTKSA_REPLAY_COUNTERS"), N(4));
    CFDictionarySetValue(rsn, S("GTKSA_REPLAY_COUNTERS"), N(1));
    CFDictionarySetValue(rsn, S("IE_KEY_RSN_AUTHSELS"),
                         CFArrayCreate(NULL, (const void *[]){ (const void *)N(2) }, 1,
                                       &kCFTypeArrayCallBacks));
    CFDictionarySetValue(rsn, S("IE_KEY_RSN_UCIPHERS"), N(5));
    CFDictionarySetValue(rsn, S("IE_KEY_RSN_MCIPHER"), N(5));

    CFMutableDictionaryRef net = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef pw = S(pass);
    CFDictionarySetValue(net, S("SSID"), CFDataCreate(NULL, (const UInt8 *)ssid, (CFIndex)strlen(ssid)));
    CFDictionarySetValue(net, S("passphrase"), pw);
    CFDictionarySetValue(net, S("password"), pw);
    CFDictionarySetValue(net, S("RSN_IE"), rsn);

    CFMutableDictionaryRef result = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int rc = A2(ref, net, pw, result);
    printf("rc=%d (0x%08x)\n", rc, (unsigned)rc);
    return 0;
}
