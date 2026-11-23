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
#include <IOKit/IOCommandGate.h>
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
    params.type = kAth10kIfTypeIeee80211;    // while if_data.ifi_type is 802.11
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
    if (_venturaInterface == nullptr)
        return;
    _venturaInterface->setLinkState(state == APPLE80211_S_RUN
                                        ? kIO80211NetworkLinkUp
                                        : kIO80211NetworkLinkDown,
                                    0);
}

// Every apple80211 request from airportd funnels through here so the boot
// breadcrumbs can answer "did the request arrive, and what did we answer".
// airportd surfaces only "Off (forced)" for both a missing request and a
// refused one.
SInt32 Ath10kPCIDevice::venturaRecordRequest(UInt requestType, int requestNumber, void *data)
{
    ++_venturaRequestCount;
    _venturaLastRequest = static_cast<UInt32>(requestNumber);
    _venturaLastRequestIsSet = requestType == 1;
    const SInt32 status = venturaDispatchRequest(requestType, requestNumber, nullptr, data);
    _venturaLastRequestStatus = status;
    if (IOService *provider = getProvider()) {
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

SInt32 Ath10kPCIDevice::apple80211SkywalkRequest(UInt requestType, int requestNumber,
                                                 IO80211SkywalkInterface *, void *data)
{
    return venturaRecordRequest(requestType, requestNumber, data);
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
        state->state = _venturaState;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_SSID: {
        if (isSet)
            return kIOReturnUnsupported;
        apple80211_ssid_data *ssid = static_cast<apple80211_ssid_data *>(data);
        ssid->version = APPLE80211_VERSION;
        ssid->ssid_len = 0;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_BSSID: {
        if (isSet)
            return kIOReturnUnsupported;
        apple80211_bssid_data *bssid = static_cast<apple80211_bssid_data *>(data);
        bssid->version = APPLE80211_VERSION;
        memset(&bssid->bssid, 0, sizeof(bssid->bssid));
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_CHANNEL:
        return venturaChannel(data, isSet);
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
        rssi->rssi[0] = 0;
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
    case APPLE80211_IOC_SCAN_RESULT:
        return venturaScanResult(static_cast<apple80211_scan_result *>(data));
    case APPLE80211_IOC_SCANCACHE_CLEAR:
        _venturaScanIndex = 0;
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
    case APPLE80211_IOC_AUTH_TYPE:
    case APPLE80211_IOC_PHY_MODE:
    case APPLE80211_IOC_OP_MODE:
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
        return kIOReturnUnsupported;
    }
}

// ---------------------------------------------------------------------------
// Staged helpers (radio wiring lands in the following stage)
// ---------------------------------------------------------------------------

IOReturn Ath10kPCIDevice::venturaPower(bool on)
{
    _venturaPowerOn = on;
    venturaPublishState(APPLE80211_S_INIT);
    if (IOService *provider = getProvider())
        provider->setProperty("Ath10kPowerPhase", on ? "power-on-request" : "power-off-request");
    return kIOReturnSuccess;
}

IOReturn Ath10kPCIDevice::venturaScan(const apple80211_scan_data *)
{
    _venturaScanIndex = 0;
    return kIOReturnSuccess;
}

IOReturn Ath10kPCIDevice::venturaScanResult(apple80211_scan_result *result)
{
    if (result == nullptr)
        return kIOReturnBadArgument;
    memset(result, 0, sizeof(*result));
    result->version = APPLE80211_VERSION;
    return kIOReturnNotFound;
}

IOReturn Ath10kPCIDevice::venturaAssociate(const apple80211_assoc_data *)
{
    return kIOReturnUnsupported;
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
    channels->num_channels = 0;
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

#endif
