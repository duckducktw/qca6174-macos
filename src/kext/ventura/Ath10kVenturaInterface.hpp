// macOS 13 (Ventura) native Wi-Fi interface.
//
// Ventura drives a third-party Wi-Fi driver through IO80211Interface, the
// classic (pre-Skywalk) network interface published by IO80211Controller.
// Receiving is a matter of handing Ethernet frames to this object; the EAPOL
// frames must go through IO80211Interface so airportd sees the 4-way
// handshake, exactly like the sealed Apple drivers do.
#pragma once

#ifdef ATH10K_VENTURA

#include "Ath10kVenturaABI.hpp"

class Ath10kPCIDevice;

class Ath10kVenturaInterface : public IO80211Interface {
    OSDeclareDefaultStructors(Ath10kVenturaInterface)

public:
    bool init(IO80211Controller *controller, Ath10kPCIDevice *owner);
    UInt32 inputPacket(mbuf_t packet, UInt32 length = 0,
                       IOOptionBits options = 0, void *param = nullptr) override;
    IOReturn attachToDataLinkLayer(IOOptionBits options, void *param) override;
    bool initIfnetParams(struct ifnet_init_params *params) override;

private:
    Ath10kPCIDevice *_owner = nullptr;
};

#endif
