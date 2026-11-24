// Minimal apple80211 associate client for the ifnet ioctl channel.
//
// Proves the ASSOCIATE request path on its own, without airportd/CoreWLAN: it
// issues SIOCSA80211 type ASSOCIATE with an apple80211_assoc_data and then polls
// STATE until the driver reports APPLE80211_S_RUN. With a passphrase it joins a
// WPA2-PSK network (the key travels in ad_key; the driver derives the PMK).
//
// build in the guest: cc -o /tmp/a11assoc apple80211-assoc.c
// usage: ./a11assoc <ssid> [en1] [wait] [passphrase]
//        (without argv[4] a passphrase is read from stdin, keeping it off `ps`)
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
enum { APPLE80211_S_INIT = 0, APPLE80211_S_SCAN = 1, APPLE80211_S_RUN = 4 };

// apple80211_assoc_data, from the SDK's own static_asserts and struct order:
//   version 0, mode 4, auth_lower 6, auth_upper 8, ssid_len 12, ssid 16,
//   bssid 48, ad_key 0x38 (56), ad_rsn_ie_len 206.
#define AD_AUTH_UPPER_OFFSET 8
#define AD_SSID_LEN_OFFSET 12
#define AD_SSID_OFFSET 16
#define AD_KEY_OFFSET 0x38
#define AD_KEY_LEN_OFFSET (AD_KEY_OFFSET + 4)
#define AD_KEY_BYTES_OFFSET (AD_KEY_OFFSET + 16)

#define APPLE80211_AUTHTYPE_WPA2_PSK (1u << 3)
#define APPLE80211_AUTHTYPE_SHA256_PSK (1u << 10)

struct apple80211req {
    char if_name[IFNAMSIZ];
    int type;
    int val;
    uint32_t len;
    void *data;
};

struct state_reply { uint32_t version; uint32_t state; };

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: %s <ssid> [ifname] [wait] [passphrase]\n", argv[0]);
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

    char pass[128] = {0};
    if (argc > 4) {
        strlcpy(pass, argv[4], sizeof pass);
    } else if (!isatty(0)) {
        if (fgets(pass, sizeof pass, stdin) == NULL)
            pass[0] = 0;
        pass[strcspn(pass, "\r\n")] = 0;
    }
    const size_t passLen = strlen(pass);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    uint8_t ad[512];
    memset(ad, 0, sizeof ad);
    *(uint32_t *)(ad + 0) = 1;                                  // version
    *(uint32_t *)(ad + AD_SSID_LEN_OFFSET) = (uint32_t)ssidLen;
    memcpy(ad + AD_SSID_OFFSET, ssid, ssidLen);
    if (passLen) {
        const uint16_t auth = (passLen == 64) ? (uint16_t)APPLE80211_AUTHTYPE_SHA256_PSK
                                              : (uint16_t)APPLE80211_AUTHTYPE_WPA2_PSK;
        *(uint16_t *)(ad + AD_AUTH_UPPER_OFFSET) = auth;
        *(uint32_t *)(ad + AD_KEY_LEN_OFFSET) = (uint32_t)passLen;
        memcpy(ad + AD_KEY_BYTES_OFFSET, pass, passLen > 64 ? 64 : passLen);
    }
    memset(pass, 0, sizeof pass);

    struct apple80211req req;
    memset(&req, 0, sizeof req);
    strlcpy(req.if_name, name, sizeof req.if_name);
    req.type = APPLE80211_IOC_ASSOCIATE;
    req.val = 1;
    req.len = sizeof ad;
    req.data = ad;
    if (ioctl(fd, SIOCSA80211, &req) < 0) {
        printf("ASSOCIATE failed: %s (%d)\n", strerror(errno), errno);
        return 1;
    }
    printf("ASSOCIATE accepted for '%s' on %s (%s)\n", ssid, name,
           passLen ? "wpa2-psk" : "open");

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
