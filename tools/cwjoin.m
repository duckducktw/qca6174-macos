// Join through CoreWLAN the same way System Settings does, and print every part of
// the NSError instead of the single line networksetup shows. The passphrase is read
// from stdin so it never appears in argv or in this output.
//
//   xcrun clang -fobjc-arc -framework Foundation -framework CoreWLAN -o cwjoin cwjoin.m
//   printf '%s\n' "$PW" | ./cwjoin QCA-TEST en1
#import <Foundation/Foundation.h>
#import <CoreWLAN/CoreWLAN.h>

int main(int argc, char **argv)
{
    @autoreleasepool {
        const char *ssidArg = argc > 1 ? argv[1] : "QCA-TEST";
        const char *ifArg = argc > 2 ? argv[2] : "en1";
        char pw[256];
        pw[0] = 0;
        if (fgets(pw, sizeof pw, stdin) == NULL)
            return 2;
        size_t n = strlen(pw);
        while (n && (pw[n - 1] == '\n' || pw[n - 1] == '\r'))
            pw[--n] = 0;

        CWWiFiClient *client = [CWWiFiClient sharedWiFiClient];
        CWInterface *iface = [client interfaceWithName:[NSString stringWithUTF8String:ifArg]];
        if (iface == nil) {
            printf("no-interface\n");
            return 3;
        }
        printf("iface=%s power=%d serviceActive=%d\n", [[iface interfaceName] UTF8String],
               [iface powerOn] ? 1 : 0, [iface serviceActive] ? 1 : 0);

        NSError *error = nil;
        CWNetwork *network = nil;
        NSSet *set = [iface scanForNetworksWithName:[NSString stringWithUTF8String:ssidArg]
                                              error:&error];
        if (error)
            printf("scan-error domain=%s code=%ld\n", [[error domain] UTF8String],
                   (long)[error code]);
        for (CWNetwork *n in set) {
            network = n;
            break;
        }
        printf("scan-saw-target=%d\n", network != nil);

        error = nil;
        BOOL ok = [iface associateToNetwork:network
                                   password:[NSString stringWithUTF8String:pw]
                                      error:&error];
        printf("assoc-ok=%d\n", ok ? 1 : 0);
        if (error != nil) {
            printf("assoc-error domain=%s code=%ld desc=%s\n", [[error domain] UTF8String],
                   (long)[error code], [[error localizedDescription] UTF8String]);
            printf("assoc-userinfo=%s\n", [[[error userInfo] description] UTF8String]);
        } else {
            printf("assoc-error none\n");
        }
        printf("ssid-now=%s rssi=%ld\n",
               [[[iface ssid] description] UTF8String], (long)[iface rssiValue]);
    }
    return 0;
}
