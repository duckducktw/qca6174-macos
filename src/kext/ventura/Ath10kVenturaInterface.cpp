// Ventura (macOS 13) IO80211 v1 glue for the QCA6174 radio core.
//
// Scope of this file: everything the sealed IO80211 stack needs in order to
// expose the adapter as a real Wi-Fi interface (power, state, channel, rates,
// capability and scan plumbing). The ath10k bring-up itself stays in the shared
// core; this file never touches registers.
#include "Ath10kVenturaABI.hpp"

#ifdef ATH10K_VENTURA

#include "Ath10kVenturaInterface.hpp"
#include "../Ath10kPCIDevice.hpp"
#include "Ath10kVenturaUserClient.hpp"

#include <IOKit/IOCommandGate.h>
#include <libkern/copyio.h>
#include <sys/errno.h>

// Apple's own IOEthernetInterface::init(IONetworkController*), addressed by its
// mangled name. A normal qualified C++ call compiles to a virtual dispatch and
// therefore lands back in IO80211Interface::init(), which is the implementation
// this branch has to avoid (it needs controller state that only
// IO80211Controller::start() creates).
extern "C" bool ath10kEthernetInterfaceInit(IOEthernetInterface *self,
                                            IONetworkController *controller)
    __asm("__ZN19IOEthernetInterface4initEP19IONetworkController");

// Diagnostic ladder: the same two implementations one level down, also by
// mangled name, so each stage can report its own outcome.
extern "C" bool ath10kNetworkInterfaceInit(IONetworkInterface *self,
                                           IONetworkController *controller)
    __asm("__ZN18IONetworkInterface4initEP19IONetworkController");

// The real 80211 interface init (exported by the live IO80211Family). Airportd
// talks to a device through IO80211APIUserClient, which IO80211Interface::init
// wires up; bypassing it leaves the interface without that plumbing even though
// an Ethernet-shaped ifnet exists.
extern "C" bool ath10kIO80211InterfaceInit(IO80211Interface *self,
                                           IONetworkController *controller)
    __asm("__ZN16IO80211Interface4initEP19IONetworkController");

#define super IO80211Interface
OSDefineMetaClassAndStructors(Ath10kVenturaInterface, IO80211Interface)

bool Ath10kVenturaInterface::init(IO80211Controller *controller, Ath10kPCIDevice *owner)
{
    if (controller == nullptr || owner == nullptr)
        return false;
    IOService *probe = owner->getProvider();
    if (probe) probe->setProperty("Ath10kIfacePhase",
                                  OSDynamicCast(IONetworkController, controller)
                                      ? "ethernet-init" : "controller-not-network");
    // Same question as the controller side (Ath10kPCIDevice::start): which copy of
    // IO80211Interface is this subclass chained to? Ventura has two, they export
    // the same names, and the live one carries no init(IOService*) at all - so a
    // mixed pair (live controller + legacy interface) would explain why the family
    // never accepts the interface. Delta against IOPCIDevice is slide-invariant:
    //   live   : IO80211Interface - IOPCIDevice = -0xE01FD0
    //   legacy : IO80211Interface - IOPCIDevice = -0xC9E318
    if (probe) {
        const OSMetaClass *ifaceMeta = getMetaClass() ? getMetaClass()->getSuperClass() : nullptr;
        const UInt64 ifaceAddr = reinterpret_cast<UInt64>(ifaceMeta);
        const UInt64 pciAddr = reinterpret_cast<UInt64>(probe->getMetaClass());
        probe->setProperty("Ath10kIfaceSuperMeta", ifaceAddr, 64);
        probe->setProperty("Ath10kIfaceMetaDelta",
                           static_cast<UInt64>(static_cast<SInt64>(ifaceAddr) -
                                               static_cast<SInt64>(pciAddr)), 64);
        IOLog("Ath10kMac: ifaceMeta=%llx pciMeta=%llx\n",
              static_cast<unsigned long long>(ifaceAddr),
              static_cast<unsigned long long>(pciAddr));
    }
    // IO80211Interface::init() reads controller state (fault reporter, log
    // stream, monitor) that only IO80211Controller::start() creates, and this
    // branch deliberately bypasses that start() (it panics without an attached
    // fault reporter). The interface Apple actually publishes is allocated and
    // named one level down, in IONetworkController/IOEthernetInterface, so call
    // that implementation directly and skip the IO80211-only preconditions.
    // Try the real 80211 interface init first: airportd reaches a device through
    // IO80211APIUserClient, and that plumbing is created there. It used to be
    // skipped because it reads controller state that only a completed
    // IO80211Controller::start() provides - Apple's start() now runs to
    // completion (Ath10kAppleStart=ok), so the preconditions exist.
    const bool familyInitOk = ath10kIO80211InterfaceInit(this, controller);
    if (probe) probe->setProperty("Ath10kIfInit80211", familyInitOk ? "ok" : "failed");
    if (familyInitOk) {
        setInterfaceType(0x6 /* IFT_ETHER: what SystemConfiguration accepts */);
        setProperty("IOPrimaryInterface", kOSBooleanTrue);
        _owner = owner;
        // airportd keeps an interface only while this role reads
        // "Infrastructure": Apple80211's per-interface filter looks the property
        // up and compares it against that constant string (recovered from the
        // framework's CFString constants). Set it on the interface object itself,
        // because the node airportd enumerates is the one created here, not the
        // vif the controller keeps.
        setProperty("IO80211InterfaceRole", "Infrastructure");
        if (probe) probe->setProperty("Ath10kIfacePhase", "inited");
        return true;
    }
    bool netOk = ath10kNetworkInterfaceInit(this, controller);
    if (probe) probe->setProperty("Ath10kNetworkInitPhase",
                                  netOk ? "network-init-ok" : "network-init-failed");
    if (!netOk) {
        if (probe) probe->setProperty("Ath10kIfacePhase", "network-init-failed");
        return false;
    }
    if (!ath10kEthernetInterfaceInit(this, controller)) {
        if (probe) probe->setProperty("Ath10kIfacePhase", "ethernet-init-failed");
        return false;
    }
    // IOEthernetInterface::initIfnetParams() refuses anything but IFT_ETHER
    // (it compares the type to 6 and bails), so the unit only appears while the
    // interface still looks like Ethernet. The 802.11 stamp therefore happens
    // after the data link layer is attached - see start() - where it updates the
    // IOInterfaceType property that SystemConfiguration classifies from.
    const bool typeOk = setInterfaceType(0x6 /* IFT_ETHER: required by initIfnetParams */);
    if (probe) probe->setProperty("Ath10kInterfaceType",
                                  typeOk ? "ethernet-pre-attach" : "set-failed");
    // setInterfaceType() also rewrites the IOInterfaceType property, and
    // SystemConfiguration reads that property when it first discovers the
    // interface. Discovery must therefore happen after the 802.11 stamp (see the
    // registerService() order below); stamping it here would make
    // initIfnetParams' own type check read 71 and refuse to build the ifnet.
    // The unit reaches IONetworkStack on its own (an IONetworkStack child node
    // appears under the interface), yet SystemConfiguration still builds no
    // hardware port for it: the one property that differs from Apple's en0 is
    // IOPrimaryInterface, which comes out as No here. Mark the interface
    // primary before the service is registered and see whether SC takes it.
    setProperty("IOPrimaryInterface", kOSBooleanTrue);
    // airportd reaches a device through a user client on the interface node, so
    // the node has to advertise one. Apple's interfaces carry
    // IOUserClientClass = "IO80211APIUserClient" (published by the interface
    // start path this driver bypasses). Without it Apple80211 enumerates the unit
    // successfully - "_getIfListCopy: exit 0, ifCount[1]" - and then drops it in
    // the public wrapper, which reports "-3903/ifCount[0]", leaving airportd with
    // "Wireless is not available".
    setProperty("IOUserClientClass", "IO80211APIUserClient");
    if (probe) probe->setProperty("Ath10kUserClientClass", "IO80211APIUserClient");
    _owner = owner;
    // Same role stamp on the fallback path (see above).
    setProperty("IO80211InterfaceRole", "Infrastructure");
    if (probe) probe->setProperty("Ath10kIfacePhase", "inited");
    return true;
}

// Leave the type at IFT_ETHER all the way through. SystemConfiguration does not
// derive the port class from the ifnet type: SCNetworkInterface.c switches on
// the IOInterfaceType *property* and rejects anything that is not IFT_ETHER or
// IFT_IEEE1394 ("unknown interface type = %d" -> interface dropped from
// SCNetworkInterfaceCopyAll). Stamping 71 there therefore made the unit
// invisible to SC, not "Wi-Fi". The Wi-Fi classification comes from the
// controller instead: with IFT_ETHER, SC checks whether the controller conforms
// to IO80211Controller and builds an IEEE80211 interface from that - the same
// shape AirportItlwm ships, which never touches the type either.
bool Ath10kVenturaInterface::initIfnetParams(struct ifnet_init_params *params)
{
    if (params == nullptr)
        return false;
    // Apple's implementation refuses anything but IFT_ETHER (it compares the
    // interface type to 6 and bails), so that check has to run while the type
    // still reads 6. What lands in the attached ifnet is a separate question:
    // Apple80211's Apple80211GetIfListCopy() lists Wi-Fi interfaces from
    // getifaddrs, keeps only ifi_type == IFT_IEEE80211 (71) and reports
    // "Wireless is not available" / ifCount[0] otherwise - that is the gate
    // airportd was failing at. SystemConfiguration reads the IOInterfaceType
    // *property* instead, which stays 6 (see attachToDataLinkLayer).
    if (!IO80211Interface::initIfnetParams(params))
        return false;
    params->type = 0x47 /* IFT_IEEE80211 */;
    if (IOService *owner = getProvider())
        owner->setProperty("Ath10kIfnetType", static_cast<UInt64>(params->type), 32);
    return true;
}

// Keep the property at IFT_ETHER. See initIfnetParams(): the 802.11 value is a
// controller-level classification in SystemConfiguration, and 71 on this
// property makes SC drop the interface entirely.
//
// This is also the right place to fill in the controller's interface ivar:
// airportd reaches the device through IO80211Controller::getNetworkInterface()
// / getInfraInterface(), which only read [controller+0x200], and the aggregate
// attachInterface() that normally stores it returns early when the ivar is
// already set - so it has to be written *inside* the attach, after that check.
IOReturn Ath10kVenturaInterface::attachToDataLinkLayer(IOOptionBits options, void *param)
{
    if (IOService *owner = getProvider())
        owner->setProperty("Ath10kDataLinkEntered", true);
    const IOReturn result = IO80211Interface::attachToDataLinkLayer(options, param);
    if (IOService *owner = getProvider())
        owner->setProperty("Ath10kDataLinkStatus",
                           static_cast<UInt64>(static_cast<UInt32>(result)), 32);
    if (result == kIOReturnSuccess) {
        setProperty("IOInterfaceType", static_cast<UInt64>(0x6), 32);
        // Apple80211 finds the service for an interface with
        // IOServiceGetMatchingService(kIOMainPortDefault, {IOPropertyMatch =
        // {IOInterfaceName = "<bsd name>"}}) and then reads IO80211InterfaceRole
        // off it, requiring the CFString "Infrastructure" (both strings and the
        // IOServiceGetMatchingService call were recovered from the framework's
        // code). Without IOInterfaceName the match fails, the interface is
        // dropped by the wrapper and airportd reports -3903.
        {
            // Build "<name><unit>" exactly like the ifnet registration does.
            char unitName[32] = {0};
            ifnet_t ifp = getIfnet();
            if (ifp != nullptr) {
                snprintf(unitName, sizeof(unitName), "%s%u", ifnet_name(ifp),
                         static_cast<unsigned>(ifnet_unit(ifp)));
                // airportd matches an interface by name —
                // IOServiceGetMatchingService({IOPropertyMatch =
                // {IOInterfaceName = "<bsd name>"}}) — and opens a user client on
                // the node that answered. The name therefore belongs *here*: this
                // is the only node whose newUserClient this driver controls, and
                // that override is what hands the open to the family's own
                // IO80211APIUserClient (see newUserClient(), which passes the
                // Skywalk vif as the provider). Publishing the name on the vif
                // instead made airportd open the client straight on Apple's
                // virtual interface, whose newUserClient rejects anything but
                // role 6/7 — measured in the kernel log as
                //   newUserClient: ERROR: invalid interfaceRole: 1 ...
                // and on the wire as IOServiceOpen 0xe00002c2.
                setProperty("IOInterfaceName", unitName);
                if (IOService *owner = getProvider())
                    owner->setProperty("Ath10kVifStamp", "interface-node");
                // The vif keeps the role/name matchable but must not advertise the
                // name, or the name match can land on it instead.
                if (_owner != nullptr && _owner->_venturaVif != nullptr) {
                    IO80211VirtualInterface *vif = _owner->_venturaVif;
                    vif->setProperty("IO80211InterfaceRole", "Infrastructure");
                    if (OSObject *stale = vif->getProperty("IOInterfaceName")) {
                        (void)stale;
                        vif->removeProperty("IOInterfaceName");
                    }
                }
            }
            if (IOService *owner = getProvider())
                owner->setProperty("Ath10kInterfaceName", unitName);
        }
        IONetworkController *controller = getController();
        if (controller != nullptr) {
            void **slot = reinterpret_cast<void **>(reinterpret_cast<char *>(controller) + 0x200);
            if (*slot == nullptr) {
                retain();
                *slot = this;
                setProperty("Ath10kCtrlInfraSlot", "installed-by-attach");
            } else {
                setProperty("Ath10kCtrlInfraSlot", "occupied");
            }
        }
    }
    return result;
}

// airportd (via Apple80211) has to open a user client on this node; when that
// open fails the interface is dropped and the caller reports "Wireless is not
// available" (-3903) without ever sending a request. Counting the attempts
// separates "airportd never asks" from "IO80211APIUserClient refuses us" - the
// two have completely different fixes.
void Ath10kVenturaInterface::noteUserClientAttempt(UInt32 type)
{
    IOService *probe = getProvider();
    Ath10kPCIDevice *owner = _owner;
    if (owner != nullptr && owner->getProvider() != nullptr)
        probe = owner->getProvider();
    if (probe != nullptr) {
        UInt64 seen = 0;
        if (OSNumber *n = OSDynamicCast(OSNumber, probe->getProperty("Ath10kNewUserClient")))
            seen = n->unsigned64BitValue();
        probe->setProperty("Ath10kNewUserClient", seen + 1, 32);
        probe->setProperty("Ath10kNewUserClientType", static_cast<UInt64>(type), 32);
        IOLog("Ath10kMac: newUserClient type=%u (#%llu)\n", (unsigned)type,
              static_cast<unsigned long long>(seen + 1));
    }
}

// Hand the open to the family's own client implementation with the Skywalk vif as
// the provider. Two casts decide this: IO80211APIUserClient::commonStart casts its
// provider to IO80211SkywalkInterface (a v1 IO80211Interface can never satisfy
// that), and IO80211VirtualInterface::newUserClient accepts interface roles 6 and
// 7 only. attachPlatformVif() therefore leaves the vif at the role
// createVirtualInterface() gave it.
//
// This is the same delegation v112 tried, and it panicked because the client's
// event subscription dereferences [ivars+0x208] (the event-pipe OSArray), which
// only IO80211SkywalkInterface::start() installs - and createVirtualInterface()
// only init()s. attachPlatformVif() now calls that start(), and the pipe pointer
// is published as Ath10kVifEventPipe; if it is still null the open is refused
// instead of panicking, so airportd falls back to the ifnet ioctl channel
// (performCommand) the way v113-v120 ran.
IOReturn Ath10kVenturaInterface::newUserClient(task_t owningTask, void *securityID,
                                               UInt32 type, OSDictionary *properties,
                                               IOUserClient **handler)
{
    noteUserClientAttempt(type);
    if (handler != nullptr)
        *handler = nullptr;
    Ath10kPCIDevice *owner = _owner;
    IO80211VirtualInterface *vif = (owner != nullptr) ? owner->_venturaVif : nullptr;
    if (vif == nullptr || handler == nullptr || owner->_venturaEventPipe == nullptr)
        return kIOReturnUnsupported;
    IOUserClient *client = nullptr;
    const IOReturn result = vif->newUserClient(owningTask, securityID, type, properties, &client);
    IOService *probe = (owner->getProvider() != nullptr) ? owner->getProvider() : getProvider();
    if (probe != nullptr)
        probe->setProperty("Ath10kFamilyClient",
                           result == kIOReturnSuccess ? "created" : "failed");
    if (result != kIOReturnSuccess || client == nullptr)
        return kIOReturnUnsupported;
    *handler = client;
    return kIOReturnSuccess;
}

// airportd's bind chain, recovered from its own log:
//   Apple80211BindToInterfaceWithService: ... IOServiceOpen failed with 0xe00002c2
//   Apple80211BindToInterfaceWithIOCTL: ... Falling back to IOCTL path,
//                                          ifname['en1'] err -1/0xffffffff
// The second line is airportd offering the classic ifnet ioctl channel, which
// fails today because nothing answers SIOCGA80211/SIOCSA80211 (the live
// IO80211Interface has no performCommand of its own, so the Ethernet
// implementation sees the command, does not know it, and returns ENOTTY).
// That channel is the one a v1 interface driver is supposed to serve: it needs
// no IO80211APIUserClient, no Skywalk registration and no event pipe, and it is
// how pre-Skywalk drivers are driven on 13.x. Requests arrive as
//   struct apple80211req { req_if_name[16]; req_type; req_val; req_len; req_data }
// where req_data is a caller (user-space) pointer to the payload of req_len
// bytes: gets write it back, sets read it in.
//
// IOEthernetInterface::performCommand is exported by IONetworkingFamily (EXT),
// so the commands that are not ours still reach the base implementation.
extern "C" SInt32 ath10kEthernetPerformCommand(IOEthernetInterface *self,
                                               IONetworkController *controller,
                                               unsigned long cmd, void *arg0, void *arg1)
    __asm("__ZN19IOEthernetInterface14performCommandEP19IONetworkControllermPvS2_");

// IO80211Interface.h carries both encodings of the two ioctl codes (_IOW/_IOWR
// over struct apple80211req for pre- and post-10.15 layouts); compare against
// both so the branch this build saw does not matter.
static const unsigned long kSiocsA80211New = 2150656456UL;
static const unsigned long kSiocgA80211New = 3224398281UL;
static const unsigned long kSiocsA80211Old = 2150132168UL;
static const unsigned long kSiocgA80211Old = 3223873993UL;

SInt32 Ath10kVenturaInterface::performCommand(IONetworkController *controller, unsigned long cmd,
                                              void *arg0, void *arg1)
{
    const bool isSet = (cmd == kSiocsA80211New || cmd == kSiocsA80211Old);
    const bool isGet = (cmd == kSiocgA80211New || cmd == kSiocgA80211Old);
    if (!isSet && !isGet)
        return ath10kEthernetPerformCommand(this, controller, cmd, arg0, arg1);

    IOService *probe = (_owner != nullptr) ? _owner->getProvider() : getProvider();
    if (probe != nullptr) {
        UInt64 calls = 0;
        if (OSNumber *n = OSDynamicCast(OSNumber, probe->getProperty("Ath10kIoctlCalls")))
            calls = n->unsigned64BitValue();
        probe->setProperty("Ath10kIoctlCalls", calls + 1, 32);
        probe->setProperty("Ath10kIoctlCmd", static_cast<UInt64>(cmd), 32);
        probe->setProperty("Ath10kIoctlVerb", isSet ? "set" : "get");
    }
    IOLog("Ath10kMac: performCommand cmd=0x%lx %s\n", cmd, isSet ? "set" : "get");

    if (_owner == nullptr)
        return EIO;

    // arg1 is *kernel* memory, not a user pointer: BSD's ioctl() copies the
    // 40-byte request structure in for IOC_IN/OIOWR commands (IOCPARM_LEN of the
    // command code) and copies it back out afterwards, so a copyin here fails
    // with EFAULT and nothing ever reaches the dispatcher — which is exactly
    // what 174 airportd calls did before this. The payload is different: it
    // lives behind req.req_data, a user-space pointer the driver copies itself.
    if (arg1 == nullptr)
        return EINVAL;
    const unsigned long parmLen = (cmd >> 16) & 0x1fff;
    if (parmLen < sizeof(struct apple80211req))
        return EINVAL;
    struct apple80211req req;
    memcpy(&req, arg1, sizeof(req));
    if (probe != nullptr) {
        probe->setProperty("Ath10kIoctlType", static_cast<UInt64>(static_cast<UInt32>(req.req_type)), 32);
        probe->setProperty("Ath10kIoctlReqLen", static_cast<UInt64>(static_cast<UInt32>(req.req_len)), 32);
    }
    // Getter replies have ABI-dependent shapes (asr_ie_data is an inline array
    // from 12.0 on, a pointer before that), and req_len is the only honest
    // signal for which one airportd compiled against. Record it before dispatch.
    _owner->_venturaIoctlLength = req.req_len;

    // Payload buffer. req_len is authoritative for the caller's buffer, but a
    // getter may be asked for less than the structure it fills, so allocate a
    // floor and only copy req_len bytes back out.
    UInt32 len = req.req_len;
    UInt32 allocLen = (len < 2048) ? 2048 : len;
    if (allocLen > 65536)
        allocLen = 65536;
    void *buf = IOMalloc(allocLen);
    if (buf == nullptr)
        return ENOMEM;
    memset(buf, 0, allocLen);
    if (isSet && len != 0 && req.req_data != nullptr) {
        if (copyin(reinterpret_cast<user_addr_t>(req.req_data), buf, len) != 0) {
            IOFree(buf, allocLen);
            return EFAULT;
        }
    }

    const SInt32 status = _owner->venturaRecordRequest(isSet ? 1 : 0, req.req_type, buf);
    if (probe != nullptr)
        probe->setProperty("Ath10kIoctlStatus",
                           static_cast<UInt64>(static_cast<UInt32>(status)), 32);

    if (isGet && status == kIOReturnSuccess && len != 0 && req.req_data != nullptr)
        copyout(buf, reinterpret_cast<user_addr_t>(req.req_data), len);

    IOFree(buf, allocLen);
    return (status == kIOReturnSuccess) ? 0 : EIO;
}

IOReturn Ath10kVenturaInterface::newUserClient(task_t owningTask, void *securityID,
                                               UInt32 type, IOUserClient **handler)
{
    // IONetworkInterface's older overload: same delegation, no properties dict.
    noteUserClientAttempt(type | 0x80000000u);
    if (handler != nullptr)
        *handler = nullptr;
    Ath10kPCIDevice *owner = _owner;
    IO80211VirtualInterface *vif = (owner != nullptr) ? owner->_venturaVif : nullptr;
    if (vif == nullptr || handler == nullptr || owner->_venturaEventPipe == nullptr)
        return kIOReturnUnsupported;
    IOUserClient *client = nullptr;
    const IOReturn result = vif->newUserClient(owningTask, securityID, type, nullptr, &client);
    if (result != kIOReturnSuccess || client == nullptr)
        return kIOReturnUnsupported;
    *handler = client;
    return kIOReturnSuccess;
}

UInt32 Ath10kVenturaInterface::inputPacket(mbuf_t packet, UInt32 length,
                                           IOOptionBits options, void *param)
{
    if (packet == nullptr)
        return kIOReturnBadArgument;
    size_t frameLength = length != 0 ? length : mbuf_pkthdr_len(packet);
    if (frameLength >= sizeof(ether_header_t)) {
        const ether_header_t *header =
            reinterpret_cast<const ether_header_t *>(mbuf_data(packet));
        if (header != nullptr && header->ether_type == htons(ETHERTYPE_PAE)) {
            // EAPOL must reach the IO80211 supplicant path, not the IP stack.
            return IO80211Interface::inputPacket(packet, static_cast<UInt32>(frameLength),
                                                 0, param);
        }
    }
    return IOEthernetInterface::inputPacket(packet, static_cast<UInt32>(frameLength),
                                            options, param);
}

#endif
