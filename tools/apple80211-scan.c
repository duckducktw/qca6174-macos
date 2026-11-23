// Minimal apple80211 scan client for the ifnet ioctl channel.
//
// airportd and the deprecated `airport` tool both go through
// SIOCSA80211/SIOCGA80211 with `struct apple80211req`; this does the same thing
// without CoreWLAN, so a scan can be proven end to end (request accepted, cursor
// protocol served, RSN element delivered) even when no UI client is polling.
//
// build in the guest: cc -o /tmp/apple80211-scan apple80211-scan.c
// usage: ./apple80211-scan [en1] [wait-seconds]
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

#define APPLE80211_IOC_SCAN_REQ 10
#define APPLE80211_IOC_SCAN_RESULT 11

#define APPLE80211_ADDR_LEN 6
#define APPLE80211_MAX_SSID_LEN 32
#define APPLE80211_MAX_RATES 15

struct apple80211req {
    char if_name[IFNAMSIZ];
    int type;
    int val;
    uint32_t len;
    void *data;
};

// Ventura's layout: asr_ie_data is an inline 1024-byte array at 0x8c.
static const uint32_t kIeOffset = 0x8c;

static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(int argc, char **argv)
{
    const char *name = argc > 1 ? argv[1] : "en1";
    int wait = argc > 2 ? atoi(argv[2]) : 12;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    // The scan hint structure only carries dwell/SSID filters; an all-zero block
    // means "sweep the whole plan", which is what the driver does anyway.
    uint8_t hint[64];
    memset(hint, 0, sizeof hint);
    struct apple80211req req;
    memset(&req, 0, sizeof req);
    strlcpy(req.if_name, name, sizeof req.if_name);
    req.type = APPLE80211_IOC_SCAN_REQ;
    req.val = 0;
    req.len = sizeof hint;
    req.data = hint;
    if (ioctl(fd, SIOCSA80211, &req) < 0) {
        printf("SCAN_REQ failed: %s\n", strerror(errno));
        return 1;
    }
    printf("SCAN_REQ accepted on %s, waiting %ds\n", name, wait);
    sleep(wait);

    uint8_t buf[2048];
    int count = 0, withRsn = 0;
    for (int i = 0; i < 256; ++i) {
        memset(buf, 0, sizeof buf);
        struct apple80211req get;
        memset(&get, 0, sizeof get);
        strlcpy(get.if_name, name, sizeof get.if_name);
        get.type = APPLE80211_IOC_SCAN_RESULT;
        get.val = 0;
        get.len = sizeof buf; // 1164 = the Ventura struct; 2048 is just room
        get.data = buf;
        if (ioctl(fd, SIOCGA80211, &get) < 0) {
            printf("SCAN_RESULT end after %d results (%s)\n", count, strerror(errno));
            break;
        }
        ++count;
        const uint8_t *bssid = buf + 0x1c;
        const uint8_t ssidLen = buf[0x60];
        const uint8_t *ssid = buf + 0x61;
        const int16_t ieLen = (int16_t)rd16(buf + 0x8a);
        char printable[33];
        uint32_t n = ssidLen > APPLE80211_MAX_SSID_LEN ? APPLE80211_MAX_SSID_LEN : ssidLen;
        for (uint32_t k = 0; k < n; ++k)
            printable[k] = (ssid[k] >= 32 && ssid[k] < 127) ? (char)ssid[k] : '.';
        printable[n] = 0;
        if (ieLen > 0) ++withRsn;
        printf("%2d ssid='%s' len=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x ch=%u flags=0x%x "
               "rssi=%d noise=%d snr=%d cap=0x%x rates=%u ie=%d%s\n",
               count, printable, ssidLen, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4],
               bssid[5], rd32(buf + 8), rd32(buf + 12), (int)(int16_t)rd16(buf + 0x16),
               (int)(int16_t)rd16(buf + 0x12), (int)(int16_t)rd16(buf + 0x14),
               rd16(buf + 0x1a), buf[0x22], ieLen, ieLen > 0 ? " (inline-rsn)" : "");
        (void)kIeOffset;
    }
    printf("total=%d with-rsn=%d\n", count, withRsn);
    return 0;
}
