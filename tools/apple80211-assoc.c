// Minimal apple80211 associate client for the ifnet ioctl channel.
//
// Proves the ASSOCIATE request path on its own, without airportd/CoreWLAN: it
// issues SIOCSA80211 type ASSOCIATE with an apple80211_assoc_data (open network)
// and then polls STATE until the driver reports APPLE80211_S_RUN.
//
// build in the guest: cc -o /tmp/apple80211-assoc apple80211-assoc.c
// usage: ./apple80211-assoc <ssid> [en1] [wait-seconds]
#include <errno.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define SIOCSA80211 2150656456UL
#define SIOCGA80211 3224398281UL

// apple80211_var.h: STATE=13, ASSOCIATE=20 (5 is not ASSOCIATE - sending it there
// lands in the compatibility list that answers success without doing anything).
enum { APPLE80211_IOC_STATE = 13, APPLE80211_IOC_ASSOCIATE = 20 };
struct state_reply { uint32_t version; uint32_t state; };
enum { APPLE80211_S_INIT = 0, APPLE80211_S_SCAN = 1, APPLE80211_S_RUN = 4 };

struct apple80211req {
    char if_name[IFNAMSIZ];
    int type;
    int val;
    uint32_t len;
    void *data;
};

// 0x150 = version, mode, auth lower/upper, ssid_len and the 32-byte SSID;
// the 6-byte BSSID follows right after. Everything past that (key, RSN IE) stays
// zero, which is exactly an open-network join.
struct assoc_open {
    uint32_t version;
    uint16_t mode;
    uint16_t auth_lower;
    uint16_t auth_upper;
    uint32_t ssid_len;
    uint8_t ssid[32];
    uint8_t bssid[6];
    uint8_t rest[400];
};

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: %s <ssid> [ifname] [wait]\n", argv[0]);
        return 2;
    }
    const char *ssid = argv[1];
    const char *name = argc > 2 ? argv[2] : "en1";
    int wait = argc > 3 ? atoi(argv[3]) : 15;
    const size_t ssidLen = strlen(ssid);
    if (ssidLen == 0 || ssidLen > 32) {
        printf("bad ssid length %zu\n", ssidLen);
        return 2;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct assoc_open ad;
    memset(&ad, 0, sizeof ad);
    ad.version = 1;
    ad.ssid_len = (uint32_t)ssidLen;
    memcpy(ad.ssid, ssid, ssidLen);

    struct apple80211req req;
    memset(&req, 0, sizeof req);
    strlcpy(req.if_name, name, sizeof req.if_name);
    req.type = APPLE80211_IOC_ASSOCIATE;
    req.val = 1;                       // set
    req.len = sizeof ad;
    req.data = &ad;
    if (ioctl(fd, SIOCSA80211, &req) < 0) {
        printf("ASSOCIATE failed: %s (%d)\n", strerror(errno), errno);
        return 1;
    }
    printf("ASSOCIATE accepted for '%s' on %s\n", ssid, name);

    for (int i = 0; i < wait; ++i) {
        struct state_reply reply;
        memset(&reply, 0, sizeof reply);
        struct apple80211req get;
        memset(&get, 0, sizeof get);
        strlcpy(get.if_name, name, sizeof get.if_name);
        get.type = APPLE80211_IOC_STATE;
        get.len = sizeof reply;
        get.data = &reply;
        if (ioctl(fd, SIOCGA80211, &get) < 0) {
            printf("STATE read failed: %s\n", strerror(errno));
            return 1;
        }
        printf("  state=%u%s\n", reply.state,
               reply.state == APPLE80211_S_RUN ? " (RUN)" : "");
        if (reply.state == APPLE80211_S_RUN) break;
        sleep(1);
    }
    return 0;
}
