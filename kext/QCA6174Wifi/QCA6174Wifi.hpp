/* SPDX-License-Identifier: ISC */
/*
 * QCA6174Wifi - M0 IOKit probe skeleton for Qualcomm Atheros QCA6174.
 *
 * SCOPE (M0 only): this class attaches to the PCI function, enables memory +
 * bus-master, maps BAR0, and performs READ-ONLY identification. It does NOT:
 *   - register any network / IO80211 interface
 *   - claim L2/L3 connectivity
 *   - run firmware, BMI, HTC, WMI or HTT
 * Those are explicitly out of scope and tracked in docs/STATUS.md.
 *
 * NOTE: this file targets the macOS Kernel framework and can only be built
 * with Xcode + the macOS SDK. It is NOT compiled by the host Makefile (Linux
 * has no macOS kernel SDK). See docs/BUILD.md.
 */
#ifndef QCA6174WIFI_HPP
#define QCA6174WIFI_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>

class QCA6174Wifi : public IOService {
    OSDeclareDefaultStructors(QCA6174Wifi)

public:
    virtual bool init(OSDictionary *dict) override;
    virtual void free(void) override;

    virtual IOService *probe(IOService *provider, SInt32 *score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;

private:
    IOPCIDevice   *fPci     = nullptr;   // provider, retained
    IOMemoryMap   *fBar0Map = nullptr;   // BAR0 mapping, retained
    volatile void *fBar0    = nullptr;   // mapped virtual base
    uint64_t       fBar0Len = 0;         // mapping length in bytes
    bool           fDebug   = false;     // -qca6174debug boot-arg

    // Tear down any acquired resources. Safe to call multiple times.
    void cleanup(void);

    // Bounds-checked MMIO reads (read-only in M0). Return false and leave
    // *out untouched if the access is out of range / misaligned.
    bool read32(uint32_t offset, uint32_t *out);
};

#endif /* QCA6174WIFI_HPP */
