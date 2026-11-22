/* Prints what SystemConfiguration actually sees (SCNetworkInterfaceCopyAll).
 * An interface missing here means SC/networkd never registered it - no hardware
 * port, no service and no Wi-Fi pane, regardless of IOInterfaceType.
 * Build in the guest: clang -framework SystemConfiguration -framework CoreFoundation \
 *   -o /tmp/sc-interfaces tools/sc-interfaces.c
 */
#include <SystemConfiguration/SystemConfiguration.h>
#include <stdio.h>
int main(void) {
    CFArrayRef all = SCNetworkInterfaceCopyAll();
    if (!all) { printf("CopyAll null\n"); return 1; }
    CFIndex n = CFArrayGetCount(all);
    printf("SCNetworkInterfaceCopyAll -> %ld\n", (long)n);
    for (CFIndex i = 0; i < n; i++) {
        SCNetworkInterfaceRef in = (SCNetworkInterfaceRef)CFArrayGetValueAtIndex(all, i);
        CFStringRef bsd = SCNetworkInterfaceGetBSDName(in);
        CFStringRef type = SCNetworkInterfaceGetInterfaceType(in);
        CFStringRef hw = NULL;
        CFStringRef local = SCNetworkInterfaceGetLocalizedDisplayName(in);
        char b[128]="(none)", t[128]="(none)", h[128]="(none)", l[128]="(none)";
        if (bsd) CFStringGetCString(bsd, b, sizeof b, kCFStringEncodingUTF8);
        if (type) CFStringGetCString(type, t, sizeof t, kCFStringEncodingUTF8);
        if (hw) CFStringGetCString(hw, h, sizeof h, kCFStringEncodingUTF8);
        if (local) CFStringGetCString(local, l, sizeof l, kCFStringEncodingUTF8);
        printf("[%ld] bsd=%s type=%s hwport=%s name=%s\n", (long)i, b, t, h, l);
    }
    return 0;
}
