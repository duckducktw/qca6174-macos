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

#include <IOKit/IOCommandGate.h>

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
    // IO80211Interface::init() reads controller state (fault reporter, log
    // stream, monitor) that only IO80211Controller::start() creates, and this
    // branch deliberately bypasses that start() (it panics without an attached
    // fault reporter). The interface Apple actually publishes is allocated and
    // named one level down, in IONetworkController/IOEthernetInterface, so call
    // that implementation directly and skip the IO80211-only preconditions.
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
    _owner = owner;
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
    return IO80211Interface::initIfnetParams(params);
}

// Keep the property at IFT_ETHER. See initIfnetParams(): the 802.11 value is a
// controller-level classification in SystemConfiguration, and 71 on this
// property makes SC drop the interface entirely.
IOReturn Ath10kVenturaInterface::attachToDataLinkLayer(IOOptionBits options, void *param)
{
    const IOReturn result = IO80211Interface::attachToDataLinkLayer(options, param);
    if (result == kIOReturnSuccess)
        setProperty("IOInterfaceType", static_cast<UInt64>(0x6), 32);
    return result;
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
