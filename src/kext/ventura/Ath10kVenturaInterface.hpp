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
    // airportd reaches a device through a user client on the interface node
    // (Apple80211GetIfListCopy opens it and drops the interface when the open
    // fails, which is the "-3903 Wireless is not available" path). Count the
    // attempts here: the counter says whether airportd ever asks, which is a
    // different question from whether IO80211APIUserClient accepts us.
    IOReturn newUserClient(task_t owningTask, void *securityID,
                           UInt32 type, OSDictionary *properties,
                           IOUserClient **handler) override;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                           IOUserClient **handler) override;

private:
    Ath10kPCIDevice *_owner = nullptr;
    void noteUserClientAttempt(UInt32 type);
    IOUserClient *makeUserClient(UInt32 type);
};

#endif
