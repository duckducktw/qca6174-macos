// Issue one AthControl request to Ath10kPCIDevice from userspace.
// usage: ctlrestart [status|restart]
// Compile in the guest: cc -o /tmp/ctlrestart ctlrestart.c -framework IOKit -framework CoreFoundation
#include <stdio.h>
#include <string.h>
#include <IOKit/IOKitLib.h>

enum { kStatus = 0, kRestart = 14, kClientType = 0xa7100057u, kVersion = 1 };
struct Request { uint32_t version, operation, index, length; uint8_t data[512]; };
struct Response { uint32_t version, stage, phase, flags, count, length; uint8_t local[6], bssid[6]; char error[96]; uint8_t data[512]; };

int main(int argc, char **argv) {
    const char *what = argc > 1 ? argv[1] : "status";
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("Ath10kPCIDevice"));
    if (!svc) { printf("no Ath10kPCIDevice service\n"); return 1; }
    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), kClientType, &conn);
    if (kr) { printf("IOServiceOpen failed 0x%x\n", kr); IOObjectRelease(svc); return 2; }
    struct Request req; struct Response resp;
    memset(&req, 0, sizeof(req)); memset(&resp, 0, sizeof(resp));
    req.version = kVersion;
    req.operation = strcmp(what, "restart") == 0 ? kRestart : kStatus;
    size_t outSize = sizeof(resp);
    kr = IOConnectCallStructMethod(conn, 0, &req, sizeof(req), &resp, &outSize);
    printf("op=%s kr=0x%x stage=%u phase=%u flags=0x%x count=%u error=%s\n",
           what, kr, resp.stage, resp.phase, resp.flags, resp.count, resp.error);
    if (resp.length >= 112) {
        printf("info width=%u nss=%u chan=%u rssi=%d statebits=0x%x\n",
               *(uint32_t *)(resp.data + 16), *(uint32_t *)(resp.data + 20),
               *(uint32_t *)(resp.data + 8), (int)*(uint32_t *)(resp.data + 12),
               *(uint32_t *)(resp.data + 28));
        printf("ssid='%.*s'\n", (int)resp.data[32], (const char *)(resp.data + 33));
    }
    IOServiceClose(conn);
    IOObjectRelease(svc);
    return kr == 0 ? 0 : 3;
}
