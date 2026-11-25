// Ventura (macOS 13) IO80211 v1 controller glue.
//
// Implemented here: interface publication, hardware address, enable/disable,
// transmit path, power/state reporting and the apple80211 request dispatcher
// that airportd uses for everything the Wi-Fi UI shows.
#include "Ath10kVenturaABI.hpp"
#include <net/kpi_interface.h>
#include <net/if_dl.h>

#ifdef ATH10K_VENTURA

// Live-family only: the step that hands an ifnet to the 80211 core.
extern "C" IOReturn ath10kPrepareVifBsdInterface(IO80211VirtualInterface *, ifnet_t, UInt32)
    __asm("__ZN23IO80211VirtualInterface19prepareBSDInterfaceEP7__ifnetj");

#include "Ath10kVenturaInterface.hpp"
#include "../Ath10kPCIDevice.hpp"
#include "../../shared/Ath10kSupplicant.hpp"
#include "../native/Ath10kKernelCrypto.hpp"
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <sys/_types/_size_t.h>
#include <net/if_media.h>

// ---------------------------------------------------------------------------
// Interface publication
// ---------------------------------------------------------------------------

// The interface createInterface() handed the family. Kept in a file-static as
// well as in the member: the member reads back null from other paths, most
// likely because our ivars sit inside the live class's ivar block, and every
// consumer below needs a pointer that survives that.
static IO80211Interface *gVenturaPublishedInterface = nullptr;
static UInt32 gVenturaCreateCalls = 0;
// IO80211ScanManager plumbing (built in setupVenturaScanManager, further down):
// declared up here because apple80211SkywalkRequest() must already know about the
// manager's pointer-out result convention.
static OSObject *gVenturaScanManager = nullptr;
static apple80211_scan_result *gVenturaScanSlot = nullptr;

// apple80211_scan_result as airportd sees it on 13.x: packed, with asr_ie_data
// inline. The 13.7 SDK header we build against still describes the pointer-era
// layout, so the offsets are asserted rather than trusted: version 0x00,
// asr_channel 0x04, asr_rates 0x24, asr_ssid 0x61, asr_age 0x84, asr_ie_len
// 0x8a, then 1024 bytes of IE data at 0x8c (no asr_unk3 field after 12.0).
#define ATH10K_SCAN_IE_OFFSET 0x8cu
static UInt32 venturaScanChannelFlags(UInt32 channel);
// Declared up here because the ioctl entry below uses them; both are live
// IO80211Controller exports and return 0 on success.
extern "C" int proc_selfpid(void);
extern "C" int proc_selfname(char *buf, size_t len);
extern "C" int ath10kControllerCopyIn(IO80211Controller *self, UInt64 address, void *to,
                                      size_t bytes)
    __asm("__ZN17IO80211Controller6copyInEyPvm");
extern "C" int ath10kControllerCopyOut(IO80211Controller *self, const void *from, UInt64 address,
                                       size_t bytes)
    __asm("__ZN17IO80211Controller7copyOutEPKvym");
static_assert(__builtin_offsetof(apple80211_scan_result, asr_ie_len) == 0x8a,
              "apple80211_scan_result: asr_ie_len moved");
static_assert(__builtin_offsetof(apple80211_scan_result, asr_ssid) == 0x61,
              "apple80211_scan_result: asr_ssid moved");
static_assert(__builtin_offsetof(apple80211_scan_result, asr_rates) == 0x24,
              "apple80211_scan_result: asr_rates moved");

IONetworkInterface *Ath10kPCIDevice::createInterface()
{
    Ath10kVenturaInterface *interface = new Ath10kVenturaInterface;
    if (interface == nullptr)
        return nullptr;
    if (!interface->init(this, this)) {
        interface->release();
        return nullptr;
    }
    // SystemConfiguration classifies the interface from the IOInterfaceType
    // property, so this object must keep IFT_ETHER all the way through: SC drops
    // any other value ("unknown interface type"), while the Wi-Fi class comes
    // from the controller conforming to IO80211Controller. Read the whole story
    // in HANDOFF §6.9.1 before touching anything here.
    _venturaInterface = interface;
    gVenturaPublishedInterface = interface;
    ++gVenturaCreateCalls;
    if (IOService *owner = getProvider()) {
        owner->setProperty("Ath10kCreateIfaceCalls",
                           static_cast<UInt64>(gVenturaCreateCalls), 32);
        owner->setProperty("Ath10kCreateIfacePtr",
                           static_cast<UInt64>(reinterpret_cast<uintptr_t>(interface)), 64);
        // Which Ath10kPCIDevice instance ran this? Compared against the same
        // value from the delayed retry and from start() to tell "two controller
        // instances" apart from "the file-static is not shared".
        owner->setProperty("Ath10kInstanceCreate",
                           static_cast<UInt64>(reinterpret_cast<uintptr_t>(this)), 64);
    }
    return interface;
}


// ---------------------------------------------------------------------------
// Controller-mandated virtuals
// ---------------------------------------------------------------------------

void Ath10kPCIDevice::_RESERVEDIO80211Controller0() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller1() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller2() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller3() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller4() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller5() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller6() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller7() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller8() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller9() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller10() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller11() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller12() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller13() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller14() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller15() {}

static void venturaPhase(IOService *p, const char *phase)
{
    if (p) p->setProperty("Ath10kVenturaLogPhase", phase);
}

void Ath10kPCIDevice::getLogPipes(CCPipe **driver, CCPipe **datapath, CCPipe **snapshot)
{
    // Hand the family our corecapture pipes: findAndAttachToFaultReporter()
    // asks for them, and leaves ivars->_faultReporter null (panic in logDebug)
    // when the answer is empty. It runs inside IO80211Controller::start(), so
    // build the chain here if start() has not done it yet.
    if (!_venturaPipesStarted[0] && !_venturaCoreFault) {
        if (!getWorkLoop() && !createWorkLoop()) {
            if (IOService *p = getProvider()) p->setProperty("Ath10kFaultSlotPhase", "no-workloop");
            return;
        }
        if (!prepareVenturaLogs()) {
            if (IOService *p = getProvider()) p->setProperty("Ath10kFaultSlotPhase", "logs-failed");
            return;
        }
    }
    if (driver) *driver = _venturaPipes[0];
    if (datapath) *datapath = _venturaPipes[1];
    if (snapshot) *snapshot = _venturaPipes[2];
    if (IOService *p = getProvider()) p->setProperty("Ath10kFaultSlotPhase", "pipes");
}

void *Ath10kPCIDevice::getFaultReporterFromDriver()
{
    if (_venturaCoreFault == nullptr) {
        if (!getWorkLoop() && !createWorkLoop()) {
            if (IOService *p = getProvider()) p->setProperty("Ath10kFaultSlotPhase", "no-workloop");
            return nullptr;
        }
        if (!prepareVenturaLogs()) {
            if (IOService *p = getProvider()) p->setProperty("Ath10kFaultSlotPhase", "logs-failed");
            return nullptr;
        }
    }
    if (IOService *p = getProvider()) p->setProperty("Ath10kFaultSlotPhase", "reporter");
    return _venturaCoreFault;
}

// ---------------------------------------------------------------------------
// Vtable alignment with the loaded IO80211Family generation
// ---------------------------------------------------------------------------
// The declarations in vendor/itlwm-v1/Airport/IO80211Controller.h now follow
// the LIVE com.apple.iokit.IO80211Family slot order (13.7.8), so getLogPipes,
// getFaultReporterFromDriver, apple80211VirtualRequest and
// apple80211SkywalkRequest are overrides on the slots the kernel actually
// dispatches. This routine only reports where the object's vptr points and
// whether the four slots carry our code, so a header regression shows up as a
// breadcrumb instead of as a panic inside framework code.

bool Ath10kPCIDevice::venturaMediumDictionaryKeys(char *out, size_t outLen)
{
    if (out == nullptr || outLen == 0)
        return false;
    out[0] = '\0';
    OSDictionary *dict = copyMediumDictionary();
    if (dict == nullptr) {
        strlcpy(out, "none", outLen);
        return false;
    }
    OSCollectionIterator *it = OSCollectionIterator::withCollection(dict);
    if (it != nullptr) {
        size_t used = 0;
        while (const OSSymbol *key = OSDynamicCast(OSSymbol, it->getNextObject())) {
            const char *name = key->getCStringNoCopy();
            if (name == nullptr) continue;
            size_t len = strlen(name);
            if (used + len + 2 >= outLen) break;
            if (used) out[used++] = ',';
            memcpy(out + used, name, len);
            used += len;
            out[used] = '\0';
        }
        it->release();
    }
    dict->release();
    const IONetworkMedium *current = getCurrentMedium();
    if (current != nullptr && outLen > 20) {
        char tail[32];
        snprintf(tail, sizeof(tail), "|cur=%08x", (unsigned)current->getType());
        strlcat(out, tail, outLen);
    }
    return true;
}

bool Ath10kPCIDevice::createVenturaMediumTables(const IONetworkMedium **primary, bool forcePublish)
{
    if (_venturaMedium != nullptr && !forcePublish) {
        if (primary != nullptr) *primary = _venturaMedium;
        return true;
    }
    // Apple80211GetIfListCopy() reads SIOCGIFMEDIA and only accepts an answer
    // that advertises the 802.11 medium, which IONetworkController derives from
    // this dictionary. The default IOEthernetController medium is
    // "autoselect (IFM_ETHER)" and airportd then reports no Wi-Fi interface at
    // all, whatever the ifnet type or IOInterfaceType property says.
    OSDictionary *mediumDict = OSDictionary::withCapacity(1);
    if (mediumDict == nullptr)
        return false;
    IONetworkMedium *medium = IONetworkMedium::medium(kIOMediumIEEE80211Auto, 54000000);
    if (medium == nullptr) {
        mediumDict->release();
        return false;
    }
    IONetworkMedium::addMedium(mediumDict, medium);
    const bool published = publishMediumDictionary(mediumDict);
    mediumDict->release();
    if (!published) {
        medium->release();
        return false;
    }
    medium->retain();
    _venturaMedium = medium;
    if (primary != nullptr) *primary = _venturaMedium;
    return true;
}

bool Ath10kPCIDevice::alignVenturaVtable()
{
    void **vtable = *reinterpret_cast<void ***>(this);
    IOService *probe = getProvider();
    if (vtable == nullptr)
        return false;
    if (probe) {
        probe->setProperty("Ath10kAlignPhase", "abi-native");
        probe->setProperty("Ath10kVptr", static_cast<UInt64>(reinterpret_cast<uintptr_t>(vtable)), 64);
        probe->setProperty("Ath10kVtLogPipes", static_cast<UInt64>(reinterpret_cast<uintptr_t>(vtable[0xd10 / sizeof(void *)])), 64);
        probe->setProperty("Ath10kVtFaultReporter", static_cast<UInt64>(reinterpret_cast<uintptr_t>(vtable[0xd48 / sizeof(void *)])), 64);
        probe->setProperty("Ath10kVtVirtualRequest", static_cast<UInt64>(reinterpret_cast<uintptr_t>(vtable[0xca8 / sizeof(void *)])), 64);
    }
    return true;
}

bool Ath10kPCIDevice::prepareVenturaLogs()
{
    IOService *owner = getProvider();
    if (!getWorkLoop()) { venturaPhase(owner, "no-workloop"); return false; }
    venturaPhase(owner, "begin");
    static const char *names[] = {"DriverLogs", "DatapathEvents", "StateSnapshots"};
    for (unsigned i = 0; i < 3; ++i) {
        CCPipeOptions options{};
        if (i == 2) {
            options.pipe_type = 0x200000001ULL;
            options.log_data_type = 2;
            options.pipe_size = 128;
        } else {
            options.pipe_type = 0;
            options.log_data_type = (i == 0) ? 1 : 0;
            options.pipe_size = 0x200000;
            options.min_log_size_notify = 0xccccc;
            options.notify_threshold = 1000;
            options.pad9 = (i == 0) ? 0x1000000 : 2;
            options.pad10 = (i == 0) ? 2 : 0x2800000;
        }
        strlcpy(options.file_name, names[i], sizeof(options.file_name));
        if (i != 1) strlcpy(options.name, (i == 0) ? "wlan0" : "0", sizeof(options.name));
        strlcpy(options.directory_name, "WiFi", sizeof(options.directory_name));
        _venturaPipes[i] = CCPipe::withOwnerNameCapacity(this, "br.com.jonasw8.driver.ath10kmac",
                                                         names[i], &options);
        if (!_venturaPipes[i]) { venturaPhase(owner, "pipe-create-failed"); return false; }
        if (!(_venturaPipesStarted[i] = _venturaPipes[i]->CCPipe::startPipe())) {
            venturaPhase(owner, "pipe-start-failed"); return false;
        }
        if (i == 0) {
            CCStreamOptions loggerOptions{};
            CCStream *logger = CCStream::withPipeAndName(_venturaPipes[0], "Ath10kMac", &loggerOptions);
            _venturaLogger = OSDynamicCast(CCLogStream, logger);
            if (!_venturaLogger) { OSSafeReleaseNULL(logger); venturaPhase(owner, "logstream-failed"); return false; }
        }
    }
    CCStreamOptions faultOptions{};
    faultOptions.stream_type = 1;
    faultOptions.console_level = ~0ULL;
    _venturaFaultStream = CCStream::withPipeAndName(_venturaPipes[2], "FaultReporter", &faultOptions);
    auto data = OSDynamicCast(CCDataStream, _venturaFaultStream);
    if (!data) { venturaPhase(owner, "fault-stream-failed"); return false; }
    // Ventura ships CCFaultReporter but not IO80211FaultReporter (that class
    // arrived with the newer IO80211 family). Importing it made OpenCore abort
    // the injection, so stop at the CCFaultReporter the 13.x kernel exports.
    _venturaCoreFault = CCFaultReporter::withStreamWorkloop(data, getWorkLoop());
    if (!_venturaCoreFault) { venturaPhase(owner, "core-fault-failed"); return false; }
    venturaPhase(owner, "ready");
    return true;
}

void Ath10kPCIDevice::releaseVenturaLogs()
{
    OSSafeReleaseNULL(_venturaCoreFault);
    OSSafeReleaseNULL(_venturaFaultStream);
    OSSafeReleaseNULL(_venturaLogger);
    for (unsigned i = 0; i < 3; ++i) {
        if (_venturaPipesStarted[i] && _venturaPipes[i]) _venturaPipes[i]->CCPipe::stopPipe();
        _venturaPipesStarted[i] = false;
        OSSafeReleaseNULL(_venturaPipes[i]);
    }
}

// Skywalk-era overloads: the 10.15+ class declares them virtual, and OpenCore
// refuses to inject a kext whose vtable leaves those slots unresolved.
IOReturn Ath10kPCIDevice::enable(IO80211SkywalkInterface *)
{
    return kIOReturnSuccess;
}

IOReturn Ath10kPCIDevice::disable(IO80211SkywalkInterface *)
{
    return kIOReturnSuccess;
}

UInt32 Ath10kPCIDevice::hardwareOutputQueueDepth()
{
    return 1024;
}

// IO80211Controller stores its IO80211CommandGate on the queue this returns and
// IO80211VirtualInterface::init() dereferences it through
// apple80211VirtualRequestIoctl(), so a null answer faults inside the family.
// IO80211WorkQueue::workQueue() is the shared instance Apple's own drivers use.
bool Ath10kPCIDevice::createWorkQueue()
{
    if (_venturaWorkQueue == nullptr)
        _venturaWorkQueue = IO80211WorkQueue::workQueue();
    return _venturaWorkQueue != nullptr;
}

IO80211WorkQueue *Ath10kPCIDevice::getWorkQueue() const
{
    return _venturaWorkQueue;
}

// ---------------------------------------------------------------------------
// Vif ifnet ownership
// ---------------------------------------------------------------------------
// Every route that tried to borrow a published ifnet is dead on this build:
// getIfnet() on our interface object returns null (Apple's attach re-creates the
// object), the controller has no parked interface, the interface is not a
// registry child of the controller, and ifnet_find_by_name()/ifnet_list_get()
// answer ENXIO even for en0. So own the ifnet the way net80211 drivers do:
// allocate it with IFT_IEEE80211 from birth, attach it ourselves, then hand the
// pointer to the vif's prepareBSDInterface(). From that point the 80211 core has
// an ifnet to publish instead of only an Ethernet-shaped unit it never
// registered.

static const u_int32_t kAth10kIfTypeIeee80211 = 71;   // IFT_IEEE80211

// The datapath is not wired yet: drop what the stack hands us and let it free
// the mbuf. Returning 0 means "consumed".
static errno_t venturaVifOutput(ifnet_t, mbuf_t)
{
    return 0;
}

static errno_t venturaVifDemux(ifnet_t, mbuf_t, char *, protocol_family_t *)
{
    return EJUSTRETURN;
}

static errno_t venturaVifAddProto(ifnet_t, protocol_family_t,
                                  const struct ifnet_demux_desc *, u_int32_t)
{
    return 0;
}

static errno_t venturaVifDelProto(ifnet_t, protocol_family_t)
{
    return 0;
}

static errno_t venturaVifCheckMulti(ifnet_t, const struct sockaddr *)
{
    return 0;
}

ifnet_t Ath10kPCIDevice::createVenturaVifIfnet(IOService *provider)
{
    if (_venturaVifIfnet != nullptr)
        return _venturaVifIfnet;

    UInt8 mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    const char *macSource = "synthetic";
    IOEthernetAddress hwAddress{};
    if (getHardwareAddress(&hwAddress) == kIOReturnSuccess) {
        memcpy(mac, &hwAddress, 6);
        macSource = "firmware";
    } else {
        bool anyByte = false;
        for (UInt32 i = 0; i < 6; ++i)
            if (_wmiInit.mac[i] != 0) anyByte = true;
        if (anyByte) {
            memcpy(mac, _wmiInit.mac, 6);
            macSource = "wmi";
        }
    }

    static const UInt8 broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    struct ifnet_init_params params;
    memset(&params, 0, sizeof(params));
    params.uniqueid = mac;
    params.uniqueid_len = 6;
    params.name = "en";
    params.unit = kAth10kVifIfnetUnit;
    params.family = IFNET_FAMILY_ETHERNET;   // dlil protocol attach stays known-good
    // IFT_ETHER, not IFT_IEEE80211: IPConfiguration's ARP client refuses any other
    // if_data.ifi_type ("arp_client_init(en1): unsupported network type"), and
    // without an ARP client it never starts the DHCP client - the interface then
    // only ever gets a manual address. airportd still sees a Wi-Fi interface
    // because that decision is made from SIOCGIFMEDIA (kIOMediumIEEE80211Auto),
    // not from this field.
    params.type = 6;                         // IFT_ETHER
    params.output = venturaVifOutput;
    params.demux = venturaVifDemux;
    params.add_proto = venturaVifAddProto;
    params.del_proto = venturaVifDelProto;
    params.check_multi = venturaVifCheckMulti;
    params.softc = this;
    params.broadcast_addr = broadcast;
    params.broadcast_len = 6;

    ifnet_t ifp = nullptr;
    const errno_t allocStatus = ifnet_allocate(&params, &ifp);
    provider->setProperty("Ath10kOwnIfnetAlloc",
                          static_cast<UInt64>(static_cast<UInt32>(allocStatus)), 32);
    if (allocStatus != 0 || ifp == nullptr) {
        provider->setProperty("Ath10kOwnIfnetPhase", "alloc-failed");
        return nullptr;
    }

    ifnet_set_lladdr(ifp, mac, 6);
    ifnet_set_mtu(ifp, 1500);
    provider->setProperty("Ath10kOwnIfnetMacSource", macSource);

    struct sockaddr_dl linkAddr;
    memset(&linkAddr, 0, sizeof(linkAddr));
    linkAddr.sdl_len = sizeof(linkAddr);
    linkAddr.sdl_family = AF_LINK;
    linkAddr.sdl_alen = 6;
    memcpy(LLADDR(&linkAddr), mac, 6);

    const errno_t attachStatus = ifnet_attach(ifp, &linkAddr);
    provider->setProperty("Ath10kOwnIfnetAttach",
                          static_cast<UInt64>(static_cast<UInt32>(attachStatus)), 32);
    if (attachStatus != 0) {
        provider->setProperty("Ath10kOwnIfnetPhase", "attach-failed");
        ifnet_release(ifp);
        return nullptr;
    }

    char unitName[16] = {0};
    snprintf(unitName, sizeof(unitName), "%s%u", ifnet_name(ifp),
             static_cast<unsigned>(ifnet_unit(ifp)));
    provider->setProperty("Ath10kOwnIfnetPhase", "attached");
    provider->setProperty("Ath10kOwnIfnetName", unitName);
    _venturaVifIfnet = ifp;     // keep the allocate reference for the driver's life
    return ifp;
}

// The last piece the legacy-class publication cannot do: register an ifnet with
// the 80211 core. On 13.7 prepareBSDInterface(ifnet, flags) lives only on the
// live family's IO80211SkywalkInterface/IO80211VirtualInterface, never on the
// plain IO80211Interface, which is why the unit exists but SystemConfiguration
// still files it as Ethernet and airportd never sends a request. The live
// implementation only sets properties, MTU (1500), IOReporters and eflags on
// the ifnet it is handed, so passing ours is safe.
IO80211Interface *Ath10kPCIDevice::venturaPublishedInterface()
{
    return gVenturaPublishedInterface;
}

bool Ath10kPCIDevice::publishVenturaVifBsdUnit(IO80211VirtualInterface *vif, IOService *provider)
{
    if (vif == nullptr)
        return false;
    if (_venturaVifBsdDone) {
        provider->setProperty("Ath10kVifBsdPhase", "already-published");
        return true;
    }
    // Own-ifnet is suspended: handing a freshly allocated ifnet to the vif's
    // prepareBSDInterface() faults inside IOSkywalkNetworkInterface at +0x1a
    // (ivars->[0], the interface extra that carries the MTU, is null for a vif
    // the family never registered with the Skywalk stack). Re-enable only after
    // the vif is registered (registerNetworkInterface) so the extra exists.
    ifnet_t ifp = nullptr;
    const char *source = "none";
    if (ifp == nullptr && _venturaInterface != nullptr) {
        ifp = _venturaInterface->getIfnet();
        if (ifp != nullptr) {
            ifnet_reference(ifp);
            source = "interface-ifnet";
        }
    }
    if (ifp == nullptr) {
        // The object createInterface() gave us is not the one Apple attached, so
        // take the interface IO80211Controller::attachInterface parked in its own
        // ivar (offset 0x200) - that is the published instance.
        IONetworkInterface *published =
            *(IONetworkInterface **)((char *)this + 0x200);
        if (published != nullptr) {
            ifnet_t candidate = published->getIfnet();
            if (candidate != nullptr) {
                ifp = candidate;
                ifnet_reference(ifp);
                source = "controller-ivar";
            }
        }
    }
    if (ifp == nullptr) {
        // The interface is not a direct child of the controller (its children are
        // the corecapture pipes and the vif), so walk both the controller and the
        // vif looking for a node that carries a "BSD Name" - reading the property
        // is what works, IONetworkInterface::getIfnet() returned null on the very
        // object that owns the unit.
        IOService *parents[2] = {this, reinterpret_cast<IOService *>(vif)};
        for (UInt32 p = 0; p < 2 && ifp == nullptr; ++p) {
            if (parents[p] == nullptr)
                continue;
            UInt32 childCount = 0;
            OSIterator *iter = parents[p]->getChildIterator(gIOServicePlane);
            while (iter != nullptr) {
                IOService *child = OSDynamicCast(IOService, iter->getNextObject());
                if (child == nullptr)
                    break;
                ++childCount;
                OSString *childName = OSDynamicCast(OSString, child->getProperty("BSD Name"));
                if (childName == nullptr)
                    continue;
                provider->setProperty("Ath10kIfaceChildClass",
                                      child->getMetaClass()->getClassName());
                if (ifnet_find_by_name(childName->getCStringNoCopy(), &ifp) == 0 && ifp != nullptr) {
                    source = p == 0 ? "controller-child" : "vif-child";
                    break;
                }
                ifp = nullptr;
            }
            if (iter) iter->release();
            if (p == 0)
                provider->setProperty("Ath10kChildCount", static_cast<UInt64>(childCount), 32);
        }
    }
    char unitName[16] = {0};
    if (ifp == nullptr) {
        // ifnet_find_by_name() returns ENXIO for every name in this kext, even
        // "en0", so the name-based routes are dead ends here. ifnet_list_get()
        // walks the attached list instead, and the unit we want is the only enN
        // whose type is IEEE80211 and whose unit is not 0.
        ifnet_t *list = nullptr;
        UInt32 listCount = 0;
        if (ifnet_list_get(IFNET_FAMILY_ANY, &list, &listCount) == 0 && list != nullptr) {
            for (UInt32 i = 0; i < listCount; ++i) {
                ifnet_t candidate = list[i];
                if (candidate == nullptr)
                    continue;
                const char *candidateName = ifnet_name(candidate);
                if (candidateName == nullptr || strncmp(candidateName, "en", 2) != 0)
                    continue;
                if (ifnet_unit(candidate) == 0)
                    continue;
                ifp = candidate;
                ifnet_reference(ifp);
                source = "ifnet-list";
                break;
            }
            ifnet_list_free(list);
        }
    }
    if (ifp == nullptr) {
        // Fallback 1: the interface does carry the unit number and the name
        // prefix (SystemConfiguration reads the same pair), so build enN.
        if (_venturaInterface != nullptr) {
            OSNumber *unit = OSDynamicCast(OSNumber, _venturaInterface->getProperty(kIOInterfaceUnit));
            OSString *prefix = OSDynamicCast(OSString,
                                             _venturaInterface->getProperty(kIOInterfaceNamePrefix));
            if (unit)
                snprintf(unitName, sizeof(unitName), "%s%u",
                         prefix ? prefix->getCStringNoCopy() : "en",
                         static_cast<unsigned>(unit->unsigned32BitValue()));
        }
        // Fallback 2: scan our own class for a published "BSD Name". Matching
        // "IO80211Interface" does not return subclass instances, so use the
        // concrete class name.
        if (unitName[0] == 0) {
            OSDictionary *match = IOService::serviceMatching("Ath10kVenturaInterface");
            OSIterator *iter = match ? IOService::getMatchingServices(match) : nullptr;
            while (iter) {
                IOService *candidate = OSDynamicCast(IOService, iter->getNextObject());
                if (candidate == nullptr)
                    break;
                OSString *name = OSDynamicCast(OSString, candidate->getProperty("BSD Name"));
                if (name == nullptr)
                    continue;
                snprintf(unitName, sizeof(unitName), "%s", name->getCStringNoCopy());
                break;
            }
            if (iter) iter->release();
            if (match) match->release();
        }
        if (unitName[0] != 0 && ifnet_find_by_name(unitName, &ifp) == 0 && ifp != nullptr)
            source = "bsd-name";
        else
            ifp = nullptr;
    }
    provider->setProperty("Ath10kVifBsdSource", source);
    if (ifp == nullptr) {
        provider->setProperty("Ath10kVifBsdPhase", "no-ifnet");
        return false;
    }
    // ifnet_allocate's reference belongs to the driver when the ifnet is ours;
    // borrowed pointers were referenced above and are dropped at every exit.
    const bool borrowed = (ifp != _venturaVifIfnet);
    snprintf(unitName, sizeof(unitName), "%s%u", ifnet_name(ifp),
             static_cast<unsigned>(ifnet_unit(ifp)));
    provider->setProperty("Ath10kVifBsdPhase", "ifnet-found");
    provider->setProperty("Ath10kVifBsdName", unitName);

    // Naming properties a published 80211 interface carries; set them before
    // prepareBSDInterface so the core publishes the unit under this name.
    IOService *target = reinterpret_cast<IOService *>(vif);
    target->setProperty("built-in", kOSBooleanTrue);
    target->setProperty("IOInterfaceName", unitName);
    target->setProperty(kIOInterfaceUnit, static_cast<UInt64>(ifnet_unit(ifp)), 32);
    target->setProperty(kIOInterfaceNamePrefix, ifnet_name(ifp));

    const IOReturn status = ath10kPrepareVifBsdInterface(vif, ifp, 0);
    provider->setProperty("Ath10kVifBsdPhase",
                          status == kIOReturnSuccess ? "prepared" : "prepare-failed");
    provider->setProperty("Ath10kVifBsdStatus",
                          static_cast<UInt64>(static_cast<UInt32>(status)), 32);
    if (status != kIOReturnSuccess) {
        if (borrowed) ifnet_release(ifp);
        return false;
    }
    // prepareBSDInterface only stores the ifnet in the vif's ivars and sets
    // MTU/eflags/reporters; the rest of the family's BSD bring-up lives in
    // configureIfnet() and attachToBpf(). Call them through the *live* vtable
    // offsets (index*8 - 0x10, because vptr points at &vtable[2]) - the
    // vendored header describes a different generation, so a named virtual call
    // could land one slot off. Measured indices on 13.7.8:
    //   284 initBSDInterfaceParameters, 285 prepareBSDInterface,
    //   286 finalizeBSDInterface, 287 getBSDInterface,
    //   455 setUnitNumber, 456 attachToBpf, 457 configureIfnet
    void **vptr = *reinterpret_cast<void ***>(vif);   // &vtable[2]
    typedef bool (*VifAttachFn)(void *);
    typedef void (*VifVoidFn)(void *);
    typedef ifnet_t (*VifIfnetFn)(void *);

    ifnet_t stored = reinterpret_cast<VifIfnetFn>(vptr[0x8e8 / 8])(vif);
    provider->setProperty("Ath10kVifIfnetStored", stored != nullptr);
    if (stored == nullptr) {
        provider->setProperty("Ath10kVifAttachPhase", "ifnet-not-stored");
        if (borrowed) ifnet_release(ifp);
        return false;
    }
    reinterpret_cast<VifVoidFn>(vptr[0xe38 / 8])(vif);   // configureIfnet()
    provider->setProperty("Ath10kVifAttachPhase", "ifnet-configured");
    const bool bpfAttached = reinterpret_cast<VifAttachFn>(vptr[0xe30 / 8])(vif);
    provider->setProperty("Ath10kVifAttachPhase",
                          bpfAttached ? "bpf-attached" : "bpf-attach-failed");
    _venturaVifBsdDone = bpfAttached;
    if (borrowed) ifnet_release(ifp);
    return bpfAttached;
}

void Ath10kPCIDevice::vifPublishCall(thread_call_param_t param0, thread_call_param_t)
{
    auto *self = static_cast<Ath10kPCIDevice *>(param0);
    if (self != nullptr)
        self->runVifPublish();
}

// Retry of the vif BSD handoff, three seconds after start(). The interface node
// that carries "BSD Name" is not a child of the controller while start() runs
// (its children then are the corecapture pipes and the vif), so the handoff can
// only find the unit once the registry has settled.
void Ath10kPCIDevice::runVifPublish()
{
    IOService *provider = getProvider();
    if (provider == nullptr)
        return;
    // Stamp the 80211 user client class on the interface we published, every time
    // this retry runs: airportd opens a user client on that node and
    // IOService::newUserClient picks the class from IOUserClientClass, which
    // IONetworkStack overwrites with its own client when it attaches the unit to
    // BSD - i.e. after our init() ran. This runs seconds later, so it is the
    // earliest point where the stamp can survive. (Do not extend the retry count
    // for this: publishVenturaVifBsdUnit() panics inside Skywalk's
    // prepareBSDInterface once the vif has been handed an ifnet.)
    if (_venturaInterface != nullptr) {
        OSString *beforeClient =
            OSDynamicCast(OSString, _venturaInterface->getProperty("IOUserClientClass"));
        provider->setProperty("Ath10kUserClientBefore",
                              beforeClient != nullptr ? beforeClient->getCStringNoCopy() : "null");
        _venturaInterface->setProperty("IOUserClientClass", "IO80211APIUserClient");
        OSString *afterClient =
            OSDynamicCast(OSString, _venturaInterface->getProperty("IOUserClientClass"));
        provider->setProperty("Ath10kUserClientAfter",
                              afterClient != nullptr ? afterClient->getCStringNoCopy() : "null");
        provider->setProperty("Ath10kUserClientAttempt",
                              static_cast<UInt64>(_vifPublishAttempts), 32);
    }
    if (_venturaVif == nullptr) {
        provider->setProperty("Ath10kVifPublishRetry", "no-vif");
        return;
    }
    const bool ok = publishVenturaVifBsdUnit(_venturaVif, provider);
    provider->setProperty("Ath10kVifPublishRetry", ok ? "published" : "failed");
    // airportd finds a device through the controller, not through the BSD unit:
    // IO80211Controller::attachInterface(IONetworkInterface **, bool) parks the
    // interface in its own ivar at +0x200, and getNetworkInterface() is just a
    // meta-cast of that ivar. If it is null airportd sees no device at all
    // (networksetup: "Wireless is not currently available on this machine",
    // no airportd user client in ioreg), even though the Wi-Fi hardware port
    // exists. Record the ivar and where our own members live, so a fix cannot
    // land on top of one of them.
    void *infraIvar = *reinterpret_cast<void **>(reinterpret_cast<char *>(this) + 0x200);
    provider->setProperty("Ath10kCtrlInfraIvar", infraIvar != nullptr);
    IO80211Interface *published = venturaPublishedInterface();
    provider->setProperty("Ath10kCtrlInfraSeen", published != nullptr);
    provider->setProperty("Ath10kInstanceRetry",
                          static_cast<UInt64>(reinterpret_cast<uintptr_t>(this)), 64);
    provider->setProperty("Ath10kStaticCallsSeen", static_cast<UInt64>(gVenturaCreateCalls), 32);
    provider->setProperty("Ath10kStaticPtrSeen",
                          static_cast<UInt64>(reinterpret_cast<uintptr_t>(gVenturaPublishedInterface)), 64);
    // Install it here (three seconds after Apple's start() returned), never in
    // createInterface(): attachInterface() skips its whole attach/register pass
    // when the ivar is already set (measured: the Wi-Fi hardware port vanishes).
    if (infraIvar == nullptr && published != nullptr) {
        published->retain();
        *reinterpret_cast<void **>(reinterpret_cast<char *>(this) + 0x200) = published;
        provider->setProperty("Ath10kCtrlInfraInstalled", "retry");
    }
    if (infraIvar != nullptr) {
        IONetworkInterface *ivarPublished = OSDynamicCast(IONetworkInterface, (OSObject *)infraIvar);
        if (ivarPublished != nullptr) {
            provider->setProperty("Ath10kCtrlInfraClass",
                                  ivarPublished->getMetaClass()->getClassName());
            OSString *bsd = OSDynamicCast(OSString, ivarPublished->getProperty("BSD Name"));
            if (bsd != nullptr)
                provider->setProperty("Ath10kCtrlInfraBsd", bsd->getCStringNoCopy());
            provider->setProperty("Ath10kCtrlInfraIsOurs", ivarPublished == _venturaInterface);
        } else {
            provider->setProperty("Ath10kCtrlInfraClass", "not-an-interface");
        }
    }
    const UInt64 selfAddress = static_cast<UInt64>(reinterpret_cast<uintptr_t>(this));
    const UInt64 memberAddress = static_cast<UInt64>(reinterpret_cast<uintptr_t>(&_venturaInterface));
    provider->setProperty("Ath10kIvarIfaceOffset", memberAddress >= selfAddress
                          ? memberAddress - selfAddress : 0, 32);
    // The controller ivar was already installed by createInterface(), where the
    // object was in hand. Do NOT re-scan the registry from here: the
    // getMatchingServices() pass wedged the kernel ("registry root held busy,
    // kernelmanagerd has not checked in") three seconds into start().
    // Stamp the 80211 user client class on the interface we published: airportd
    // opens a user client on that node and IOService::newUserClient picks the
    // class from IOUserClientClass, which IONetworkStack overwrites with its own
    // client when it attaches the unit to BSD - i.e. after our init() ran. This
    // retry runs seconds later, so it is the earliest point where the stamp can
    // survive. Both the value found and the value written are recorded.
    if (_venturaInterface != nullptr) {
        OSString *beforeClient =
            OSDynamicCast(OSString, _venturaInterface->getProperty("IOUserClientClass"));
        provider->setProperty("Ath10kUserClientBefore",
                              beforeClient != nullptr ? beforeClient->getCStringNoCopy() : "null");
        _venturaInterface->setProperty("IOUserClientClass", "IO80211APIUserClient");
        OSString *afterClient =
            OSDynamicCast(OSString, _venturaInterface->getProperty("IOUserClientClass"));
        provider->setProperty("Ath10kUserClientAfter",
                              afterClient != nullptr ? afterClient->getCStringNoCopy() : "null");
        provider->setProperty("Ath10kUserClientAttempt",
                              static_cast<UInt64>(_vifPublishAttempts), 32);
    }
    // Record what the registry looks like from here three seconds in: during
    // start() the controller's children are only the corecapture pipes and the
    // vif, and the same walk still found no "BSD Name" child.
    UInt32 index = 0;
    OSIterator *iter = getChildIterator(gIOServicePlane);
    while (iter != nullptr) {
        IOService *child = OSDynamicCast(IOService, iter->getNextObject());
        if (child == nullptr)
            break;
        if (++index > 8)
            break;
        char key[24];
        snprintf(key, sizeof(key), "Ath10kRetryChild%u", static_cast<unsigned>(index));
        provider->setProperty(key, child->getMetaClass()->getClassName());
        OSString *bsd = OSDynamicCast(OSString, child->getProperty("BSD Name"));
        if (bsd != nullptr) {
            char key2[24];
            snprintf(key2, sizeof(key2), "Ath10kRetryBsd%u", static_cast<unsigned>(index));
            provider->setProperty(key2, bsd->getCStringNoCopy());
        }
    }
    if (iter) iter->release();
    // Probe the ifnet lookup itself: every name-based source failed while
    // `ifconfig` plainly lists en1, so record the raw return codes.
    for (UInt32 unit = 0; unit < 4; ++unit) {
        char name[8];
        snprintf(name, sizeof(name), "en%u", static_cast<unsigned>(unit));
        ifnet_t probe = nullptr;
        const int rc = ifnet_find_by_name(name, &probe);
        char key[24];
        snprintf(key, sizeof(key), "Ath10kFindEn%u", static_cast<unsigned>(unit));
        provider->setProperty(key, static_cast<UInt64>(static_cast<UInt32>(rc)), 32);
    }
    // Apple's attach runs later than three seconds (measured: the delayed retry
    // saw the interface pointer still null while createInterface() had already
    // run by the time of the ioreg read). Re-arm instead of giving up, so the
    // controller ivar lands as soon as the interface exists.
    const bool installed = *reinterpret_cast<void **>(reinterpret_cast<char *>(this) + 0x200) != nullptr;
    if (_vifPublishCall != nullptr && !installed && ++_vifPublishAttempts <= 8) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(5, kSecondScale, &deadline);
        thread_call_enter_delayed(_vifPublishCall, deadline);
        provider->setProperty("Ath10kCtrlInfraRetries",
                              static_cast<UInt64>(_vifPublishAttempts), 32);
    }
}

SInt32 Ath10kPCIDevice::performCountryCodeOperation(IO80211CountryCodeOp operation)
{
    if (operation == kIO80211CountryCodeReset)
        return kIOReturnSuccess;
    return kIOReturnUnsupported;
}

SInt32 Ath10kPCIDevice::enableFeature(IO80211FeatureCode code, void *)
{
    if (code == kIO80211Feature80211n)
        return kIOReturnSuccess;
    return kIOReturnUnsupported;
}

// ---------------------------------------------------------------------------
// apple80211 request dispatch
// ---------------------------------------------------------------------------

void Ath10kPCIDevice::venturaPublishState(UInt32 state)
{
    _venturaState = state;
    if (IOService *provider = getProvider())
        provider->setProperty("Ath10kRadioState", static_cast<UInt64>(state), 32);
    // Deliberately no setLinkState() here. On the live 13.7 IO80211Interface that
    // virtual does not exist (only the legacy bundle has it), so the call
    // dispatches into an unrelated slot of the live vtable and the CPU ends up
    // executing an unmapped address: measured as a kernel trap with
    // RIP == CR2 == 0xffffff8052c00000 and the frame in
    // venturaDispatchRequest + 0x1b6, i.e. the return address right after the
    // call to venturaPower(). Link state is reported through the property until
    // the datapath stage wires the real one.
}

// Every apple80211 request from airportd funnels through here so the boot
// breadcrumbs can answer "did the request arrive, and what did we answer".
// airportd surfaces only "Off (forced)" for both a missing request and a
// refused one.
// Record every request that reaches a driver entry point, before any filtering.
// airportd's join died on a request whose type came back as 255 through this
// path, so the cmd word has to travel with the type: it says which calling
// convention the caller used and therefore where the type field really lives.
void Ath10kPCIDevice::venturaTraceRequest(UInt32 entry, UInt32 cmd, UInt32 type, UInt32 len)
{
    _venturaLastEntry = entry;
    _venturaLastEntryCmd = cmd;
    _venturaEntryTrace[_venturaEntryTraceNext].entry = entry;
    _venturaEntryTrace[_venturaEntryTraceNext].cmd = cmd;
    _venturaEntryTrace[_venturaEntryTraceNext].type = type;
    _venturaEntryTrace[_venturaEntryTraceNext].len = len;
    _venturaEntryTraceNext = (_venturaEntryTraceNext + 1) % kVenEntryTraceSlots;
    if (_venturaEntryTraceCount < kVenEntryTraceSlots)
        ++_venturaEntryTraceCount;
    IOService *probe = getProvider();
    if (probe == nullptr)
        return;
    char trace[kVenEntryTraceSlots * 32];
    trace[0] = 0;
    const UInt32 first = (_venturaEntryTraceCount == kVenEntryTraceSlots) ? _venturaEntryTraceNext : 0;
    for (UInt32 i = 0; i < _venturaEntryTraceCount; ++i) {
        const UInt32 slot = (first + i) % kVenEntryTraceSlots;
        char item[32];
        snprintf(item, sizeof(item), "%u:%lx:%u:%u ", _venturaEntryTrace[slot].entry,
                 _venturaEntryTrace[slot].cmd, _venturaEntryTrace[slot].type,
                 _venturaEntryTrace[slot].len);
        strlcat(trace, item, sizeof(trace));
    }
    probe->setProperty("Ath10kEntryTrace", trace);
}

SInt32 Ath10kPCIDevice::venturaRecordRequest(UInt requestType, int requestNumber, void *data)
{
    ++_venturaRequestCount;
    _venturaLastRequest = static_cast<UInt32>(requestNumber);
    _venturaLastRequestIsSet = requestType == 1;
    const SInt32 status = venturaDispatchRequest(requestType, requestNumber, nullptr, data);
    _venturaLastRequestStatus = status;
    // Keep the last few (verb, type, status) triples: "the client stopped asking
    // after request N" is only diagnosable from the sequence, not from the last
    // single request. airportd's scans begin with a batch of small getters, and a
    // refused one there aborts the scan without ever sending SCAN_REQ.
    _venturaTrace[_venturaTraceNext].isSet = requestType == 1;
    _venturaTrace[_venturaTraceNext].type = static_cast<UInt32>(requestNumber);
    _venturaTrace[_venturaTraceNext].status = static_cast<UInt32>(status);
    _venturaTraceNext = (_venturaTraceNext + 1) % kVenTraceSlots;
    if (_venturaTraceCount < kVenTraceSlots)
        ++_venturaTraceCount;
    if (IOService *provider = getProvider()) {
        char trace[kVenTraceSlots * 24];
        trace[0] = 0;
        const UInt32 first = (_venturaTraceCount == kVenTraceSlots) ? _venturaTraceNext : 0;
        for (UInt32 i = 0; i < _venturaTraceCount; ++i) {
            const UInt32 slot = (first + i) % kVenTraceSlots;
            char item[24];
            snprintf(item, sizeof(item), "%s%u=%x ", _venturaTrace[slot].isSet ? "s" : "g",
                     _venturaTrace[slot].type, _venturaTrace[slot].status);
            strlcat(trace, item, sizeof(trace));
        }
        provider->setProperty("Ath10kRequestTrace", trace);
        provider->setProperty("Ath10kApple80211Requests",
                              static_cast<UInt64>(_venturaRequestCount), 32);
        provider->setProperty("Ath10kLastApple80211Request",
                              static_cast<UInt64>(_venturaLastRequest), 32);
        provider->setProperty("Ath10kLastApple80211Status",
                              static_cast<UInt64>(static_cast<UInt32>(status)), 32);
        provider->setProperty("Ath10kLastApple80211Verb",
                              _venturaLastRequestIsSet ? "set" : "get");
    }
    return status;
}

// The live family reaches a driver through these two, one per interface class.
SInt32 Ath10kPCIDevice::apple80211VirtualRequest(UInt requestType, int requestNumber,
                                                 IO80211VirtualInterface *, void *data)
{
    return venturaRecordRequest(requestType, requestNumber, data);
}

// Measurement shims for the two generic ioctl slots. The family's own per-type
// handlers (setPower, setScanRequest, ...) call [controller vptr+0xcb0] with
// arguments this driver never saw documented: setPower passes (0x803069c8, 19,
// <interface>, <req_data>) there and takes the return value as the request result,
// which is why a POWER set answers 102/EOPNOTSUPP. Publishing the raw registers
// tells us which slot and which argument order that really is, and the Apple
// implementation still runs afterwards so nothing regresses in the meantime.
static void publishIoctlArgs(IOService *probe, const char *slot, unsigned long a, unsigned long b,
                             unsigned long c, unsigned long d)
{
    if (probe == nullptr)
        return;
    char value[160];
    snprintf(value, sizeof(value), "%s a=%lx b=%lx c=%lx d=%lx", slot, a, b, c, d);
    probe->setProperty("Ath10kIoctlArgs", value);
    probe->setProperty("Ath10kIoctlArgsSlot", slot);
}

// Which of the three transport paths carried airportd's SCAN_RESULT (type 11)?
// A type-11 request can arrive three different ways: the BSD ioctl channel
// (performCommand), the slot-406 getter convention (cmd in arg1), or the slot-408
// Skywalk request the family's scan manager calls. They have different buffer
// shapes, and serving two of them at once is what wedged the client's read loop.
// Each entry records its own type-11 hit count, request length and status, so one
// cwscan run answers the question instead of another round of guessing.
struct VenturaEntryProbe {
    const char *name;
    UInt32 hits;
    UInt32 type;
    UInt32 len;
    SInt32 status;
};

static VenturaEntryProbe gVenturaEntryProbe[3] = {
    {"perf", 0, 0, 0, 0},
    {"slot406", 0, 0, 0, 0},
    {"slot408", 0, 0, 0, 0},
};

extern "C" void ath10kVenturaProbeEntry(int which, IOService *probe, UInt32 type, UInt32 len,
                                       SInt32 status)
{
    if (which < 0 || which >= 3)
        return;
    if (type != APPLE80211_IOC_SCAN_RESULT)
        return;
    VenturaEntryProbe &entry = gVenturaEntryProbe[which];
    ++entry.hits;
    entry.type = type;
    entry.len = len;
    entry.status = status;
    IOLog("Ath10kMac: SCAN_RESULT via %s len=%u st=%x (hit %u)\n", entry.name, len,
          static_cast<unsigned>(static_cast<UInt32>(status)), entry.hits);
    if (probe == nullptr)
        return;
    char key[32];
    snprintf(key, sizeof(key), "Ath10kEnt_%s", entry.name);
    char value[80];
    snprintf(value, sizeof(value), "n=%u len=%u st=%x", entry.hits, entry.len,
             static_cast<unsigned>(static_cast<UInt32>(entry.status)));
    probe->setProperty(key, value);
}

// ---------------------------------------------------------------------------
// In-kernel WPA2 supplicant (open networks never touch this)
// ---------------------------------------------------------------------------
//
// The Ventura personality has no user-space helper, so M1/M3 arrive as EAPOL and
// have to be answered here. The supplicant only produces the handshake messages
// and the derived keys; installing them stays with the shared state machine
// (buildSecurityWMI + the securityPhase ladder), which is the code path the
// native personality already proves.

struct Ath10kVenturaSupplicant {
    AthSecurity::Supplicant<Ath10kKernelCrypto, true, true> supplicant;
    UInt8 pmk[32]{};
    UInt8 akm = 2;
    bool pmkReady = false;
    bool active = false;
    bool replyPending = false;
    bool keysPending = false;
    UInt32 keyOperation = 0;
    UInt8 reply[512]{};
    UInt32 replyLength = 0;
    UInt8 keys[56]{};
};

// WPA2 passphrase -> PMK (PBKDF2-HMAC-SHA256, 4096 rounds, salt = SSID). A 64-hex
// key is already the PMK, which is the other documented form of `wpa-psk`.
static bool venturaDerivePMK(const UInt8 *psk, UInt32 length, const UInt8 *ssid, UInt32 ssidLength,
                             UInt8 *pmk)
{
    if (length == 64) {
        UInt8 raw[32];
        for (UInt32 i = 0; i < 32; ++i) {
            UInt8 value = 0;
            for (UInt32 nibble = 0; nibble < 2; ++nibble) {
                const UInt8 c = psk[i * 2 + nibble];
                UInt8 digit;
                if (c >= '0' && c <= '9')
                    digit = static_cast<UInt8>(c - '0');
                else if (c >= 'a' && c <= 'f')
                    digit = static_cast<UInt8>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')
                    digit = static_cast<UInt8>(c - 'A' + 10);
                else
                    return false;
                value = static_cast<UInt8>((value << 4) | digit);
            }
            raw[i] = value;
        }
        bcopy(raw, pmk, 32);
        AthSecurity::wipe(raw, sizeof(raw));
        return true;
    }
    if (length < 8 || length > 63)
        return false;
    // WPA2-PSK derives the PMK with PBKDF2-HMAC-**SHA1** (RFC 2898 with SHA1, 4096
    // rounds, salt = SSID, 32-byte output = two 20-byte blocks). SHA256 here makes
    // the PMK wrong, and the only symptom is the AP dropping M2: hostapd logs
    // "PTK derivation using PRF(SHA1)" then "invalid MIC in msg 2/4".
    // Implemented over the HMAC the supplicant already links rather than calling
    // corecrypto's ccpbkdf2_hmac: that symbol is C++-mangled in the kernel SDK, so
    // importing it leaves an unresolved name and OpenCore silently skips the kext.
    if (ssidLength > 60)
        return false;
    UInt8 msg[64];
    bcopy(ssid, msg, ssidLength);
    for (UInt32 blockIndex = 1; blockIndex <= 2; ++blockIndex) {
        msg[ssidLength] = 0;
        msg[ssidLength + 1] = 0;
        msg[ssidLength + 2] = 0;
        msg[ssidLength + 3] = static_cast<UInt8>(blockIndex);   // big-endian counter
        UInt8 u[20], t[20];
        if (!Ath10kKernelCrypto::hmac(psk, length, msg, ssidLength + 4, u))
            return false;
        bcopy(u, t, sizeof(t));
        for (UInt32 round = 1; round < 4096; ++round) {
            UInt8 next[20];
            if (!Ath10kKernelCrypto::hmac(psk, length, u, sizeof(u), next))
                return false;
            bcopy(next, u, sizeof(u));
            for (UInt32 i = 0; i < sizeof(t); ++i)
                t[i] = static_cast<UInt8>(t[i] ^ u[i]);
        }
        if (blockIndex == 1)
            bcopy(t, pmk, 20);
        else
            bcopy(t, pmk + 20, 12);
        AthSecurity::wipe(u, sizeof(u));
        AthSecurity::wipe(t, sizeof(t));
    }
    AthSecurity::wipe(msg, sizeof(msg));
    return true;
}

IOReturn Ath10kPCIDevice::venturaSecurityArm(const UInt8 *psk, UInt32 pskLength, UInt8 akm)
{
    IOService *probe = getProvider();
    auto &s = _connection;
    if (_venturaSecurity == nullptr) {
        _venturaSecurity = IONew(Ath10kVenturaSupplicant, 1);
        if (_venturaSecurity == nullptr) {
            if (probe != nullptr)
                probe->setProperty("Ath10kSecPhase", "no-memory");
            return kIOReturnNoMemory;
        }
        // IOMalloc gives raw memory: the default member initialisers do not run, so
        // the one non-zero default (the error string publishConnection-style logging
        // reads with %s) is restored by hand.
        bzero(_venturaSecurity, sizeof(*_venturaSecurity));
        _venturaSecurity->supplicant.error = "none";
    }
    Ath10kVenturaSupplicant *sec = _venturaSecurity;
    if (!venturaDerivePMK(psk, pskLength, s.ssid, s.ssidLength, sec->pmk)) {
        if (probe != nullptr) {
            char phase[64];
            snprintf(phase, sizeof(phase), "bad-psk len=%u", static_cast<unsigned>(pskLength));
            probe->setProperty("Ath10kSecPhase", phase);
        }
        return kIOReturnBadArgument;
    }
    sec->akm = akm;
    sec->pmkReady = true;
    if (probe != nullptr) {
        // Fingerprint only: lets the host-side test compare the derived PMK against
        // an independent PBKDF2 without ever printing key material. Two leading bytes
        // of the passphrase catch a mis-parsed apple80211_key offset, two of the PMK
        // catch a wrong PRF.
        char check[80];
        snprintf(check, sizeof(check), "len=%u p=%02x%02x k=%02x%02x m=%02x%02x%02x%02x%02x%02x",
                 static_cast<unsigned>(pskLength), psk[0], pskLength > 1 ? psk[1] : 0,
                 sec->pmk[0], sec->pmk[1], s.local[0], s.local[1], s.local[2], s.local[3],
                 s.local[4], s.local[5]);
        probe->setProperty("Ath10kSecCheck", check);
        probe->setProperty("Ath10kSecPhase", "pmk-ready");
    }
    return kIOReturnSuccess;
}

void Ath10kPCIDevice::venturaSecurityStop()
{
    auto sec = _venturaSecurity;
    if (sec == nullptr)
        return;
    sec->supplicant.reset();
    AthSecurity::wipe(sec->pmk, sizeof(sec->pmk));
    AthSecurity::wipe(sec->reply, sizeof(sec->reply));
    AthSecurity::wipe(sec->keys, sizeof(sec->keys));
    sec->pmkReady = sec->active = sec->replyPending = sec->keysPending = false;
    sec->replyLength = 0;
    sec->keyOperation = 0;
    if (IOService *probe = getProvider())
        probe->setProperty("Ath10kSecPhase", "stopped");
}

// Push one supplicant result into the shared install machine's vocabulary.
bool Ath10kPCIDevice::venturaSecurityAccept(unsigned int result)
{
    using S = AthSecurity::Supplicant<Ath10kKernelCrypto, true, true>;
    auto sec = _venturaSecurity;
    auto &s = _connection;
    if (sec == nullptr || sec->replyPending || sec->keysPending)
        return false;
    const bool data = result == S::Keys || result == S::GroupKeys || result == S::PairKeys;
    if (result != S::Reply && !data)
        return false;
    if (!sec->supplicant.outputLength || sec->supplicant.outputLength > sizeof(sec->reply))
        return false;
    if (data) {
        const UInt8 changes = sec->supplicant.keyChanges;
        if (changes & ~UInt8(S::PTKChanged | S::GTKChanged))
            return false;
        if ((result == S::Keys || result == S::PairKeys) && !(changes & S::PTKChanged))
            return false;
        if (result == S::GroupKeys && ((changes & S::GTKChanged) == 0 || (changes & S::PTKChanged)))
            return false;
        if (sec->supplicant.keys[49] > 3 || sec->supplicant.keys[48] != s.groupCipher)
            return false;
        bcopy(sec->supplicant.keys, sec->keys, sizeof(sec->keys));
        sec->keysPending = true;
        sec->keyOperation = result;
    }
    bcopy(sec->supplicant.output, sec->reply, sec->supplicant.outputLength);
    sec->replyLength = static_cast<UInt32>(sec->supplicant.outputLength);
    sec->replyPending = true;
    // PTK exists only after M1 arrived, so the fingerprint belongs here and not at
    // begin() time (where it reads as 0000).
    if (IOService *probe = getProvider()) {
        char ptk[24];
        snprintf(ptk, sizeof(ptk), "%02x%02x", sec->supplicant.ptkByte(0),
                 sec->supplicant.ptkByte(1));
        probe->setProperty("Ath10kSecPtkFp", ptk);
    }
    return true;
}

const char *Ath10kPCIDevice::venturaSecurityService(UInt64 nowMs)
{
    using S = AthSecurity::Supplicant<Ath10kKernelCrypto, true, true>;
    auto sec = _venturaSecurity;
    if (sec == nullptr || !sec->pmkReady)
        return nullptr;
    auto &s = _connection;
    IOService *probe = getProvider();
    if (s.cancelRequested || s.disconnect != Ath10k::DisconnectStage::None ||
        s.stage == Ath10k::LinkStage::Disabled || s.stage == Ath10k::LinkStage::Failed) {
        if (sec->active)
            venturaSecurityStop();
        return nullptr;
    }
    if (!s.secure || !Ath10k::connectionAssociated(s))
        return nullptr;
    // The AP's RSN element is only known once the BSS candidate is committed, so
    // the handshake starts here rather than at request time.
    if (!sec->active) {
        if (s.ap.rsnLength == 0)
            return "wpa2-no-rsn-element";
        UInt8 nonce[32];
        if (!Ath10kKernelCrypto::random(nonce, sizeof(nonce)))
            return "wpa2-no-random";
        const bool started = sec->supplicant.begin(sec->pmk, nonce, s.local, s.bssid, s.ap.rsn,
                                                   s.ap.rsnLength, sec->akm);
        AthSecurity::wipe(nonce, sizeof(nonce));
        if (!started) {
            if (probe != nullptr)
                probe->setProperty("Ath10kSecPhase", "begin-rejected");
            return "wpa2-supplicant-rejected";
        }
        sec->active = true;
        if (probe != nullptr) {
            char phase[64];
            snprintf(phase, sizeof(phase), "handshake akm=%u rsn=%u", sec->akm,
                     static_cast<unsigned>(s.ap.rsnLength));
            probe->setProperty("Ath10kSecPhase", phase);
            char ptk[24];
            snprintf(ptk, sizeof(ptk), "%02x%02x", sec->supplicant.ptkByte(0),
                     sec->supplicant.ptkByte(1));
            probe->setProperty("Ath10kSecPtkFp", ptk);
        }
    }
    // Handshake frames are exchanged on the workloop; the same gates the native
    // security service uses keep DMA and WMI transactions from overlapping.
    const bool quiet = !s.txOwned && !s.dataPending && !s.wmiOwned && !s.submitted &&
                       !s.stats.step && !s.maintenance.step && !s.radio.step && !s.csa.blocked &&
                       !s.txPauseMask && _wmiScan.completed && !_wmi.credits.outstanding;

    // The initial install happens while the ladder is still at phase 0 (native's
    // own condition is "initial && !securityPhase"); a rekey only runs once the port
    // is live at phase 5. Requiring phase != 0 here deadlocks the first handshake:
    // the AP answers our M2 with M3, we derive keys, refuse to install them, and
    // then deauthenticates with reason 15 after its own 4-way timeout.
    const bool initial = sec->keysPending && sec->keyOperation == S::Keys;
    const bool pair = sec->keysPending && sec->keyOperation == S::PairKeys;
    if (sec->keysPending && quiet &&
        ((initial && s.securityPhase == 0) || (!initial && s.securityPhase == 5 && s.portAuthorized))) {
        const unsigned keyBytes = s.groupCipher == AthSecurity::cipherTKIP ? 32 : 16;
        const bool sameGroup = s.groupInstalled[sec->keys[49]] &&
                               AthSecurity::equal(sec->keys + 16, s.installedGroups[sec->keys[49]],
                                                  keyBytes);
        bcopy(sec->keys, s.temporalKey, 16);
        bcopy(sec->keys + 16, s.groupKey, 32);
        s.groupIndex = sec->keys[49];
        bcopy(sec->keys + 50, s.groupRSC, 6);
        if (initial) {
            UInt64 pn = 0;
            for (unsigned i = 0; i < 6; ++i)
                pn |= UInt64(s.groupRSC[i]) << (8 * i);
            for (auto &v : s.groupPN)
                v = pn;
        }
        s.rekeyKind = initial ? 0 : (pair ? 2 : 1);
        s.rekeySkipGroup = pair && sameGroup;
        s.securityPhase = (initial || pair) ? 1 : 2;
        s.submitted = false;
        s.securityAck = false;
        s.authorizeRequested = false;
        s.portAuthorized = false;
        s.sinceMs = nowMs;
        sec->keysPending = false;
        AthSecurity::wipe(sec->keys, sizeof(sec->keys));
        if (probe != nullptr) {
            char phase[64];
            snprintf(phase, sizeof(phase), "keys-in initial=%u pair=%u gtk=%u", initial ? 1 : 0,
                     pair ? 1 : 0, s.groupIndex);
            probe->setProperty("Ath10kSecPhase", phase);
        }
    }
    if (sec->replyPending && quiet && !sec->keysPending &&
        (s.securityPhase == 0 || s.securityPhase == 3 || s.securityPhase == 5)) {
        const char *error = submitNetworkPacket(sec->reply, sec->replyLength, false, nowMs);
        if (error) {
            s.error = error;
            s.cancelRequested = true;
            return error;
        }
        sec->replyPending = false;
        AthSecurity::wipe(sec->reply, sizeof(sec->reply));
        sec->replyLength = 0;
    }
    // Four-way handshake is done once the firmware acknowledged the M4/install
    // transaction; the shared ladder then authorizes the controlled port.
    if (s.securityPhase == 3 && !s.authorizeRequested && !sec->replyPending && !sec->keysPending &&
        s.txComplete && !s.txStatus && !s.eapolTxFailed)
        s.authorizeRequested = true;
    if (!sec->replyPending && !sec->keysPending && s.eapolCount && !s.rekeyKind) {
        const auto result = sec->supplicant.receive(s.eapolFrames[s.eapolRead],
                                                    s.eapolLength[s.eapolRead]);
        AthSecurity::wipe(s.eapolFrames[s.eapolRead], 512);
        s.eapolRead = (s.eapolRead + 1) % 4;
        --s.eapolCount;
        if (result == S::Fatal) {
            s.error = "wpa2-handshake-failed";
            s.cancelRequested = true;
            if (probe != nullptr) {
                char phase[96];
                snprintf(phase, sizeof(phase), "fatal %s", sec->supplicant.error);
                probe->setProperty("Ath10kSecPhase", phase);
            }
            return nullptr;
        }
        if (result != S::Ignore && !venturaSecurityAccept(static_cast<UInt32>(result))) {
            s.error = "wpa2-result-rejected";
            s.cancelRequested = true;
            s.portAuthorized = false;
            if (probe != nullptr)
                probe->setProperty("Ath10kSecPhase", "result-rejected");
        }
    }
    return nullptr;
}

SInt32 Ath10kPCIDevice::apple80211_ioctl(IO80211VirtualInterface *iface, ifnet_t ifp,
                                         unsigned long cmd, void *data)
{
    publishIoctlArgs(getProvider(), "virtual-ifnet-cmd-data",
                     reinterpret_cast<unsigned long>(iface), reinterpret_cast<unsigned long>(ifp),
                     cmd, reinterpret_cast<unsigned long>(data));
    return IO80211Controller::apple80211_ioctl(iface, ifp, cmd, data);
}

// The 48-byte request block every apple80211 ioctl carries:
//   char if_name[16]; int req_type; int req_val; u32 req_len; void *req_data;
// The family's own handlers prove the offsets: setPowersave reads req_val at
// +0x14, getCurrentNetwork compares req_len at +0x18 against 0x48c, and
// setRSN_Conf loads req_data from +0x20. Reading req_type from the start of the
// block (as this struct used to) is what returned 255.
struct VenturaRawRequest {
    char ifName[16];
    int type;      // +0x10
    int val;       // +0x14
    UInt32 len;    // +0x18
    void *data;    // +0x20
};

SInt32 Ath10kPCIDevice::apple80211_ioctl(IO80211SkywalkInterface *iface, unsigned long a,
                                         void *b, void *c)
{
    const unsigned long kGetCmd = 0xc03069c9UL;   // _IOWR('i', 201, 48) - getter
    const unsigned long kSetCmd = 0x803069c8UL;   // _IOW('i', 200, 48) - setter
    const unsigned long second = a;
    const unsigned long third = reinterpret_cast<unsigned long>(b);
    const unsigned long fourth = reinterpret_cast<unsigned long>(c);
    const unsigned long first = reinterpret_cast<unsigned long>(iface);
    publishIoctlArgs(getProvider(), "skywalk", first, second, third, fourth);
    venturaTraceRequest(1, static_cast<UInt32>(second), static_cast<UInt32>(third),
                        static_cast<UInt32>(fourth));
    // Record the first dwords of *every* request block that reaches this slot, not
    // just the ones whose type looks wrong: comparing a call we answer correctly
    // (type 96) with one that reads as 387 is the only way to learn where the
    // caller really writes req_type.
    if ((second == kGetCmd || second == kSetCmd) && third >= 0xffffff0000000000UL) {
        const UInt32 *words = reinterpret_cast<const UInt32 *>(third);
        _venturaArgDump[_venturaArgDumpNext][0] = static_cast<UInt32>(second);
        for (UInt32 i = 0; i < 6; ++i)
            _venturaArgDump[_venturaArgDumpNext][i + 1] = words[i];
        _venturaArgDumpNext = (_venturaArgDumpNext + 1) % kVenArgDumpSlots;
        if (_venturaArgDumpCount < kVenArgDumpSlots)
            ++_venturaArgDumpCount;
        if (IOService *probe = getProvider()) {
            char trace[kVenArgDumpSlots * 48];
            trace[0] = 0;
            const UInt32 firstSlot =
                (_venturaArgDumpCount == kVenArgDumpSlots) ? _venturaArgDumpNext : 0;
            for (UInt32 i = 0; i < _venturaArgDumpCount; ++i) {
                const UInt32 slot = (firstSlot + i) % kVenArgDumpSlots;
                char item[48];
                snprintf(item, sizeof(item), "%lx:%x,%x,%x,%x,%x,%x ", _venturaArgDump[slot][0],
                         _venturaArgDump[slot][1], _venturaArgDump[slot][2],
                         _venturaArgDump[slot][3], _venturaArgDump[slot][4],
                         _venturaArgDump[slot][5], _venturaArgDump[slot][6]);
                strlcat(trace, item, sizeof(trace));
            }
            probe->setProperty("Ath10kArgDump", trace);
        }
    }

    // Two conventions reach this slot (see IO80211Controller.h). Getter paths put the
    // cmd in the 2nd argument and the request block in the 3rd; setPower and
    // setScanRequest put the cmd in the 1st, the type in the 2nd, the interface in the
    // 3rd and the payload in the 4th. Serving both is what makes a POWER/SCAN_REQ from
    // the family land in this driver instead of coming back as 102/EOPNOTSUPP.
    if (second == kGetCmd || second == kSetCmd) {
        const bool isSet = second == kSetCmd;
        if (third >= 0xffffff0000000000UL) {
            const auto *raw = reinterpret_cast<const VenturaRawRequest *>(third);
            if (IOService *probe = getProvider()) {
                probe->setProperty("Ath10kIoctlReqType", static_cast<UInt64>(
                                       static_cast<UInt32>(raw->type)), 32);
                // A request number outside the apple80211 type range means this
                // pointer is not a request block at all (measured: type 255 for
                // cmd 0x803069c8). Dump the first 0x28 bytes so the next run says
                // what it really is instead of needing another guess.
                if (raw->type < 0 || raw->type > 200) {
                    const UInt8 *bytes = reinterpret_cast<const UInt8 *>(raw);
                    char dump[0x28 * 3 + 1];
                    dump[0] = 0;
                    if (bytes != nullptr) {
                        for (UInt32 i = 0; i < 0x28; ++i) {
                            char one[4];
                            snprintf(one, sizeof(one), "%02x ", bytes[i]);
                            strlcat(dump, one, sizeof(dump));
                        }
                    }
                    probe->setProperty("Ath10kReqDump", dump);
                    // Who is asking: the type reads as garbage, so identify the
                    // caller instead of guessing what the number means. pid/name
                    // come from the kernel's own helpers, the return address is the
                    // live family function that dispatched into this slot.
                    char who[128];
                    char name[64];
                    name[0] = 0;
                    proc_selfname(name, sizeof(name));
                    snprintf(who, sizeof(who), "pid=%d name=%s ret=%p type=%d len=%u",
                             proc_selfpid(), name, __builtin_return_address(0),
                             raw->type, static_cast<unsigned>(raw->len));
                    probe->setProperty("Ath10kReqWho", who);
                    // The request block alone says "which type"; the payload says
                    // "what it wants". 0x38 bytes is what airportd sent here, so dump
                    // it and read the answer instead of guessing at the type number.
                    if (raw->data != nullptr && raw->len != 0 && raw->len <= 0x40) {
                        const UInt8 *payload = reinterpret_cast<const UInt8 *>(raw->data);
                        char hex[0x40 * 3 + 1];
                        hex[0] = 0;
                        for (UInt32 i = 0; i < raw->len; ++i) {
                            char one[4];
                            snprintf(one, sizeof(one), "%02x ", payload[i]);
                            strlcat(hex, one, sizeof(hex));
                        }
                        probe->setProperty("Ath10kReqPayload", hex);
                    }
                }
            }
            // SCAN_REQ has to go through the manager: it owns the pending request node
            // that airportd's scan completion (and its later result read) is matched
            // against. SCAN_RESULT is served from the driver's own cursor below, because
            // getScanResult() only answers a pid that owns one of those nodes.
            const SInt32 routed = venturaScanManagerRequest(raw->type, raw->data);
            if (routed != kIOReturnUnsupported) {
                ath10kVenturaProbeEntry(1, getProvider(), raw->type,
                                        static_cast<UInt32>(raw->len), routed);
                return routed;
            }
            const SInt32 status = venturaRecordRequest(isSet ? 1 : 0, raw->type, raw->data);
            ath10kVenturaProbeEntry(1, getProvider(), raw->type,
                                    static_cast<UInt32>(raw->len), status);
            if (status == kIOReturnSuccess)
                return kIOReturnSuccess;
            // SCAN_RESULT must never fall through to Apple's implementation: its
            // answer for a getter it does not route is 0x66 (EOPNOTSUPP = the 102
            // airportd prints), and the client reads that as "the scan failed"
            // even though it just walked a full list. The end-of-list status this
            // driver returns is what ends the read loop.
            if (raw->type == APPLE80211_IOC_SCAN_RESULT)
                return status;
            _venturaFallType = raw->type;
        } else if (third != 0) {
            // User-space apple80211req. Apple's own path copies it in before using
            // it, which is why airportd's join reached this slot with a user address
            // (measured: 1:c03069c9:559930376:65535) and every type read here was
            // garbage - the join then died "unsupported" without an ASSOCIATE ever
            // being sent. Copy the request in, serve it against kernel memory, and
            // copy the answer back out for getters.
            VenturaRawRequest raw{};
            if (ath10kControllerCopyIn(this, static_cast<UInt64>(third), &raw, sizeof(raw)) == 0) {
                if (IOService *probe = getProvider()) {
                    probe->setProperty("Ath10kIoctlReqType",
                                       static_cast<UInt64>(static_cast<UInt32>(raw.type)), 32);
                    // This is the path airportd's own requests appear on (the third
                    // argument is a ~1.1 GB user address). Copy in and publish the
                    // whole block: the types read out of it (255/256/387) are not
                    // apple80211 numbers, so the layout has to be seen, not assumed.
                    const UInt8 *bytes = reinterpret_cast<const UInt8 *>(&raw);
                    char hex[sizeof(raw) * 3 + 1];
                    hex[0] = 0;
                    for (UInt32 i = 0; i < sizeof(raw); ++i) {
                        char one[4];
                        snprintf(one, sizeof(one), "%02x ", bytes[i]);
                        strlcat(hex, one, sizeof(hex));
                    }
                    probe->setProperty("Ath10kUserArgDump", hex);
                }
                if (raw.len != 0 && raw.len <= 0x800 && raw.data != nullptr) {
                    UInt8 scratch[0x800];
                    bzero(scratch, sizeof(scratch));
                    const bool haveInput =
                        !isSet || ath10kControllerCopyIn(this,
                                                         reinterpret_cast<UInt64>(raw.data),
                                                         scratch, raw.len) == 0;
                    if (haveInput) {
                        const SInt32 status =
                            venturaRecordRequest(isSet ? 1 : 0, raw.type, scratch);
                        ath10kVenturaProbeEntry(1, getProvider(), raw.type, raw.len, status);
                        if (status == kIOReturnSuccess && !isSet)
                            ath10kControllerCopyOut(this, scratch,
                                                    reinterpret_cast<UInt64>(raw.data), raw.len);
                        if (status == kIOReturnSuccess || raw.type == APPLE80211_IOC_SCAN_RESULT)
                            return status;
                        _venturaFallType = raw.type;
                    }
                } else if (raw.len == 0) {
                    const SInt32 status = venturaRecordRequest(isSet ? 1 : 0, raw.type, nullptr);
                    ath10kVenturaProbeEntry(1, getProvider(), raw.type, 0, status);
                    if (status == kIOReturnSuccess)
                        return kIOReturnSuccess;
                    _venturaFallType = raw.type;
                }
            }
        }
    } else if (first == kSetCmd || first == kGetCmd) {
        const bool isSet = first == kSetCmd;
        const int type = static_cast<int>(second);
        if (IOService *probe = getProvider())
            probe->setProperty("Ath10kIoctlReqType", static_cast<UInt64>(
                                   static_cast<UInt32>(type)), 32);
        // Both verbs carry the payload directly here. Getters have to be served too:
        // airportd's SCAN_RESULT arrives this way, and leaving it to Apple's
        // implementation is what returned 0x66 (= the error 102 it reports).
        const SInt32 status = venturaRecordRequest(isSet ? 1 : 0, type, c);
        ath10kVenturaProbeEntry(1, getProvider(), static_cast<UInt32>(type), 0, status);
        if (status == kIOReturnSuccess)
            return kIOReturnSuccess;
        // Same rule as above: 0x66 here is what airportd reports as error 102.
        if (type == APPLE80211_IOC_SCAN_RESULT)
            return status;
        _venturaFallType = type;
    }
    // Anything this driver does not serve still goes to Apple's implementation, which
    // is what answered the housekeeping gets before this shim existed. Its 0x66
    // (EOPNOTSUPP) is exactly the 102 airportd reports when a scan comes back empty, so
    // which type falls through and what it answers is published here.
    const SInt32 appleStatus = IO80211Controller::apple80211_ioctl(iface, a, b, c);
    if (IOService *probe = getProvider()) {
        char fall[96];
        snprintf(fall, sizeof(fall), "type=%d st=%x entry=%u cmd=%lx", _venturaFallType,
                 static_cast<unsigned>(appleStatus), _venturaLastEntry,
                 static_cast<unsigned long>(_venturaLastEntryCmd));
        probe->setProperty("Ath10kIoctlFall", fall);
    }
    return appleStatus;
}

SInt32 Ath10kPCIDevice::apple80211SkywalkRequest(UInt requestType, int requestNumber,
                                                 IO80211SkywalkInterface *, void *data)
{
    // The first argument is the ioctl command, not a get/set flag: the family's
    // scan manager reaches this slot with 0x803069c8 / 0xc03069c9 exactly like the
    // ioctl path does (live 0x201342c for SCAN_REQ, 0x2013fba for SCAN_RESULT).
    const bool isSet = requestType == 0x803069c8UL;
    venturaTraceRequest(2, requestType, static_cast<UInt32>(requestNumber), 0);
    if (requestNumber == APPLE80211_IOC_SCAN_RESULT && !isSet && data != nullptr) {
        // Results come back as a *pointer* here: the manager passes the address of
        // its own local and memcpy's sizeof(apple80211_scan_result) from whatever is
        // stored there, so writing the struct in place would smash its stack.
        if (gVenturaScanSlot != nullptr) {
            // The manager drains the sweep through this pointer-out shape and so
            // does the client's own read; a single shared cursor handed the head of
            // the list to whichever came first, and the other reader started
            // mid-list (measured: manager consumed 0-10, airportd got 11-19 and
            // still showed no networks). Give every pid its own position here too.
            int8_t *slot = venturaReaderSlot();
            if (slot != nullptr)
                _venturaScanIndex = static_cast<UInt8>(*slot);
            _venturaIoctlLength = sizeof(*gVenturaScanSlot);
            const IOReturn status = venturaScanResult(gVenturaScanSlot);
            if (slot != nullptr)
                *slot = static_cast<int8_t>(_venturaScanIndex);
            ath10kVenturaProbeEntry(2, getProvider(), requestNumber, _venturaIoctlLength, status);
            if (status == kIOReturnSuccess) {
                *static_cast<apple80211_scan_result **>(data) = gVenturaScanSlot;
                return kIOReturnSuccess;
            }
            // End of the sweep. This caller stores a *pointer* here - the manager's
            // own 8-byte local - and memcpy's 0x48c from whatever we put in it, so
            // there is nothing to write for an empty answer. Falling through to the
            // plain-buffer path below memset 0x48c bytes at that 8-byte local:
            // measured as a page fault on the kernel stack guard page (CR2 = stack
            // top, RDX = 0x48c) inside IO80211ScanManager::scanDone, which panics.
            return status;
        }
        // Not the manager's pointer-out shape: the family also calls this slot with a
        // plain result buffer (airportd's SCAN_RESULT arrives here), so fill that with
        // this reader's own position in the sweep.
        int8_t *slot = venturaReaderSlot();
        if (slot != nullptr)
            _venturaScanIndex = static_cast<UInt8>(*slot);
        const IOReturn plain = venturaScanResult(static_cast<apple80211_scan_result *>(data));
        if (slot != nullptr)
            *slot = static_cast<int8_t>(_venturaScanIndex);
        ath10kVenturaProbeEntry(2, getProvider(), requestNumber, _venturaIoctlLength, plain);
        return plain;
    }
    return venturaRecordRequest(isSet ? 1 : 0, requestNumber, data);
}

// The client polls STATE while it waits for a join; answering a constant INIT
// makes airportd believe the join never started.
static UInt32 venturaStageState(const Ath10k::ConnectionState &s)
{
    using Ath10k::LinkStage;
    switch (s.stage) {
    case LinkStage::Disabled:
        return APPLE80211_S_INIT;
    case LinkStage::Scan:
        return APPLE80211_S_SCAN;
    case LinkStage::Start:
    case LinkStage::Peer:
    case LinkStage::PeerAssoc:
    case LinkStage::Auth:
        return 2;   // APPLE80211_S_AUTH
    case LinkStage::Assoc:
    case LinkStage::Barrier:
        return 3;   // APPLE80211_S_ASSOC
    case LinkStage::Connected:
        return APPLE80211_S_RUN;
    default:
        return APPLE80211_S_SCAN;
    }
}

SInt32 Ath10kPCIDevice::venturaDispatchRequest(unsigned int requestType, int requestNumber,
                                               IO80211Interface *, void *data)
{
    if (data == nullptr)
        return kIOReturnBadArgument;
    const bool isSet = requestType == 1;
    switch (requestNumber) {
    case APPLE80211_IOC_POWER: {
        apple80211_power_data *power = static_cast<apple80211_power_data *>(data);
        if (isSet) {
            venturaPower(power->power_state[0] != 0);
        } else {
            power->version = APPLE80211_VERSION;
            power->num_radios = APPLE80211_MAX_RADIO;
            for (int i = 0; i < APPLE80211_MAX_RADIO; i++)
                power->power_state[i] = _venturaPowerOn ? 1 : 0;
        }
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_STATE: {
        if (isSet)
            return kIOReturnUnsupported;
        apple80211_state_data *state = static_cast<apple80211_state_data *>(data);
        state->version = APPLE80211_VERSION;
        // Link progress, not radio power: this is what the client's join loop polls
        // until it reaches APPLE80211_S_RUN.
        _venturaState = venturaStageState(_connection);
        state->state = _venturaState;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_SSID: {
        if (isSet) {
            // airportd's join is "Apple80211Set(SSID) then Apple80211Set(AUTH_TYPE)
            // then ASSOCIATE". Refusing the set was fatal and silent: /var/log/wifi.log
            // shows `Error Apple80211Set: Operation not supported seqNum 1 Total 2
            // chg 1 en1` on every `networksetup -setairportnetwork`, and airportd
            // then reported -3903 without ever sending ASSOCIATE (Ath10kRequestTrace
            // contained no `s20`). The SSID that matters for the join is re-sent
            // inside apple80211_assoc_data, so accept and remember it for diagnostics.
            const apple80211_ssid_data *ssid = static_cast<const apple80211_ssid_data *>(data);
            if (ssid == nullptr || ssid->ssid_len > APPLE80211_MAX_SSID_LEN)
                return kIOReturnBadArgument;
            const UInt32 bytes = ssid->ssid_len;
            bzero(_venturaPendingSSID, sizeof(_venturaPendingSSID));
            bcopy(ssid->ssid_bytes, _venturaPendingSSID, bytes);
            _venturaPendingSSIDLength = static_cast<UInt8>(bytes);
            if (IOService *probe = getProvider()) {
                probe->setProperty("Ath10kSetSSIDLength", static_cast<UInt64>(bytes), 32);
                probe->setProperty("Ath10kSetSSID",
                                   _venturaPendingSSID[0] ? (const char *)_venturaPendingSSID : "");
            }
            return kIOReturnSuccess;
        }
        apple80211_ssid_data *ssid = static_cast<apple80211_ssid_data *>(data);
        ssid->version = APPLE80211_VERSION;
        // Returning an empty SSID made `networksetup -getairportnetwork` print a
        // stray 0x01 and made configd log "en1: SSID ^A BSSID 01:00:..". The
        // associated BSS's SSID is already kept in the connection state, so
        // publish it: the menu bar name, System Settings and every client that
        // asks "which network am I on" read it from here.
        ssid->ssid_len = 0;
        bzero(ssid->ssid_bytes, sizeof(ssid->ssid_bytes));
        if (_connection.ssidLength && Ath10k::connectionAssociated(_connection)) {
            const UInt32 bytes = _connection.ssidLength > APPLE80211_MAX_SSID_LEN
                                     ? APPLE80211_MAX_SSID_LEN
                                     : _connection.ssidLength;
            bcopy(_connection.ssid, ssid->ssid_bytes, bytes);
            ssid->ssid_len = bytes;
        }
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_BSSID: {
        if (isSet)
            return kIOReturnUnsupported;
        apple80211_bssid_data *bssid = static_cast<apple80211_bssid_data *>(data);
        bssid->version = APPLE80211_VERSION;
        memset(&bssid->bssid, 0, sizeof(bssid->bssid));
        if (Ath10k::connectionAssociated(_connection))
            bcopy(_connection.bssid, &bssid->bssid, 6);
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_CHANNEL:
        return venturaChannel(data, isSet);
    case APPLE80211_IOC_CURRENT_NETWORK: {
        // live getCurrentNetwork (0xffffff8001fde40a) just forwards 0x67 (103) to
        // the driver through apple80211VirtualRequestIoctl and turns any failure
        // into 0x66/EOPNOTSUPP, which is the e00002c7 airportd was getting. The
        // caller's buffer is 0x48c bytes = version + num_bss + one
        // apple80211_scan_result, i.e. "the current BSS, shaped like a scan hit".
        if (isSet)
            return kIOReturnUnsupported;
        UInt8 *out = static_cast<UInt8 *>(data);
        if (out == nullptr)
            return kIOReturnBadArgument;
        const UInt32 capacity = _venturaIoctlLength >= 8 + sizeof(apple80211_scan_result)
                                    ? _venturaIoctlLength
                                    : static_cast<UInt32>(8 + sizeof(apple80211_scan_result));
        if (IOService *probe = getProvider())
            probe->setProperty("Ath10kCurrentNetReqLen",
                               static_cast<UInt64>(_venturaIoctlLength), 32);
        *reinterpret_cast<UInt32 *>(out) = APPLE80211_VERSION;
        const bool associated = Ath10k::connectionAssociated(_connection);
        *reinterpret_cast<UInt32 *>(out + 4) = associated ? 1 : 0;
        if (!associated)
            return kIOReturnSuccess;
        auto *result = reinterpret_cast<apple80211_scan_result *>(out + 8);
        bzero(result, sizeof(*result));
        result->version = APPLE80211_VERSION;
        const UInt32 channel = _connection.operatingChannel;
        result->asr_channel.version = APPLE80211_VERSION;
        result->asr_channel.channel = channel;
        result->asr_channel.flags = venturaScanChannelFlags(channel);
        const SInt16 noise = (_connection.fwStats.noiseValid &&
                              _connection.fwStats.noiseDbm > -128)
                                 ? static_cast<SInt16>(_connection.fwStats.noiseDbm)
                                 : static_cast<SInt16>(-95);
        result->asr_noise = noise;
        const int32_t dbm = _connection.signalDbm != INT32_MIN ? _connection.signalDbm
                                                              : noise + 30;
        result->asr_rssi = static_cast<SInt16>(dbm);
        result->asr_snr = static_cast<SInt16>(dbm - noise);
        result->asr_beacon_int = static_cast<SInt16>(_connection.ap.beaconInterval);
        result->asr_cap = static_cast<SInt16>(_connection.ap.capabilities);
        memcpy(result->asr_bssid, _connection.bssid, sizeof(result->asr_bssid));
        const UInt32 ssidBytes = _connection.ssidLength > APPLE80211_MAX_SSID_LEN
                                     ? APPLE80211_MAX_SSID_LEN
                                     : _connection.ssidLength;
        result->asr_ssid_len = static_cast<UInt8>(ssidBytes);
        memcpy(result->asr_ssid, _connection.ssid, ssidBytes);
        const UInt32 ieLen = _connection.ap.rsnPresent ? _connection.ap.rsnLength : 0;
        if (ieLen != 0 && capacity >= 8 + ATH10K_SCAN_IE_OFFSET + ieLen) {
            memcpy(out + 8 + ATH10K_SCAN_IE_OFFSET, _connection.ap.rsn, ieLen);
            result->asr_ie_len = static_cast<SInt16>(ieLen);
        }
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_RSN_IE: {
        // What an Apple driver answers here is the RSN element of the *current*
        // association; it is one of the 115 request types AirPort_BrcmNIC
        // implements (its dispatch table at 0x22b1cb4) and this driver answered
        // "unsupported", so any client asking for security details saw none.
        if (isSet)
            return kIOReturnSuccess;
        struct RsnIeReply {
            UInt32 version;
            UInt32 length;
            UInt8 element[257];
        } *reply = static_cast<RsnIeReply *>(data);
        if (reply == nullptr)
            return kIOReturnBadArgument;
        bzero(reply, sizeof(*reply));
        reply->version = APPLE80211_VERSION;
        if (Ath10k::connectionAssociated(_connection) && _connection.ap.rsnPresent &&
            _connection.ap.rsnLength <= sizeof(reply->element)) {
            bcopy(_connection.ap.rsn, reply->element, _connection.ap.rsnLength);
            reply->length = _connection.ap.rsnLength;
        }
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_RSN_CONF:
        // Accepted as a no-op: the driver installs its own RSN configuration from
        // the WPA2 state machine, and refusing it made clients believe the
        // interface cannot do RSN at all.
        return kIOReturnSuccess;
    case APPLE80211_IOC_STA_AUTHORIZE:
        // Peer authorize request from the supplicant path; the connection state
        // machine already authorizes the port itself once securityPhase reaches 5.
        return (Ath10k::connectionAssociated(_connection) && _connection.portAuthorized)
                   ? kIOReturnSuccess
                   : kIOReturnNotReady;
    case APPLE80211_IOC_CIPHER_KEY: {
        // Keys are installed by the in-kernel supplicant through the firmware, so a
        // client-issued key write has nothing to do. Acknowledge it rather than
        // returning unsupported, which reads as "this interface has no key path".
        if (isSet)
            return kIOReturnSuccess;
        return kIOReturnUnsupported;
    }
    case APPLE80211_IOC_RATE: {
        if (isSet)
            return kIOReturnUnsupported;
        apple80211_rate_data *rate = static_cast<apple80211_rate_data *>(data);
        rate->version = APPLE80211_VERSION;
        rate->num_radios = APPLE80211_MAX_RADIO;
        for (int i = 0; i < APPLE80211_MAX_RADIO; i++)
            rate->rate[i] = 0;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_RSSI: {
        if (isSet)
            return kIOReturnUnsupported;
        apple80211_rssi_data *rssi = static_cast<apple80211_rssi_data *>(data);
        rssi->version = APPLE80211_VERSION;
        rssi->num_radios = APPLE80211_MAX_RADIO;
        // The associated beacon's signal is already tracked; a constant 0 made the
        // menu bar draw an empty signal meter even while connected.
        const int32_t dbm = Ath10k::connectionAssociated(_connection) &&
                                    _connection.signalDbm != INT32_MIN
                                ? _connection.signalDbm
                                : 0;
        for (int i = 0; i < APPLE80211_MAX_RADIO; i++) {
            rssi->rssi[i] = dbm;
            rssi->rssi_ext[i] = 0;
        }
        rssi->aggregate_rssi = dbm;
        rssi->aggregate_rssi_ext = 0;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_CARD_CAPABILITIES:
        return venturaCapabilities(data);
    case APPLE80211_IOC_SUPPORTED_CHANNELS:
    case APPLE80211_IOC_HW_SUPPORTED_CHANNELS:
        return venturaSupportedChannels(data);
    case APPLE80211_IOC_DRIVER_VERSION:
    case APPLE80211_IOC_HARDWARE_VERSION:
        return venturaVersion(data);
    case APPLE80211_IOC_COUNTRY_CODE:
        return venturaCountryCode(data, isSet);
    case APPLE80211_IOC_SCAN_REQ:
    case APPLE80211_IOC_SCAN_REQ_MULTIPLE:
        return venturaScan(static_cast<apple80211_scan_data *>(data));
    case APPLE80211_IOC_SCAN_RESULT: {
        // Every reader gets its own position in the last sweep. airportd retries the
        // read inside one scan, a tool like apple80211-scan walks it once, and the
        // manager drains it while caching - one shared cursor cannot serve all three.
        int8_t *slot = venturaReaderSlot();
        if (slot != nullptr)
            _venturaScanIndex = static_cast<UInt8>(*slot);
        const IOReturn status =
            venturaScanResult(static_cast<apple80211_scan_result *>(data));
        if (slot != nullptr)
            *slot = static_cast<int8_t>(_venturaScanIndex);
        if (IOService *probe = getProvider()) {
            probe->setProperty("Ath10kCursorStatus",
                               static_cast<UInt64>(static_cast<UInt32>(status)), 32);
            // Only a served entry says anything: the loop always ends on NotFound,
            // and that call never touches the buffer.
            const auto *seen = static_cast<const apple80211_scan_result *>(data);
            if (status != kIOReturnSuccess)
                return status;
            char shape[96];
            snprintf(shape, sizeof(shape), "ch=%u fl=%x nr=%u ie=%u",
                     static_cast<unsigned>(seen->asr_channel.channel),
                     static_cast<unsigned>(seen->asr_channel.flags),
                     static_cast<unsigned>(seen->asr_nrates),
                     static_cast<unsigned>(seen->asr_ie_len));
            probe->setProperty("Ath10kResultShape", shape);
            probe->setProperty("Ath10kResultReqLen",
                               static_cast<UInt64>(_venturaIoctlLength), 32);
        }
        return status;
    }
    case APPLE80211_IOC_SCANCACHE_CLEAR:
        _venturaScanIndex = 0;
        return kIOReturnSuccess;
    case APPLE80211_IOC_VIRTUAL_IF_CREATE:
    case APPLE80211_IOC_VIRTUAL_IF_DELETE:
        // airportd asks for a virtual interface in the run-up to a join and gave up
        // with -3903 when this fell through to Apple's implementation (measured:
        // Ath10kIoctlFall = "type=94"). This driver already owns exactly one
        // attached station interface (en1, role 0), which is what the create would
        // produce, so acknowledge instead of building a second one; the role/parent
        // getters below already answer for that interface.
        if (IOService *probe = getProvider())
            probe->setProperty("Ath10kVirtualIfRequest",
                               requestNumber == APPLE80211_IOC_VIRTUAL_IF_CREATE ? "create"
                                                                                 : "delete");
        return kIOReturnSuccess;
    case APPLE80211_IOC_VIRTUAL_IF_ROLE:
    case APPLE80211_IOC_VIRTUAL_IF_PARENT:
        // The live family answers these from its own vif bookkeeping, and the ioctl
        // channel never reaches that code: performCommand lands straight in this
        // dispatcher. Measured consequence of refusing them: CoreWLAN asks
        // SUPPORTED_CHANNELS, BSSID, POWER and then VIRTUAL_IF_ROLE, gets
        // kIOReturnUnsupported and gives up with an empty list without ever
        // sending SCAN_REQ. Role 0 is the primary station interface (P2P/AWDL/
        // SoftAP are 1..5), and this interface has no virtual parent, so the
        // zero-filled buffer is the correct answer for both.
        return kIOReturnSuccess;
    case APPLE80211_IOC_ASSOCIATE:
        return venturaAssociate(static_cast<apple80211_assoc_data *>(data));
    case APPLE80211_IOC_DISASSOCIATE:
    case APPLE80211_IOC_DEAUTH:
        venturaDisassociate(requestNumber == APPLE80211_IOC_DEAUTH);
        return kIOReturnSuccess;
    case APPLE80211_IOC_ASSOCIATE_RESULT: {
        apple80211_assoc_result_data *result =
            static_cast<apple80211_assoc_result_data *>(data);
        result->version = APPLE80211_VERSION;
        result->result = 0;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_ASSOCIATION_STATUS: {
        apple80211_assoc_status_data *status =
            static_cast<apple80211_assoc_status_data *>(data);
        status->version = APPLE80211_VERSION;
        status->status = _venturaState;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_AUTH_TYPE: {
        // Was "return success, write nothing": the caller then read whatever was in
        // its buffer. airportd asks AUTH_TYPE in the run-up to a join and uses it to
        // decide which security the interface is in, so fill it from the live
        // connection: the upper-auth bitmap is exactly the AKM word we negotiated.
        if (isSet)
            return kIOReturnSuccess;
        struct AuthTypeReply {
            UInt32 version;
            UInt32 authType;
        } *reply = static_cast<AuthTypeReply *>(data);
        if (reply == nullptr)
            return kIOReturnBadArgument;
        UInt32 upper = APPLE80211_AUTHTYPE_NONE;
        if (_connection.ssidLength && Ath10k::connectionAssociated(_connection)) {
            switch (_connection.securityAKM) {
            case 2: upper = APPLE80211_AUTHTYPE_WPA2_PSK; break;
            case 6: upper = APPLE80211_AUTHTYPE_SHA256_PSK; break;
            case 8: upper = APPLE80211_AUTHTYPE_WPA3_SAE; break;
            default: upper = APPLE80211_AUTHTYPE_NONE; break;
            }
        }
        reply->version = APPLE80211_VERSION;
        reply->authType = upper;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_OP_MODE: {
        struct OpModeReply {
            UInt32 version;
            UInt32 opMode;
        } *reply = static_cast<OpModeReply *>(data);
        if (!isSet && reply != nullptr) {
            reply->version = APPLE80211_VERSION;
            reply->opMode = APPLE80211_M_STA;
        }
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_PHY_MODE:
    case APPLE80211_IOC_POWERSAVE:
    case APPLE80211_IOC_PROTMODE:
    case APPLE80211_IOC_TXPOWER:
    case APPLE80211_IOC_NOISE:
    case APPLE80211_IOC_LOCALE:
    case APPLE80211_IOC_TX_NSS:
    case APPLE80211_IOC_NSS:
    case APPLE80211_IOC_MCS:
    case APPLE80211_IOC_MCS_VHT:
    case APPLE80211_IOC_TX_ANTENNA:
    case APPLE80211_IOC_ANTENNA_DIVERSITY:
        return kIOReturnSuccess;
    default:
        // Every request type Apple's own driver implements is served here rather
        // than refused. The reverse-engineered dispatch table in
        // AirPort_BrcmNIC::apple80211Request (0xffffff80022b06a4, table at
        // 0xffffff80022b1cb4) implements 115 of the 265 accepted numbers; this
        // driver answers 45 specifically, and airportd walks through the rest in
        // the run-up to a join - each refusal ended the whole flow with -3903
        // before ASSOCIATE was ever sent.
        //
        // A getter gets its buffer zeroed for the length the caller declared (a
        // version-only or all-zero answer is what many clients expect from a
        // feature this radio does not have), and a setter is a no-op. The type is
        // published so anything that later proves to matter is visible instead of
        // silent.
        if (IOService *probe = getProvider()) {
            char note[48];
            snprintf(note, sizeof(note), "%s:%d", isSet ? "set" : "get", requestNumber);
            probe->setProperty("Ath10kUnknownReq", note);
        }
        if (!isSet && data != nullptr && _venturaIoctlLength >= 4 &&
            _venturaIoctlLength <= 0x800) {
            // Every apple80211_*_data reply starts with a version word, and a
            // client that reads 0 there treats the whole answer as invalid - which
            // is exactly how a "success" turns into a silent refusal. Stamp the
            // version and zero the rest.
            bzero(data, _venturaIoctlLength);
            *static_cast<UInt32 *>(data) = APPLE80211_VERSION;
        }
        return kIOReturnSuccess;
    }
}

// ---------------------------------------------------------------------------
// Staged helpers (radio wiring lands in the following stage)
// ---------------------------------------------------------------------------

IOReturn Ath10kPCIDevice::venturaPower(bool on)
{
    const bool changed = (_venturaPowerOn != on);
    _venturaPowerOn = on;
    venturaPublishState(APPLE80211_S_INIT);
    if (!on && changed) {
        // Power off has to take the link down, not just latch a flag: airportd
        // turns the radio off before every join attempt, and with the association
        // still standing (en1 kept its DHCP lease) the following ASSOCIATE was
        // refused with -3903 "Failed to join network". Cancelling through the
        // shared state machine is what actually stops the vdev, drops the peer and
        // clears the BSD link, and clearing the security state makes the next join
        // start from EAPOL instead of resuming a half-torn-down session.
        _connection.cancelRequested = true;
        _connection.portAuthorized = false;
        Ath10k::connectionFailure(_connection, "radio-off");
        _venturaState = APPLE80211_S_INIT;
        networkLink(false);
    }
    if (IOService *provider = getProvider())
        provider->setProperty("Ath10kPowerPhase", on ? "power-on-request" : "power-off-request");
    return kIOReturnSuccess;
}

IOReturn Ath10kPCIDevice::venturaScan(const apple80211_scan_data *request)
{
    (void)request;
    _venturaScanIndex = 0;
    ++_venturaScanRequests;
    // serviceWMIScan() is a one-shot: it returns as soon as it completes, so
    // without re-arming, airportd would be served the beacon set captured at
    // boot forever. Re-arm the shared state machine here (never hand-build a
    // WMI frame: the credit/DMA ownership lives in there) and keep the cache:
    // the previous BSS list stays servable while the fresh sweep runs.
    bool rearmed = false;
    if (_wmiScan.completed && !_wmiScan.txOwned && !_dmaQuarantine && _wmiLive.active) {
        _wmiScan.attempted = false;
        _wmiScan.submitted = false;
        _wmiScan.txOwned = false;
        _wmiScan.txConsumed = false;
        _wmiScan.creditReturned = false;
        _wmiScan.started = false;
        _wmiScan.terminal = false;
        _wmiScan.failed = false;
        _wmiScan.completed = false;
        _wmiScan.cancelRequested = false;
        _wmiScan.stopSubmitted = false;
        _wmiScan.stopConsumed = false;
        _wmiScan.eventCount = 0;
        _wmiScan.lastType = 0;
        _wmiScan.reason = 0;
        _wmiScan.frequency = 0;
        _wmiScan.mgmtCount = 0;
        _wmiScan.mgmtBytes = 0;
        _wmiScan.visitedMask = 0;
        _wmiScan.channelInfoCount = 0;
        _wmiScan.channelInfoErrors = 0;
        _wmiScan.channelInfoMask = 0;
        _wmiScan.elapsedMs = 0;
        _wmiScan.status = "rearmed-for-airportd";
        ++_wmiScan.generation;
        _wmiScan.id = 0xa000u | ((0x36u + _wmiScan.generation) & 0xfffu);
        ++_venturaScanRearms;
        rearmed = true;
    }
    if (IOService *probe = getProvider()) {
        probe->setProperty("Ath10kScanRequests", static_cast<UInt64>(_venturaScanRequests), 32);
        probe->setProperty("Ath10kScanRearms", static_cast<UInt64>(_venturaScanRearms), 32);
        probe->setProperty("Ath10kScanScanId", static_cast<UInt64>(_wmiScan.id), 32);
        probe->setProperty("Ath10kScanArmed", rearmed);
    }
    if (rearmed) {
        // Watch for the sweep to finish, then try to tell airportd (see runScanNotify).
        if (_scanNotifyCall == nullptr) {
            _scanNotifyCall = thread_call_allocate_with_options(
                scanNotifyCall, this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
        }
        if (_scanNotifyCall != nullptr) {
            _scanNotifyAttempts = 0;
            _scanNotifyGeneration = _wmiScan.generation;
            uint64_t deadline = 0;
            clock_interval_to_deadline(1, kSecondScale, &deadline);
            thread_call_enter_delayed(_scanNotifyCall, deadline);
        }
    }
    return kIOReturnSuccess;
}

// 80211 channel -> apple80211 flags. Everything we scan is a 20 MHz channel on
// a plan of passive/active entries; airportd only uses the band bit to sort the
// list, and the width bits to draw the badge.
static UInt32 venturaScanChannelFlags(UInt32 channel)
{
    UInt32 flags = APPLE80211_C_FLAG_ACTIVE | APPLE80211_C_FLAG_20MHZ;
    flags |= (channel <= 14) ? APPLE80211_C_FLAG_2GHZ : APPLE80211_C_FLAG_5GHZ;
    return flags;
}

extern "C" int proc_selfpid(void);

// Who reads the sweep? Both the family's scan manager (a kernel thread) and the
// client's own Apple80211 read drain it, and only the pid tells them apart. The
// last few served entries carry "pid:index" so one client run says whether the
// caller that ends up with zero networks is the one that actually got rows.
static char gVenturaServeTrace[8][20];
static UInt32 gVenturaServeTraceNext = 0;
static UInt32 gVenturaServeTraceCount = 0;

static void venturaTraceServe(UInt32 index)
{
    snprintf(gVenturaServeTrace[gVenturaServeTraceNext], sizeof(gVenturaServeTrace[0]), "%d:%u",
             proc_selfpid(), static_cast<unsigned>(index));
    gVenturaServeTraceNext = (gVenturaServeTraceNext + 1) % 8;
    if (gVenturaServeTraceCount < 8)
        ++gVenturaServeTraceCount;
}

// Per-process position in the last sweep, keyed by pid. getScanResult() in the
// family keys its own cache the same way (proc_selfpid), and a shared cursor left
// airportd reading past the end while a tool had already walked the list.
int8_t *Ath10kPCIDevice::venturaReaderSlot()
{
    const int pid = proc_selfpid();
    for (UInt32 i = 0; i < kVenReaderSlots; ++i) {
        if (_venturaReaders[i].pid == pid)
            return &_venturaReaders[i].index;
    }
    // Reuse the slot of the oldest pid, which is almost always a finished tool.
    UInt32 victim = 0;
    UInt32 oldest = _venturaReaders[0].stamp;
    for (UInt32 i = 1; i < kVenReaderSlots; ++i) {
        if (_venturaReaders[i].stamp < oldest) {
            oldest = _venturaReaders[i].stamp;
            victim = i;
        }
    }
    _venturaReaders[victim].pid = pid;
    _venturaReaders[victim].index = 0;
    _venturaReaders[victim].stamp = ++_venturaReaderStamp;
    return &_venturaReaders[victim].index;
}

IOReturn Ath10kPCIDevice::venturaScanResult(apple80211_scan_result *result)
{
    if (result == nullptr)
        return kIOReturnBadArgument;
    memset(result, 0, sizeof(*result));
    result->version = APPLE80211_VERSION;

    // Cursor protocol: one BSS per call, end signalled with a non-zero return.
    // airportd stops reading at the first failure, so handing it the whole
    // array in a single call leaves every entry after the first unread.
    while (_venturaScanIndex < _wmiBSS.count &&
           _wmiBSS.entries[_venturaScanIndex].ssidLength == 0)
        ++_venturaScanIndex;
    if (_venturaScanIndex >= _wmiBSS.count) {
        ++_venturaScanEnds;
        if (IOService *probe = getProvider()) {
            probe->setProperty("Ath10kScanServed", static_cast<UInt64>(_venturaScanServed), 32);
            probe->setProperty("Ath10kScanEnds", static_cast<UInt64>(_venturaScanEnds), 32);
            probe->setProperty("Ath10kScanCached", static_cast<UInt64>(_wmiBSS.count), 32);
        }
        // End of list. airportd propagates whatever this returns straight into its
        // scan verdict ("Failed to perform Wi-Fi scan, returned error code X"), and
        // measured: kIOReturnNotFound (0xe00002f0) turns a full, successful sweep
        // into a failed scan with zero networks. EIO (5) is the end-of-list marker
        // the classic ioctl path already produces and the one itlwm uses; the client
        // reads it as "no more results", not as an error.
        return EIO;
    }

    const Ath10k::WMIBSS &entry = _wmiBSS.entries[_venturaScanIndex++];
    const UInt32 channel = entry.advertisedChannel != 0 ? entry.advertisedChannel
                                                        : entry.rxChannel;
    result->asr_channel.version = APPLE80211_VERSION;
    result->asr_channel.channel = channel;
    result->asr_channel.flags = venturaScanChannelFlags(channel);
    // The management event only carries SNR, so the noise floor has to come from
    // the firmware stats request; until that lands, use a typical QCA floor so
    // the UI does not show a signal of 0 dBm (which sorts every AP as strongest).
    const SInt16 noise = (_connection.fwStats.noiseValid &&
                          _connection.fwStats.noiseDbm > -128)
                             ? static_cast<SInt16>(_connection.fwStats.noiseDbm)
                             : static_cast<SInt16>(-95);
    const SInt16 snr = static_cast<SInt16>(entry.snr > 127 ? 127 : entry.snr);
    result->asr_noise = noise;
    result->asr_snr = snr;
    result->asr_rssi = static_cast<SInt16>(noise + snr);
    result->asr_beacon_int = static_cast<SInt16>(entry.beaconInterval);
    result->asr_cap = static_cast<SInt16>(entry.capabilities);
    memcpy(result->asr_bssid, entry.bssid, sizeof(result->asr_bssid));
    result->asr_ssid_len = entry.ssidLength;
    memcpy(result->asr_ssid, entry.ssid, entry.ssidLength);
    // asr_rates are 500 kbps units; the beacon carries the 0x80 "basic" flag.
    UInt8 rates = 0;
    for (UInt32 i = 0; i < entry.rateCount && rates < APPLE80211_MAX_RATES; ++i)
        result->asr_rates[rates++] = entry.rates[i] & 0x7fu;
    result->asr_nrates = rates;
    result->asr_age = 0; // served live, not from a persisted cache

    // The RSN element goes into asr_ie_data. Which shape that member has depends
    // on the caller's ABI, not on our header: macOS 12+ replaced the pointer with
    // a 1024-byte inline array packed immediately after asr_ie_len, and only the
    // family itself knows which one airportd compiled against. req_len is the
    // only honest signal, so nothing is written beyond what the caller asked for.
    const UInt32 ieLen = entry.rsnPresent ? entry.rsnLength : 0;
    if (ieLen != 0 && _venturaIoctlLength >= ATH10K_SCAN_IE_OFFSET + ieLen) {
        memcpy(reinterpret_cast<UInt8 *>(result) + ATH10K_SCAN_IE_OFFSET,
               entry.rsn, entry.rsnLength);
        result->asr_ie_len = static_cast<SInt16>(ieLen);
    }

    ++_venturaScanServed;
    venturaTraceServe(static_cast<UInt32>(_venturaScanIndex - 1));
    if (IOService *probe = getProvider()) {
        probe->setProperty("Ath10kScanServed", static_cast<UInt64>(_venturaScanServed), 32);
        probe->setProperty("Ath10kScanEnds", static_cast<UInt64>(_venturaScanEnds), 32);
        probe->setProperty("Ath10kScanCached", static_cast<UInt64>(_wmiBSS.count), 32);
        probe->setProperty("Ath10kScanIeLen", static_cast<UInt64>(ieLen), 32);
        probe->setProperty("Ath10kScanReqLen", static_cast<UInt64>(_venturaIoctlLength), 32);
        char trace[8 * 22];
        trace[0] = 0;
        const UInt32 first = (gVenturaServeTraceCount == 8) ? gVenturaServeTraceNext : 0;
        for (UInt32 i = 0; i < gVenturaServeTraceCount; ++i) {
            strlcat(trace, gVenturaServeTrace[(first + i) % 8], sizeof(trace));
            strlcat(trace, " ", sizeof(trace));
        }
        probe->setProperty("Ath10kScanServeTrace", trace);
    }
    return kIOReturnSuccess;
}

IOReturn Ath10kPCIDevice::venturaAssociate(const apple80211_assoc_data *request)
{
    IOService *probe = getProvider();
    if (request != nullptr && probe != nullptr) {
        // What the request actually carried, before any validation: an "open join"
        // against a protected AP is indistinguishable from a wrong struct offset
        // unless the three fields are published.
        char raw[128];
        snprintf(raw, sizeof(raw), "ver=%u len=%u upper=%04x lower=%04x keylen=%u rsnlen=%u",
                 static_cast<unsigned>(request->version),
                 static_cast<unsigned>(request->ad_ssid_len),
                 static_cast<unsigned>(request->ad_auth_upper),
                 static_cast<unsigned>(request->ad_auth_lower),
                 static_cast<unsigned>(request->ad_key.key_len),
                 static_cast<unsigned>(request->ad_rsn_ie_len));
        probe->setProperty("Ath10kAssocRaw", raw);
    }
    if (request == nullptr || !_wmiLive.active)
        return kIOReturnBadArgument;
    const UInt32 ssidLength = request->ad_ssid_len;
    if (ssidLength == 0 || ssidLength > APPLE80211_MAX_SSID_LEN)
        return kIOReturnBadArgument;

    // The family names the BSS it wants. A zeroed BSSID means "any AP with this
    // SSID", which this driver resolves against its own beacon cache: the family's
    // scan cache is not reachable from here.
    UInt8 bssid[6] = {0, 0, 0, 0, 0, 0};
    UInt8 nonzero = 0;
    for (UInt32 i = 0; i < 6; ++i) {
        bssid[i] = request->ad_bssid.octet[i];
        nonzero |= bssid[i];
    }
    SInt32 bestSnr = -1;
    if (nonzero == 0) {
        for (UInt32 i = 0; i < _wmiBSS.count; ++i) {
            const Ath10k::WMIBSS &entry = _wmiBSS.entries[i];
            if (entry.ssidLength != ssidLength)
                continue;
            bool same = true;
            for (UInt32 j = 0; j < ssidLength; ++j)
                if (entry.ssid[j] != request->ad_ssid[j])
                    same = false;
            if (!same)
                continue;
            const SInt32 snr = static_cast<SInt32>(entry.snr);
            if (snr <= bestSnr)
                continue;
            bestSnr = snr;
            for (UInt32 j = 0; j < 6; ++j)
                bssid[j] = entry.bssid[j];
        }
        if (bestSnr < 0) {
            if (probe != nullptr)
                probe->setProperty("Ath10kAssocPhase", "bss-not-in-cache");
            return kIOReturnNotFound;
        }
    }

    // WPA2/WPA2-SHA256 personal: derive the PMK here and let the in-kernel
    // supplicant answer the four-way handshake once the BSS is committed.
    // Enterprise (802.1X) has no supplicant in this personality yet and SAE is
    // handled by its own exchange, so both are still refused with a breadcrumb.
    const bool wantsKey = (request->ad_auth_upper != 0) ||
                          (request->ad_key.key_len != 0) ||
                          (request->ad_rsn_ie_len != 0);
    UInt8 akm = 2;
    if (wantsKey) {
        // Bit 3 is WPA2-PSK (RSN AKM 2), bit 10 is PSK-SHA256 (AKM 6). Both share
        // the PBKDF2 PMK; only the descriptor differs and the supplicant picks it
        // from the validated RSN element.
        if (request->ad_auth_upper & (1u << 10))
            akm = 6;
        else if (request->ad_auth_upper & (1u << 3))
            akm = 2;
        else if (request->ad_auth_upper & (0x100u << 0)) {
            if (probe != nullptr)
                probe->setProperty("Ath10kAssocPhase", "enterprise-not-supported");
            return kIOReturnUnsupported;
        }
    }

    auto &s = _connection;
    s.ssidLength = static_cast<UInt8>(ssidLength);
    for (UInt32 i = 0; i < sizeof(s.ssid); ++i)
        s.ssid[i] = i < ssidLength ? request->ad_ssid[i] : 0;
    for (UInt32 i = 0; i < 6; ++i)
        s.bssid[i] = bssid[i];
    for (UInt32 i = 0; i < 6; ++i)
        s.local[i] = _wmiInit.mac[i];
    s.hiddenSSID = false;
    s.secure = wantsKey;
    s.securityAKM = wantsKey ? akm : 0;
    s.portAuthorized = !wantsKey;
    if (wantsKey) {
        // Key material first: a failure here must refuse the join instead of
        // associating and then failing every handshake frame.
        const IOReturn armed = venturaSecurityArm(request->ad_key.key, request->ad_key.key_len, akm);
        if (armed != kIOReturnSuccess)
            return armed;
    } else {
        venturaSecurityStop();
    }
    if (s.txPowerHalfDbm == 0 || s.txPowerHalfDbm > 40)
        s.txPowerHalfDbm = 20;   // 10 dBm; prepareBSSCandidate clamps to the AP limit
    s.cancelRequested = false;
    // Never null: publishConnection() hands this straight to setProperty(), and
    // OSSymbol::withCString(NULL) faults in strnlen (measured panic at Start).
    s.error = "none";
    s.disconnect = Ath10k::DisconnectStage::None;
    // A cancelled/failed sweep blocks the Scan stage forever (`_wmiScan.completed`
    // is its entry condition), which turned every join after a service bounce into
    // a 5 s stage timeout. The beacon cache is still the last sweep, so reuse it
    // instead of wiping it: selectOpenBSS() only needs `_wmiBSS`, and a join that
    // targets a BSS we no longer hold fails with "configured-bss-not-found" anyway.
    if (!_wmiScan.completed && _wmiBSS.count) {
        _wmiScan.completed = true;
        _wmiScan.failed = false;
        _wmiScan.cancelRequested = false;
        _wmiScan.status = "cache-reused-for-join";
        if (probe != nullptr)
            probe->setProperty("Ath10kAssocScanRearm", "cache-reuse");
    } else if (_wmiScan.completed && (_wmiScan.failed || _wmiScan.cancelRequested)) {
        (void)venturaScan(nullptr);
        if (probe != nullptr)
            probe->setProperty("Ath10kAssocScanRearm", "yes");
    }
    s.stage = Ath10k::LinkStage::Scan;
    _venturaState = APPLE80211_S_SCAN;
    if (probe != nullptr) {
        char phase[96];
        snprintf(phase, sizeof(phase), "requested bssid=%02x:%02x:%02x:%02x:%02x:%02x",
                 bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
        probe->setProperty("Ath10kAssocPhase", phase);
        char ssid[33];
        for (UInt32 i = 0; i < ssidLength; ++i)
            ssid[i] = static_cast<char>(request->ad_ssid[i]);
        ssid[ssidLength] = 0;
        probe->setProperty("Ath10kAssocSSID", ssid);
    }
    IOLog("Ath10kMac: ASSOCIATE request ssid-len=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x open\n",
          static_cast<unsigned>(ssidLength), bssid[0], bssid[1], bssid[2], bssid[3], bssid[4],
          bssid[5]);
    return kIOReturnSuccess;
}

void Ath10kPCIDevice::venturaDisassociate(bool)
{
    venturaPublishState(APPLE80211_S_INIT);
}

SInt32 Ath10kPCIDevice::venturaChannel(void *data, bool isSet)
{
    apple80211_channel_data *channel = static_cast<apple80211_channel_data *>(data);
    if (isSet)
        return kIOReturnUnsupported;
    channel->version = APPLE80211_VERSION;
    channel->channel.version = APPLE80211_VERSION;
    channel->channel.channel = 0;
    channel->channel.flags = 0;
    return kIOReturnSuccess;
}

SInt32 Ath10kPCIDevice::venturaCapabilities(void *data)
{
    // apple80211 expects a packed phase-1 capability octet string, not a cipher
    // list. The bits below advertise 802.11a/b/g/n/ac plus WEP/TKIP/CCMP.
    apple80211_capability_data *capabilities =
        static_cast<apple80211_capability_data *>(data);
    memset(capabilities, 0, sizeof(*capabilities));
    capabilities->version = APPLE80211_VERSION;
    UInt8 *bits = capabilities->capabilities;
    bits[0] = 0x0f;   // 802.11a/b/g/n
    bits[1] = APPLE80211_CAPINFO_ESS >> 8;
    // Capability #22. IO80211ScanManager::initWithControllerAndSkywalkInterface()
    // (live 0x2010568) asks for CARD_CAPABILITIES and refuses to build the manager
    // unless byte 6 of the reply - caps[2] - has bit 0x40 set: with it clear the
    // family answers 0x66 and airportd's scan has no cache to land in.
    bits[2] |= 0x40;
    bits[4] = 0x01;   // WEP40
    bits[5] = 0x01;   // WEP104
    bits[6] = 0x01;   // TKIP
    bits[7] = 0x01;   // CCMP
    bits[8] = 0x00;   // no WPA3 yet
    bits[10] = 0x01;  // 802.11ac
    return kIOReturnSuccess;
}

SInt32 Ath10kPCIDevice::venturaSupportedChannels(void *data)
{
    apple80211_sup_channel_data *channels = static_cast<apple80211_sup_channel_data *>(data);
    channels->version = APPLE80211_VERSION;
    // Report the channels the scan plan actually sweeps. An empty list is not
    // neutral: measured with num_channels=0, CoreWLAN asks POWER/BSSID/ROLE and
    // then returns an empty network list without ever sending SCAN_REQ, so the UI
    // never sees an access point at all.
    UInt32 count = 0;
    for (UInt32 i = 0; i < Ath10k::kWMIScanChannelCount && count < APPLE80211_MAX_CHANNELS;
         ++i) {
        const UInt32 channel = Ath10k::kWMIScanPlan[i].channel;
        if (!Ath10k::wmiScanChannelAllowed(channel))
            continue;
        channels->supported_channels[count].version = APPLE80211_VERSION;
        channels->supported_channels[count].channel = channel;
        channels->supported_channels[count].flags = venturaScanChannelFlags(channel);
        ++count;
    }
    channels->num_channels = count;
    if (IOService *probe = getProvider())
        probe->setProperty("Ath10kSupportedChannels", static_cast<UInt64>(count), 32);
    return kIOReturnSuccess;
}

SInt32 Ath10kPCIDevice::venturaVersion(void *data)
{
    apple80211_version_data *info = static_cast<apple80211_version_data *>(data);
    info->version = APPLE80211_VERSION;
    strlcpy(info->string, "0.0.105-ventura", sizeof(info->string));
    info->string_len = static_cast<u_int16_t>(strlen(info->string));
    return kIOReturnSuccess;
}

SInt32 Ath10kPCIDevice::venturaCountryCode(void *data, bool isSet)
{
    apple80211_country_code_data *country =
        static_cast<apple80211_country_code_data *>(data);
    if (isSet)
        return kIOReturnSuccess;
    country->version = APPLE80211_VERSION;
    memset(country->cc, 0, sizeof(country->cc));
    return kIOReturnSuccess;
}

// Hand the controller's own virtual interface back to it as the infra
// interface. Airportd and IO80211APIUserClient both cast their provider to
// IO80211SkywalkInterface, which the v1 interface can never satisfy; a subclass
// of our own is rejected by OpenCore at injection time.
//
// The vif itself is already created by the start() path (station role 7), so this
// adds exactly one undefined symbol - the controller's attachInterface overload.
// The role that overload checks ([ivars+0x50], ivars = [iface+0x110]) is written
// directly: setInterfaceRole/getInterfaceRole are exported several times in the
// KC and referencing them is not needed for this.
extern "C" bool ath10kAttachSkywalkInterface(IO80211Controller *self, OSObject *iface,
                                             IOService *service)
    __asm("__ZN17IO80211Controller15attachInterfaceEP8OSObjectP9IOService");

// Apple's in-kernel RSN supplicant (live IO80211Family's RSNSupplicant class) is
// gated by this one predicate. Disassembly of the live implementation
// (0xffffff8001fed798) is only:
//   mov rax,[rsi+0x118]      ; the vif's Skywalk ivars
//   mov al,[rax+0x118]       ; RSN-conf byte
//   and al,2 ; shr al,1 ; ret
// i.e. bit 1 of the dword that APPLE80211_IOC_OFFLOAD_RSN (177) writes into the
// same ivars (handler at 0xffffff8002038e2c stores the request's upper dword
// there, then forwards the set to the driver). airportd never sends 177 to this
// driver, so the bit stays 0 and the family keeps its supplicant switched off.
extern "C" bool ath10kUseAppleRSN(IO80211Controller *self, IO80211VirtualInterface *iface)
    __asm("__ZN17IO80211Controller21useAppleRSNSupplicantEP23IO80211VirtualInterface");

// The family's own request path copies the caller's apple80211req in itself, which
// is why airportd's join reached this slot with a *user* address in the third
// argument (measured Ath10kEntryTrace: 1:c03069c9:559930376:65535). Nothing here
// can dereference that pointer, so the request has to be copied in first; both
// helpers exist in the live bundle and return 0 on success.
extern "C" int ath10kControllerCopyIn(IO80211Controller *self, UInt64 address, void *to,
                                      size_t bytes)
    __asm("__ZN17IO80211Controller6copyInEyPvm");
extern "C" int ath10kControllerCopyOut(IO80211Controller *self, const void *from, UInt64 address,
                                       size_t bytes)
    __asm("__ZN17IO80211Controller7copyOutEPKvym");

// IO80211VirtualInterface::start() is a tail call into
// IO80211SkywalkInterface::start(); that body is the only code in the kernel
// cache that installs the event-pipe array:
//     [ivars+0x208] = OSArray::withCapacity(0x18)      (capstone, live 0x200043a)
// IO80211SkywalkInterface::createEventPipe() dereferences that slot and
// IO80211APIUserClient's event subscription runs through it, so a virtual
// interface that was only init()ed - which is all createVirtualInterface() does -
// can never deliver an event. Measured before this call: Ath10kVifEventPipe=0.
extern "C" bool ath10kVifStart(IO80211VirtualInterface *self, IOService *provider)
    __asm("__ZN23IO80211VirtualInterface5startEP9IOService");

// ---------------------------------------------------------------------------
// IO80211ScanManager wiring
//
// The live family keeps its scan cache in IO80211ScanManager and airportd never
// talks to the driver about results directly: `setScanRequest` forwards to the
// manager, the manager calls the driver back (SCAN_REQ), and on completion its
// `scanDone` pulls SCAN_RESULT until the driver answers non-zero, caches every
// entry and then posts the event airportd waits for. A vif whose
// `getScanManager()` is the live stub (xor eax,eax; ret) therefore can serve
// perfect results and still time airportd out with -3905.
//
// Two facts from the live KC (0x20037a8, 0x2010568, 0x2013fba) make this
// reachable without subclassing Apple's classes:
//   * getScanManager is vtable slot 373 (vptr+0xb98) and reads no ivars at all,
//     so a private vtable copy with that one entry replaced is enough;
//   * initWithControllerAndSkywalkInterface() only needs getWorkQueue(), a
//     provider for the interface, and a CARD_CAPABILITIES answer whose byte 6
//     has bit 0x40 (capability #22) set.
extern "C" char ath10kScanManagerMetaClass[] __asm("__ZN18IO80211ScanManager10gMetaClassE");
extern "C" OSObject *ath10kScanManagerMetaClassAlloc(const void *self)
    __asm("__ZNK18IO80211ScanManager9MetaClass5allocEv");
extern "C" bool ath10kScanManagerInitWithInterface(OSObject *self, IO80211Controller *controller,
                                                   IOService *iface)
    __asm("__ZN18IO80211ScanManager37initWithControllerAndSkywalkInterfaceEP17IO80211ControllerP23IO80211SkywalkInterface");
extern "C" SInt32 ath10kScanManagerStartScan(OSObject *self, const apple80211_scan_data *data)
    __asm("__ZN18IO80211ScanManager9startScanEP20apple80211_scan_data");
extern "C" void ath10kScanManagerScanDone(OSObject *self, bool done)
    __asm("__ZN18IO80211ScanManager8scanDoneEb");
extern "C" IOReturn ath10kScanManagerGetResult(OSObject *self, apple80211_scan_result **result)
    __asm("__ZN18IO80211ScanManager13getScanResultEPP22apple80211_scan_result");
// Reads exactly the path the family uses: getPrimarySkywalkInterface() then
// [vptr+0xb98] (live 0x1fef034), i.e. the slot patched below.
extern "C" void *ath10kGetPrimaryInterfaceScanManager(IO80211Controller *self)
    __asm("__ZN17IO80211Controller30getPrimaryInterfaceScanManagerEv");
// Cache ages. scanDone() arms its purge timer from [ivars+8] (live 0x2013fba:
// `imul esi, dword ptr [rax+8], 0x3e8`), so a zero there means the cache it just
// filled is emptied on the next timer tick and a client that asks one millisecond
// later finds nothing. setCacheAgeThreshold() writes that same field and is gated
// on procIsSuperuser(); the per-client ages live at [ivars+0xc] / [ivars+0x14].
extern "C" UInt32 ath10kScanManagerCacheAgeThresh(OSObject *self)
    __asm("__ZN18IO80211ScanManager14cacheAgeThreshEv");
extern "C" UInt32 ath10kScanManagerDirectAgeThresh(OSObject *self)
    __asm("__ZN18IO80211ScanManager15directAgeThreshEv");
extern "C" UInt32 ath10kScanManagerBroadcastAgeThresh(OSObject *self)
    __asm("__ZN18IO80211ScanManager18broadcastAgeThreshEv");
extern "C" SInt32 ath10kScanManagerSetCacheAgeThreshold(OSObject *self, UInt32 seconds)
    __asm("__ZN18IO80211ScanManager20setCacheAgeThresholdEj");
extern "C" void ath10kScanManagerSetDirectAgeThresh(OSObject *self, UInt32 seconds)
    __asm("__ZN18IO80211ScanManager18setDirectAgeThreshEj");
extern "C" void ath10kScanManagerSetBroadcastAgeThresh(OSObject *self, UInt32 seconds)
    __asm("__ZN18IO80211ScanManager21setBroadcastAgeThreshEj");

// One manager for the whole controller; the thunk below is what the patched
// vtable slot returns, so it has to be callable as a member function (this in
// rdi, pointer out in rax). The storage itself is declared at the top of the file.
static OSObject *venturaScanManagerThunk(OSObject *)
{
    return gVenturaScanManager;
}

bool Ath10kPCIDevice::setupVenturaScanManager()
{
    IOService *probe = getProvider();
    IO80211VirtualInterface *vif = _venturaVif;
    if (probe == nullptr || vif == nullptr)
        return false;
    if (gVenturaScanManager != nullptr)
        return true;

    OSObject *manager = ath10kScanManagerMetaClassAlloc(ath10kScanManagerMetaClass);
    if (manager == nullptr) {
        probe->setProperty("Ath10kScanMgrInit", "alloc-failed");
        return false;
    }
    const bool inited = ath10kScanManagerInitWithInterface(manager, this, vif);
    probe->setProperty("Ath10kScanMgrInit", inited ? "ok" : "init-failed");
    if (!inited) {
        manager->release();
        return false;
    }
    if (gVenturaScanSlot == nullptr)
        gVenturaScanSlot = static_cast<apple80211_scan_result *>(IOMalloc(sizeof(*gVenturaScanSlot)));
    if (gVenturaScanSlot == nullptr) {
        manager->release();
        probe->setProperty("Ath10kScanMgrInit", "no-slot");
        return false;
    }
    gVenturaScanManager = manager;
    _venturaScanManager = manager;

    // Replace exactly one vtable entry. The copy holds the already-relocated
    // pointers Apple's own table has, so every other slot stays byte-identical.
    void **vptr = *reinterpret_cast<void ***>(vif);
    const size_t slots = kVenturaVtableCopySlots;
    void **copy = static_cast<void **>(IOMalloc(slots * sizeof(void *)));
    if (copy == nullptr) {
        probe->setProperty("Ath10kScanMgrPatch", "no-memory");
        return false;
    }
    memcpy(copy, vptr, slots * sizeof(void *));
    // vptr points at vtable[2]; slot 373 is therefore copy[371]. (Every vtable
    // offset in this file uses that -2 correction.)
    copy[kVenturaVifGetScanManagerSlot - 2] = reinterpret_cast<void *>(&venturaScanManagerThunk);
    _venturaVifVtableCopy = copy;
    *reinterpret_cast<void ***>(vif) = copy;
    probe->setProperty("Ath10kScanMgrPatch", "patched");
    probe->setProperty("Ath10kScanMgr",
                       static_cast<UInt64>(reinterpret_cast<uintptr_t>(manager)), 64);
    if (ath10kGetPrimaryInterfaceScanManager(this) != nullptr)
        probe->setProperty("Ath10kScanMgrVisible", "yes");
    else
        probe->setProperty("Ath10kScanMgrVisible", "no");
    // What the purge timer will actually use, read before anything touches it.
    const UInt32 cacheAge = ath10kScanManagerCacheAgeThresh(manager);
    const UInt32 directAge = ath10kScanManagerDirectAgeThresh(manager);
    const UInt32 broadcastAge = ath10kScanManagerBroadcastAgeThresh(manager);
    char ages[64];
    snprintf(ages, sizeof(ages), "cache=%u direct=%u bcast=%u", cacheAge, directAge,
             broadcastAge);
    probe->setProperty("Ath10kScanMgrAges", ages);
    if (cacheAge == 0 || cacheAge > 3600) {
        ath10kScanManagerSetCacheAgeThreshold(manager, 30);
        probe->setProperty("Ath10kScanMgrAgeSet", "cache=30");
    }
    if (directAge == 0)
        ath10kScanManagerSetDirectAgeThresh(manager, 30);
    if (broadcastAge == 0)
        ath10kScanManagerSetBroadcastAgeThresh(manager, 300);
    char agesAfter[64];
    snprintf(agesAfter, sizeof(agesAfter), "cache=%u direct=%u bcast=%u",
             ath10kScanManagerCacheAgeThresh(manager),
             ath10kScanManagerDirectAgeThresh(manager),
             ath10kScanManagerBroadcastAgeThresh(manager));
    probe->setProperty("Ath10kScanMgrAgesAfter", agesAfter);
    IOLog("Ath10kMac: scan manager %p installed on vif %p\n", manager, vif);
    return true;
}

// Routes the two scan types the family would otherwise answer itself; returns
// kIOReturnUnsupported for everything else (and when there is no manager), which
// is the caller's signal to fall back to the driver's own result cursor.
SInt32 Ath10kPCIDevice::venturaScanManagerRequest(int type, void *data)
{
    if (_venturaScanManager == nullptr || data == nullptr)
        return kIOReturnUnsupported;
    // Only SCAN_REQ is routed. getScanResult() answers strictly by proc_selfpid
    // against the pending node list (live 0x2013b84), and the family dispatches the
    // request on its own workloop thread, so a routed read can only ever come back 5
    // and would shadow the driver's cursor. Results are served below instead.
    const bool isScanRequest = type == APPLE80211_IOC_SCAN_REQ ||
                               type == APPLE80211_IOC_SCAN_REQ_MULTIPLE;
    if (!isScanRequest)
        return kIOReturnUnsupported;
    IOService *probe = getProvider();
    SInt32 routed = kIOReturnUnsupported;
    // The manager scans and pulls results by calling straight back into this driver
    // through the Skywalk request slot, which lands in venturaDispatchRequest again;
    // the flag keeps that call from being routed back into the manager.
    _venturaManagerCall = true;
    if (isScanRequest) {
        // startScan() runs validateScanData(), allocates the pending request node (the
        // pid-keyed record getScanResult() matches on) and then calls this driver back
        // through the Skywalk request slot for the real SCAN_REQ.
        const SInt32 status = ath10kScanManagerStartScan(
            _venturaScanManager, static_cast<const apple80211_scan_data *>(data));
        if (probe != nullptr)
            probe->setProperty("Ath10kScanMgrStart", status == 0 ? "ok" : "failed");
        if (status == 0)
            routed = kIOReturnSuccess;
    } else {
        apple80211_scan_result *result = nullptr;
        const IOReturn status = ath10kScanManagerGetResult(_venturaScanManager, &result);
        if (probe != nullptr) {
            char seen[64];
            snprintf(seen, sizeof(seen), "st=%x %s", static_cast<unsigned>(status),
                     result != nullptr ? "entry" : "null");
            probe->setProperty("Ath10kScanMgrGet", seen);
        }
        if (status == kIOReturnSuccess && result != nullptr) {
            memcpy(data, result, sizeof(apple80211_scan_result));
            ++_venturaScanMgrServes;
            if (probe != nullptr)
                probe->setProperty("Ath10kScanMgrServes",
                                   static_cast<UInt64>(_venturaScanMgrServes), 32);
            routed = kIOReturnSuccess;
        }
    }
    _venturaManagerCall = false;
    return routed;
}

IOService *Ath10kPCIDevice::attachPlatformVif()
{
    IOService *probe = getProvider();
    IO80211VirtualInterface *vif = _venturaVif;
    if (vif == nullptr) {
        if (probe) probe->setProperty("Ath10kInfraPhase", "no-vif");
        return nullptr;
    }
    void **ivarsSlot = reinterpret_cast<void **>(reinterpret_cast<char *>(vif) + 0x110);
    // Apple's start() first, while the vif still carries the role
    // createVirtualInterface() gave it. Everything below depends on its ivars.
    const bool vifStarted = ath10kVifStart(vif, this);
    if (*ivarsSlot != nullptr) {
        _venturaEventPipe = *reinterpret_cast<void **>(
            reinterpret_cast<char *>(*ivarsSlot) + 0x208);
    }
    if (probe) {
        probe->setProperty("Ath10kVifStart", vifStarted ? "ok" : "failed");
        probe->setProperty("Ath10kVifEventPipe",
                           static_cast<UInt64>(reinterpret_cast<uintptr_t>(_venturaEventPipe)), 64);
    }
    // Turn Apple's in-kernel RSN supplicant on and record whether the predicate
    // flipped. The gate reads [vif+0x118] -> ivars, then byte [ivars+0x118], so
    // this is a *different* ivars pointer than the 0x110 one holding the role.
    // airportd never sends APPLE80211_IOC_OFFLOAD_RSN(177) to this driver, so
    // the family otherwise keeps RSNSupplicant switched off for our interface.
    {
        void **skywalkIvars = reinterpret_cast<void **>(reinterpret_cast<char *>(vif) + 0x118);
        UInt32 rsnBefore = 0;
        const bool gateBefore = ath10kUseAppleRSN(this, vif);
        if (*skywalkIvars != nullptr) {
            volatile UInt32 *rsnConf = reinterpret_cast<volatile UInt32 *>(
                reinterpret_cast<char *>(*skywalkIvars) + 0x118);
            rsnBefore = *rsnConf;
            *rsnConf = rsnBefore | 2;
        }
        const bool gateAfter = ath10kUseAppleRSN(this, vif);
        if (probe) {
            probe->setProperty("Ath10kRSNConfIvars",
                               static_cast<UInt64>(reinterpret_cast<uintptr_t>(*skywalkIvars)), 64);
            probe->setProperty("Ath10kRSNConfBefore", static_cast<UInt64>(rsnBefore), 32);
            probe->setProperty("Ath10kAppleRSNGate",
                               gateAfter ? (gateBefore ? "already-on" : "set-by-driver") : "refused");
        }
        IOLog("Ath10kMac: apple-rsn gate before=%d after=%d conf=0x%x\n",
              (int)gateBefore, (int)gateAfter, (unsigned)rsnBefore);
    }
    if (*ivarsSlot != nullptr)
        *reinterpret_cast<UInt32 *>(reinterpret_cast<char *>(*ivarsSlot) + 0x50) = 1;
    // The userspace filter that decides whether airportd keeps an interface reads
    // the IORegistry property "IO80211InterfaceRole" and requires the string
    // "Infrastructure" (recovered from Apple80211's constant CFStrings). Writing
    // the role ivar alone does not publish it, because that is what
    // IO80211SkywalkInterface::setInterfaceRole(1) does behind the ivar store.
    vif->setProperty("IO80211InterfaceRole", "Infrastructure");
    // The name must NOT be published here: airportd matches services by
    // IOInterfaceName, and a name on the vif makes it open the family's user
    // client straight on Apple's virtual interface, whose newUserClient rejects
    // roles other than 6/7 ("invalid interfaceRole: 1"). The name is stamped on
    // the v1 interface node in Ath10kVenturaInterface::attachToDataLinkLayer(),
    // which is where the BSD name first exists.
    const bool attached = ath10kAttachSkywalkInterface(this, vif, this);
    // attachInterface stored the infra pointer while the role read 1. Put the role
    // back to what createVirtualInterface() made, because
    // IO80211VirtualInterface::newUserClient() accepts 6 and 7 only and logs
    // "invalid interfaceRole: 1" for anything else - measured in v112, and it is
    // why airportd's IOServiceOpen on this node failed with 0xe00002c2.
    if (*ivarsSlot != nullptr)
        *reinterpret_cast<UInt32 *>(reinterpret_cast<char *>(*ivarsSlot) + 0x50) = 7;
    if (probe) {
        probe->setProperty("Ath10kInfraAttach", attached ? "ok" : "failed");
        probe->setProperty("Ath10kInfraPhase", attached ? "attached" : "attach-failed");
        probe->setProperty("Ath10kInfraIf",
                           static_cast<UInt64>(reinterpret_cast<uintptr_t>(vif)), 64);
    }
    IOLog("Ath10kMac: infra attach vif %p -> %d\n", vif, (int)attached);
    // Only after the attach: the manager's init calls back into this driver for
    // CARD_CAPABILITIES through the Skywalk request slot, and getScanManager is
    // only ever asked for through the vif that attachInterface() parks in the
    // controller.
    setupVenturaScanManager();
    return vif;
}

// airportd only collects scan results after the driver says the scan finished, and
// the live v1 interface has no postMessage of its own (that symbol exists only in
// the legacy bundle). The controller's gated postMessage is the remaining route;
// it ends in the interface's event pipe, so the pipe pointer is checked first and
// the outcome is published rather than assumed.
extern "C" void ath10kControllerPostMessage(IO80211Controller *self, UInt type, void *data,
                                            UInt len, void *extra)
    __asm("__ZN17IO80211Controller11postMessageEjPvmjS0_");

// Apple's own bridge from a driver message to the kern_event stream: it builds
// kev_msg{vendor_code=1, kev_class=6, subclass=1, event_code=msg, dv[0]={16,ifname},
// dv[1]={len,data}} and queues it through IO80211QueueCall::kev_post_msg (live
// 0x1ff029f -> 0x2048596). That is what Apple80211EventMonitoringInit2's reader
// thread picks up with read(fd, buf, 0x1018) after its ioctl(fd, 0x800c6502, ...),
// i.e. the channel airportd actually waits on. The Skywalk vif's postMessage only
// fans out to IOUC pipes, so a driver that stops there can post every message with
// status 0 and still leave the client timing out with -3905.
extern "C" int ath10kInterfacePostMessage(const char *ifname, UInt32 msg, void *data,
                                          size_t len, IO80211Controller *controller)
    __asm("__Z27IO80211InterfacePostMessagePKcjPvmP17IO80211Controller");

// The event a client actually waits for is posted through the IOUC pipe list
// ([ivars+0x208]), and that path looks the pipe up by the interface's BSD name:
//   postMessageInternal -> isIOUCPipeOpened -> getBSDName() -> postMessageIOUC()
// getBSDName() derives the name from a real BSD interface (vtable slot 287) and
// only then caches it at ivars+1, so a virtual interface that never got an ifnet
// reports null - and postMessageIOUC then has nothing to match. Both calls are
// exported, so the outcome is measured instead of assumed.
extern "C" const char *ath10kVifGetBSDName(IO80211VirtualInterface *self)
    __asm("__ZN23IO80211SkywalkInterface10getBSDNameEv");
extern "C" IOReturn ath10kVifPostMessageIOUC(IO80211VirtualInterface *self, const char *name,
                                             UInt32 message, void *data, size_t len)
    __asm("__ZN23IO80211SkywalkInterface15postMessageIOUCEPKcjPvm");
extern "C" IOReturn ath10kVifPostMessageInternal(IO80211VirtualInterface *self, UInt32 message,
                                                 void *data, size_t len)
    __asm("__ZN23IO80211SkywalkInterface19postMessageInternalEjPvm");

void Ath10kPCIDevice::scanNotifyCall(thread_call_param_t param0, thread_call_param_t)
{
    auto *self = static_cast<Ath10kPCIDevice *>(param0);
    if (self != nullptr)
        self->runScanNotify();
}

// [ivars+0x208] is an OSArray of per-client event pipes; the same vtable slot
// createEventPipe reads to enforce its 0x18-pipe ceiling is its element count.
static UInt32 venturaEventPipeCount(void *array)
{
    if (array == nullptr)
        return 0;
    void **vtable = *reinterpret_cast<void ***>(array);
    using CountFn = UInt32 (*)(void *);
    return reinterpret_cast<CountFn>(vtable[0x130 / 8])(array);
}

void Ath10kPCIDevice::eventSweepCall(thread_call_param_t param0, thread_call_param_t)
{
    auto *self = static_cast<Ath10kPCIDevice *>(param0);
    if (self != nullptr)
        self->runEventSweep();
}

// Posts one message id per 200 ms until the client reacts. "Reacted" is defined
// from the driver side only: the client asks for SCAN_RESULT (type 11), which
// every request channel records in _venturaLastRequest.
void Ath10kPCIDevice::runEventSweep()
{
    IOService *probe = getProvider();
    if (probe == nullptr || _venturaVif == nullptr)
        return;
    if (_venturaLastRequest == APPLE80211_IOC_SCAN_RESULT && _eventSweepId != 0) {
        probe->setProperty("Ath10kEventSweepHit",
                           static_cast<UInt64>(_eventSweepId - 1), 32);
        probe->setProperty("Ath10kEventSweep", "hit");
        return;
    }
    if (_eventSweepId == 0 || _eventSweepId > APPLE80211_M_MAX) {
        probe->setProperty("Ath10kEventSweep", "exhausted");
        return;
    }
    const UInt32 message = _eventSweepId;
    const char *name = ath10kVifGetBSDName(_venturaVif);
    ath10kVifPostMessageIOUC(_venturaVif, name, message, nullptr, 0);
    probe->setProperty("Ath10kEventSweepId", static_cast<UInt64>(message), 32);
    ++_eventSweepId;
    if (_eventSweepId <= APPLE80211_M_MAX && _eventSweepCall != nullptr) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(200, kMillisecondScale, &deadline);
        thread_call_enter_delayed(_eventSweepCall, deadline);
    }
}

void Ath10kPCIDevice::runScanNotify()
{
    IOService *probe = getProvider();
    if (probe == nullptr)
        return;
    // Only the scan that was armed when this was scheduled matters.
    if (_wmiScan.generation != _scanNotifyGeneration)
        return;
    if (!_wmiScan.completed) {
        if (++_scanNotifyAttempts <= 20 && _scanNotifyCall != nullptr) {
            uint64_t deadline = 0;
            clock_interval_to_deadline(1, kSecondScale, &deadline);
            thread_call_enter_delayed(_scanNotifyCall, deadline);
        }
        return;
    }
    probe->setProperty("Ath10kScanCompletedMs", static_cast<UInt64>(_wmiScan.elapsedMs), 64);
    // The family's own completion path: IO80211ScanManager::scanDone(true) pulls
    // SCAN_RESULT from this driver until it answers non-zero, caches every entry
    // against the pending request node (airportd's pid) and then posts the event the
    // client waits for. A real driver reaches it through IO80211InfraInterface's
    // postMessage(msg 10); this interface is Apple's vif, whose postMessage only fans
    // out to client pipes, so the manager is called directly - once per scan.
    if (_venturaScanManager != nullptr && _venturaScanMgrGeneration != _wmiScan.generation) {
        _venturaScanMgrGeneration = _wmiScan.generation;
        ath10kScanManagerScanDone(_venturaScanManager, true);
        ++_venturaScanMgrDones;
        probe->setProperty("Ath10kScanMgrDone", "ok");
        probe->setProperty("Ath10kScanMgrDones",
                           static_cast<UInt64>(_venturaScanMgrDones), 32);
        // The manager consumed the driver's result cursor while it cached the sweep
        // (scanDone pulls SCAN_RESULT until the driver reports end), and the client's
        // own SCAN_RESULT is served from that same cursor, so rewind it here or the
        // client's first read finds nothing left.
        _venturaScanIndex = 0;
        for (UInt32 i = 0; i < kVenReaderSlots; ++i)
            _venturaReaders[i].index = 0;
        // One sweep per boot: it walks the message ids the client might be waiting
        // for (see runEventSweep) and stops at the first one that produces a
        // SCAN_RESULT request.
        if (_eventSweepId == 0) {
            if (_eventSweepCall == nullptr) {
                _eventSweepCall = thread_call_allocate_with_options(
                    eventSweepCall, this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
            }
            if (_eventSweepCall != nullptr) {
                _eventSweepId = 1;
                probe->setProperty("Ath10kEventSweep", "armed");
                thread_call_enter(_eventSweepCall);
            }
        }
    }
    if (_venturaEventPipe == nullptr) {
        // No event pipe: Skywalk registration is what installs it, so airportd is
        // left polling nothing and abandons the scan with -3905.
        probe->setProperty("Ath10kScanEvent", "no-event-pipe");
        return;
    }
    ath10kControllerPostMessage(this, APPLE80211_M_SCAN_DONE, nullptr, 0, nullptr);
    // The controller's postMessage fans out from the primary interface; posting on the
    // Skywalk interface itself is the direct route into the event pipe that airportd's
    // IO80211APIUserClient subscribed to. A scan-done carries the completion status and
    // the number of results collected so the client's bookkeeping matches.
    if (_venturaVif != nullptr) {
        struct { UInt32 status; UInt32 results; } payload{0, static_cast<UInt32>(_wmiBSS.count)};
        // Three delivery shapes, because which one the Skywalk-era client listens on is
        // not documented: the interface broadcast, the controller's fan-out with the
        // "deliver" flag set, and the interface's named-IOUC channel (reached through a
        // raw vtable call because IO80211VirtualInterface does not declare it).
        _venturaVif->postMessage(APPLE80211_M_SCAN_DONE, &payload, sizeof(payload));
        _venturaVif->postMessage(APPLE80211_M_SCAN_CACHE_UPDATED, &payload, sizeof(payload));
        ath10kControllerPostMessage(this, APPLE80211_M_SCAN_DONE, &payload, sizeof(payload),
                                    reinterpret_cast<void *>(1));

        // Measured delivery into the client pipes. The name is what the pipe lookup
        // keys on; a vif has no ifnet, so getBSDName() comes back empty until the
        // unit name is stamped at ivars+1 (the same slot getBSDName() itself fills).
        const char *bsd = ath10kVifGetBSDName(_venturaVif);
        char *vifIvars = *reinterpret_cast<char **>(reinterpret_cast<char *>(_venturaVif) + 0x110);
        probe->setProperty("Ath10kVifBsdName", bsd != nullptr ? bsd : "(null)");
        if (bsd == nullptr && vifIvars != nullptr) {
            const char *unit = nullptr;
            if (_venturaInterface != nullptr) {
                if (OSString *n = OSDynamicCast(OSString,
                                                _venturaInterface->getProperty("IOInterfaceName")))
                    unit = n->getCStringNoCopy();
            }
            if (unit != nullptr) {
                strlcpy(vifIvars + 1, unit, 16);
                probe->setProperty("Ath10kVifBsdNameStamp", unit);
                bsd = ath10kVifGetBSDName(_venturaVif);
                probe->setProperty("Ath10kVifBsdNameAfter", bsd != nullptr ? bsd : "(null)");
            }
        }
        const IOReturn doneStatus = ath10kVifPostMessageIOUC(
            _venturaVif, bsd, APPLE80211_M_SCAN_DONE, nullptr, 0);
        const IOReturn cacheStatus = ath10kVifPostMessageIOUC(
            _venturaVif, bsd, APPLE80211_M_SCAN_CACHE_UPDATED, nullptr, 0);
        const IOReturn internalStatus = ath10kVifPostMessageInternal(
            _venturaVif, APPLE80211_M_SCAN_CACHE_UPDATED, nullptr, 0);
        char statuses[96];
        snprintf(statuses, sizeof(statuses), "done=%x cache=%x internal=%x",
                 static_cast<unsigned>(doneStatus), static_cast<unsigned>(cacheStatus),
                 static_cast<unsigned>(internalStatus));
        probe->setProperty("Ath10kPostStatus", statuses);

        // The kern_event route airportd's reader actually consumes.
        char ifname[16] = {};
        const char *unit = nullptr;
        if (_venturaInterface != nullptr) {
            if (OSString *n = OSDynamicCast(OSString,
                                            _venturaInterface->getProperty("IOInterfaceName")))
                unit = n->getCStringNoCopy();
        }
        strlcpy(ifname, unit != nullptr ? unit : "en1", sizeof(ifname));
        struct { UInt32 status; UInt32 results; } kevPayload{0, static_cast<UInt32>(_wmiBSS.count)};
        const int kevDone = ath10kInterfacePostMessage(
            ifname, APPLE80211_M_SCAN_DONE, &kevPayload, sizeof(kevPayload), this);
        const int kevCache = ath10kInterfacePostMessage(
            ifname, APPLE80211_M_SCAN_CACHE_UPDATED, &kevPayload, sizeof(kevPayload), this);
        char kevStatus[64];
        snprintf(kevStatus, sizeof(kevStatus), "done=%d cache=%d", kevDone, kevCache);
        probe->setProperty("Ath10kKevStatus", kevStatus);
    }
    _venturaScanNotifySent = true;
    probe->setProperty("Ath10kScanEvent", "posted-scan-done");
    // How many clients actually hold an event pipe: with zero, posting is a no-op and
    // airportd keeps waiting no matter what type is sent.
    probe->setProperty("Ath10kEventPipes",
                       static_cast<UInt64>(venturaEventPipeCount(_venturaEventPipe)), 32);
}

// The filter keeps an interface only when IORegistryEntryCreateCFProperty(entry,
// "IO80211InterfaceRole") is a CFString equal to "Infrastructure". Which entry it
// is passed is still unknown (Apple80211 asks IO80211 for a port and that helper
// walks its own registry), so stamp every node in the controller's subtree.
UInt32 Ath10kPCIDevice::stampInterfaceRole(IOService *root, int depth)
{
    if (root == nullptr || depth > 3)
        return 0;
    UInt32 n = 0;
    root->setProperty("IO80211InterfaceRole", "Infrastructure");
    ++n;
    if (OSIterator *it = root->getChildIterator(gIOServicePlane)) {
        OSObject *obj = nullptr;
        while ((obj = it->getNextObject()) != nullptr) {
            if (IOService *child = OSDynamicCast(IOService, obj))
                n += stampInterfaceRole(child, depth + 1);
        }
        it->release();
    }
    return n;
}

#endif
