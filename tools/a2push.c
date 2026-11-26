/* Push ASSOCIATE(20) through the *Apple80211 ref's own fd* — the channel that
   Apple80211Associate2 uses internally (ref+0x00 = socket fd, ref+0x04 = "en1").
   If this connects, the kernel side of "click to join" is fine and the block is
   purely airportd's userspace preconditions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }

#define AD_VERSION     0
#define AD_AUTH_LOWER  6
#define AD_AUTH_UPPER  8
#define AD_SSID_LEN    12
#define AD_SSID        16
#define AD_KEY_LEN     0x3c
#define AD_KEY_BYTES   0x48
#define AUTHTYPE_WPA2_PSK 8

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *ssid = argc > 1 ? argv[1] : "QCA-TEST";
    char pass[128] = {0};
    if (!fgets(pass, sizeof pass, stdin)) pass[0] = 0;
    pass[strcspn(pass, "\r\n")] = 0;
    const int open_net = (pass[0] == 0);

    void *h = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    open_t O = (open_t)dlsym(h, "Apple80211Open");
    bind_t B = (bind_t)dlsym(h, "Apple80211BindToInterface");
    void *ref = 0;
    if (O(&ref) || !ref) { printf("open failed\n"); return 1; }
    if (B(ref, S("en1"))) { printf("bind failed\n"); return 1; }
    int fd = *(int *)ref;
    printf("ref=%p fd=%d ifname=%.4s flags(+0x50)=0x%08x\n", ref, fd,
           (char *)ref + 4, *(unsigned int *)((char *)ref + 0x50));

    static unsigned char ad[0x250];
    memset(ad, 0, sizeof ad);
    *(unsigned int *)(ad + AD_VERSION) = 1;
    *(unsigned int *)(ad + AD_AUTH_LOWER) = open_net ? 1 : 0;   /* open / unset */
    *(unsigned int *)(ad + AD_AUTH_UPPER) = open_net ? 0 : AUTHTYPE_WPA2_PSK;
    *(unsigned int *)(ad + AD_SSID_LEN) = (unsigned int)strlen(ssid);
    memcpy(ad + AD_SSID, ssid, strlen(ssid));
    if (!open_net) {
        *(unsigned int *)(ad + AD_KEY_LEN) = (unsigned int)strlen(pass);
        memcpy(ad + AD_KEY_BYTES, pass, strlen(pass));
    }

    static unsigned char req[64];
    memset(req, 0, sizeof req);
    memcpy(req, "en1", 4);
    *(int *)(req + 16) = 20;                 /* APPLE80211_IOC_ASSOCIATE */
    *(int *)(req + 20) = 0;
    *(unsigned int *)(req + 24) = 0x250;     /* sizeof apple80211_assoc_data */
    *(void **)(req + 32) = ad;

    errno = 0;
    int rc = ioctl(fd, 0x802869c8UL, req);
    printf("ioctl(set 20) rc=%d errno=%d (%s)  security=%s\n", rc, errno, strerror(errno),
           open_net ? "open" : "wpa2-psk");
    return 0;
}
