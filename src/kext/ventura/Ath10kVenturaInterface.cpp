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
    _owner = owner;
    if (probe) probe->setProperty("Ath10kIfacePhase", "inited");
    return true;
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
