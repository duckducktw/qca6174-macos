// Print the BSD interface type (if_data.ifi_type) of every AF_LINK interface.
//
// Why this exists: `ifconfig`, `ioreg` and `networksetup` all fail to answer
// "what type does the kernel think this unit is". ioreg shows the driver's
// IOInterfaceType *property*, which is not the same value - a driver can publish
// 71 there while the ifnet is still IFT_ETHER, and SystemConfiguration
// classifies the unit from the ifnet, not the property. Build and run inside the
// guest:
//
//   xcrun clang -o ifnet-type ifnet-type.c && ./ifnet-type
//
// Expected: en0 type=0x06 (IFT_ETHER). A Wi-Fi unit must report 0x47
// (IFT_IEEE80211); anything else means SystemConfiguration will file it as an
// Ethernet service even if the driver property says otherwise.

#include <stdio.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_var.h>

int main(void)
{
    struct ifaddrs *ifa, *p;
    if (getifaddrs(&ifa) != 0) {
        perror("getifaddrs");
        return 1;
    }
    for (p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_LINK)
            continue;
        struct if_data *d = (struct if_data *)p->ifa_data;
        printf("%-6s type=0x%02x (%u) mtu=%u\n", p->ifa_name,
               d ? d->ifi_type : 0, d ? d->ifi_type : 0, d ? d->ifi_mtu : 0);
    }
    freeifaddrs(ifa);
    return 0;
}
