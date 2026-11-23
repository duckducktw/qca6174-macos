// Ask airportd for a scan through CoreWLAN - the same path the Wi-Fi menu uses,
// so it proves the airportd side (not just the ifnet ioctl channel).
//
// build in the guest:
//   clang -fobjc-arc -framework Foundation -framework CoreWLAN -o /tmp/cwscan cwscan.m
// usage: ./cwscan [en1]
#import <CoreWLAN/CoreWLAN.h>
#import <Foundation/Foundation.h>

int main(int argc, const char *argv[])
{
    @autoreleasepool {
        NSString *name = argc > 1 ? [NSString stringWithUTF8String:argv[1]] : nil;
        CWWiFiClient *client = [CWWiFiClient sharedWiFiClient];
        CWInterface *iface = name ? [client interfaceWithName:name] : [client interface];
        if (iface == nil) {
            printf("no interface (name=%s)\n", name ? [name UTF8String] : "(default)");
            return 2;
        }
        printf("interface=%s power=%d\n", [[iface interfaceName] UTF8String],
               (int)[iface powerOn]);
        NSError *err = nil;
        NSSet<CWNetwork *> *nets = [iface scanForNetworksWithName:nil error:&err];
        if (nets == nil) {
            printf("scan failed: %s\n", err ? [[err localizedDescription] UTF8String] : "nil");
            return 1;
        }
        printf("networks=%lu\n", (unsigned long)[nets count]);
        NSArray *sorted = [[nets allObjects] sortedArrayUsingComparator:^NSComparisonResult(CWNetwork *a, CWNetwork *b) {
            return [@([b rssiValue]) compare:@([a rssiValue])];
        }];
        for (CWNetwork *n in sorted) {
            printf("  ssid='%s' bssid=%s ch=%ld rssi=%ld sec(wpa2=%d wpa3=%d wpa=%d)\n",
                   n.ssid ? [n.ssid UTF8String] : "(hidden)",
                   [[n bssid] UTF8String], (long)[n wlanChannel].channelNumber,
                   (long)[n rssiValue], (int)[n supportsSecurity:kCWSecurityWPA2Personal],
                   (int)[n supportsSecurity:kCWSecurityWPA3Personal],
                   (int)[n supportsSecurity:kCWSecurityWPAPersonal]);
        }
    }
    return 0;
}
