// Replay airportd's interface match and then attempt the user-client open it
// performs, so "match failed" and "client refused" stop looking identical.
//
//   Apple80211 does, per interface:
//     IOServiceGetMatchingService(kIOMainPortDefault,
//         { IOPropertyMatch = { IOInterfaceName = "<bsd name>" } })
//     IORegistryEntryCreateCFProperty(svc, "IO80211InterfaceRole", ...) == "Infrastructure"
//     IOServiceOpen(svc, mach_task_self(), 0, &conn)
//
// Build: clang -framework IOKit -framework CoreFoundation -o io-match io-match.c
// Run:   ./io-match en1
#include <stdio.h>
#include <string.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>

static void printStringProp(io_service_t svc, const char *key)
{
    CFStringRef k = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFTypeRef v = IORegistryEntryCreateCFProperty(svc, k, kCFAllocatorDefault, 0);
    if (v == NULL) {
        printf("    %-24s <absent>\n", key);
    } else {
        char buf[256] = {0};
        if (CFGetTypeID(v) == CFStringGetTypeID()) {
            CFStringGetCString(v, buf, sizeof buf, kCFStringEncodingUTF8);
            printf("    %-24s \"%s\" (CFString)\n", key, buf);
        } else {
            printf("    %-24s <type %lu>\n", key, (unsigned long)CFGetTypeID(v));
        }
        CFRelease(v);
    }
    CFRelease(k);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: io-match <ifname>\n"); return 2; }
    io_iterator_t it = IO_OBJECT_NULL;
    CFMutableDictionaryRef match = IOServiceMatching("IOService");
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, argv[1], kCFStringEncodingUTF8);
    CFDictionarySetValue(props, CFSTR("IOInterfaceName"), name);
    CFDictionarySetValue(match, CFSTR("IOPropertyMatch"), props);

    kern_return_t kr = IOServiceGetMatchingServices(kIOMainPortDefault, match, &it);
    if (kr != KERN_SUCCESS) { printf("matching failed 0x%x\n", kr); return 1; }

    int n = 0;
    io_service_t svc = IO_OBJECT_NULL;
    while ((svc = IOIteratorNext(it)) != IO_OBJECT_NULL) {
        ++n;
        io_name_t cls = {0};
        uint64_t entryID = 0;
        IOObjectGetClass(svc, cls);
        IORegistryEntryGetRegistryEntryID(svc, &entryID);
        printf("[%d] class=%s entryID=0x%llx\n", n - 1, cls,
               (unsigned long long)entryID);
        printStringProp(svc, "IO80211InterfaceRole");
        printStringProp(svc, "IOUserClientClass");
        printStringProp(svc, "IO80211VirtualInterfaceRole");
        io_connect_t conn = IO_OBJECT_NULL;
        kern_return_t okr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
        printf("    IOServiceOpen(type 0)  -> 0x%08x %s\n", okr,
               okr == KERN_SUCCESS ? "OK" : "(failed)");
        if (okr == KERN_SUCCESS) {
            io_name_t ccls = {0};
            IOObjectGetClass(conn, ccls);
            printf("    client class           %s\n", ccls);
            IOServiceClose(conn);
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    if (n == 0) printf("no service matched IOInterfaceName=\"%s\"\n", argv[1]);
    return 0;
}
