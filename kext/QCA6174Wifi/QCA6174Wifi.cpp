/* SPDX-License-Identifier: ISC */
#include "QCA6174Wifi.hpp"

/* Shared, host-tested logic. The SAME bounds-check code is unit-tested on
 * Linux (see tests/test_regs.c) and compiled into the kext here. */
extern "C" {
#include "../../src/common/qca6174_regs.h"
#include "../../src/common/hw_params.h"
}

#define super IOService
OSDefineMetaClassAndStructors(QCA6174Wifi, IOService)

/* Quiet by default; gated on the -qca6174debug boot-arg. */
#define QLOG(fmt, ...)                                                     \
    do { if (fDebug) IOLog("QCA6174Wifi: " fmt "\n", ##__VA_ARGS__); } while (0)
/* Errors are always logged (rare, and useful for a failed probe). */
#define QERR(fmt, ...) IOLog("QCA6174Wifi: " fmt "\n", ##__VA_ARGS__)

bool QCA6174Wifi::init(OSDictionary *dict)
{
    if (!super::init(dict))
        return false;

    int flag = 0;
    if (PE_parse_boot_argn("-qca6174debug", &flag, sizeof(flag)))
        fDebug = true;

    fPci = nullptr;
    fBar0Map = nullptr;
    fBar0 = nullptr;
    fBar0Len = 0;
    return true;
}

void QCA6174Wifi::free(void)
{
    cleanup();
    super::free();
}

IOService *QCA6174Wifi::probe(IOService *provider, SInt32 *score)
{
    IOService *res = super::probe(provider, score);

    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) {
        QERR("probe: provider is not an IOPCIDevice");
        return nullptr;
    }

    // Defensive re-check of the identity even though the personality already
    // constrains matching to 168c:003e.
    uint16_t vendor = pci->configRead16(kIOPCIConfigVendorID);
    uint16_t device = pci->configRead16(kIOPCIConfigDeviceID);
    if (vendor != QCA6174_PCI_VENDOR_ID || device != QCA6174_PCI_DEVICE_ID) {
        QLOG("probe: id %04x:%04x is not QCA6174, declining", vendor, device);
        return nullptr;
    }

    QLOG("probe: matched QCA6174 %04x:%04x", vendor, device);
    return res; // normal score; we do not inflate priority
}

bool QCA6174Wifi::start(IOService *provider)
{
    if (!super::start(provider)) {
        QERR("start: super::start failed");
        return false;
    }

    fPci = OSDynamicCast(IOPCIDevice, provider);
    if (!fPci) {
        QERR("start: provider is not an IOPCIDevice");
        return false;
    }
    fPci->retain();

    // Report identity (read-only PCI config space; always available).
    uint16_t vendor    = fPci->configRead16(kIOPCIConfigVendorID);
    uint16_t device    = fPci->configRead16(kIOPCIConfigDeviceID);
    uint8_t  revision  = fPci->configRead8(kIOPCIConfigRevisionID);
    uint16_t subVendor = fPci->configRead16(kIOPCIConfigSubSystemVendorID);
    uint16_t subDevice = fPci->configRead16(kIOPCIConfigSubSystemID);
    QLOG("start: %04x:%04x rev %02x subsys %04x:%04x",
         vendor, device, revision, subVendor, subDevice);

    if (vendor != QCA6174_PCI_VENDOR_ID || device != QCA6174_PCI_DEVICE_ID) {
        QERR("start: unexpected id %04x:%04x, aborting", vendor, device);
        cleanup();
        return false;
    }

    // Enable memory space + bus mastering.
    fPci->setMemoryEnable(true);
    fPci->setBusMasterEnable(true);

    // Map BAR0 (the register window).
    fBar0Map = fPci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!fBar0Map) {
        QERR("start: failed to map BAR0");
        cleanup();
        return false;
    }
    fBar0    = reinterpret_cast<volatile void *>(fBar0Map->getVirtualAddress());
    fBar0Len = fBar0Map->getLength();
    if (!fBar0 || fBar0Len == 0) {
        QERR("start: BAR0 mapping has no virtual address / zero length");
        cleanup();
        return false;
    }
    QLOG("start: BAR0 mapped, length 0x%llx", (unsigned long long)fBar0Len);

    // The register window we intend to touch must fit inside the BAR.
    if (!qca6174_bar_window_ok(fBar0Len)) {
        QERR("start: BAR0 (0x%llx) smaller than register window 0x%x",
             (unsigned long long)fBar0Len, QCA6174_REG_WINDOW_SIZE);
        cleanup();
        return false;
    }

    // M0 deliberately stops before raw MMIO. Linux ath10k confirms the
    // QCA6174 register-table value is 0xf0, but we have not yet independently
    // verified that a direct BAR0 read is the correct/safe macOS access path
    // before SOC wake. Enabling an unproven kernel read is not acceptable.
    QLOG("start: PCI identity and BAR0 mapping verified; raw chip-id read disabled pending access-path validation");

    // M0 ENDS HERE. We deliberately publish NO network interface and make NO
    // L2/L3 claim. Registering the service merely advertises that the probe
    // attached cleanly so ioreg/diagnostics can see us.
    registerService();
    QLOG("start: M0 probe complete (no Wi-Fi interface published)");
    return true;
}

void QCA6174Wifi::stop(IOService *provider)
{
    QLOG("stop");
    cleanup();
    super::stop(provider);
}

void QCA6174Wifi::cleanup(void)
{
    fBar0 = nullptr;
    fBar0Len = 0;
    if (fBar0Map) {
        fBar0Map->release();
        fBar0Map = nullptr;
    }
    if (fPci) {
        // Leave memory/bus-master as the system default on detach; drop ref.
        fPci->release();
        fPci = nullptr;
    }
}

bool QCA6174Wifi::read32(uint32_t offset, uint32_t *out)
{
    if (!out || !fBar0)
        return false;
    if (!qca6174_mmio_access_ok(fBar0Len, offset, 4))
        return false;
    volatile uint32_t *p =
        reinterpret_cast<volatile uint32_t *>(
            reinterpret_cast<volatile uint8_t *>(fBar0) + offset);
    *out = OSReadLittleInt32(p, 0);
    return true;
}
