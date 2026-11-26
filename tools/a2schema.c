/* Discover which keys Apple80211Associate2 expects and in what type, by trying
   one key at a time (fork per attempt).  Password from stdin. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>

typedef int (*open_t)(void **);
typedef int (*bind_t)(void *, CFStringRef);
typedef int (*assoc2_t)(void *, CFDictionaryRef, CFStringRef, void *);

static void *H; static open_t O; static bind_t B; static assoc2_t A2;
static char gSSID[64], gPass[128];

static CFStringRef S(const char *s) { return CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8); }
static CFNumberRef N(int v) { return CFNumberCreate(NULL, kCFNumberIntType, &v); }
static CFDataRef D(const void *p, unsigned n) { return CFDataCreate(NULL, p, n); }

static const char *keys[] = {
    "SSID", "SSID_STR", "BSSID", "CHANNEL", "RSN_IE", "passphrase", "password",
    "security", "SECURITY", NULL
};
/* 0 = string, 1 = 6-byte data, 2 = int, 3 = data(rsn), 4 = data(ssid) */
static const int kinds[] = { 4, 0, 1, 2, 3, 0, 0, 2, 2 };

static void add(CFMutableDictionaryRef d, const char *k, int kind)
{
    unsigned char bssid[6] = {0x02,0,0,0,0,1};
    unsigned char rsn[34]; memset(rsn, 0, sizeof rsn);
    /* 30 14 01 00 00 0f ac 04 01 00 00 0f ac 04 01 00 00 0f ac 02 00 00 */
    const unsigned char rsnie[] = {0x30,0x14,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,
                                   0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x02,0x00,0x00};
    memcpy(rsn, rsnie, sizeof rsnie);
    switch (kind) {
    case 0: CFDictionarySetValue(d, S(k), S(!strcmp(k,"passphrase")||!strcmp(k,"password") ? gPass : gSSID)); break;
    case 1: CFDictionarySetValue(d, S(k), D(bssid, 6)); break;
    case 2: CFDictionarySetValue(d, S(k), N(!strcmp(k,"CHANNEL") ? 6 : 4)); break;
    case 3: CFDictionarySetValue(d, S(k), D(rsn, 22)); break;
    case 4: CFDictionarySetValue(d, S(k), D(gSSID, strlen(gSSID))); break;
    }
}

static int attempt(int mode, int keyidx)
{
    void *ref = 0;
    if (O(&ref) || !ref) { printf("open failed\n"); return -1; }
    if (B(ref, S("en1"))) { printf("bind failed\n"); return -1; }
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (mode == 0) {            /* single key */
        add(d, keys[keyidx], kinds[keyidx]);
    } else {                    /* everything, using discovered kinds */
        for (int i = 0; keys[i]; i++) add(d, keys[i], kinds[i]);
    }
    errno = 0;
    int rc = A2(ref, d, S(gPass), NULL);
    printf("mode=%d key=%-12s rc=%d (0x%08x) errno=%d\n", mode,
           mode == 0 ? keys[keyidx] : "(all)", rc, (unsigned)rc, errno);
    return rc;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(gSSID, sizeof gSSID, "%s", argc > 1 ? argv[1] : "QCA-TEST");
    if (!fgets(gPass, sizeof gPass, stdin)) gPass[0] = 0;
    gPass[strcspn(gPass, "\r\n")] = 0;
    H = dlopen("/System/Library/PrivateFrameworks/IO80211.framework/Versions/A/IO80211", RTLD_NOW);
    if (!H) { printf("dlopen: %s\n", dlerror()); return 1; }
    O = (open_t)dlsym(H, "Apple80211Open");
    B = (bind_t)dlsym(H, "Apple80211BindToInterface");
    A2 = (assoc2_t)dlsym(H, "Apple80211Associate2");
    if (!O || !B || !A2) { printf("symbols missing\n"); return 1; }

    for (int k = 0; keys[k]; k++) {
        pid_t p = fork();
        if (p == 0) { attempt(0, k); _exit(0); }
        int st = 0; waitpid(p, &st, 0);
        if (WIFSIGNALED(st)) printf("mode=0 key=%-12s CRASH sig=%d\n", keys[k], WTERMSIG(st));
    }
    pid_t p = fork();
    if (p == 0) { attempt(1, 0); _exit(0); }
    int st = 0; waitpid(p, &st, 0);
    if (WIFSIGNALED(st)) printf("mode=1 all CRASH sig=%d\n", WTERMSIG(st));
    return 0;
}
