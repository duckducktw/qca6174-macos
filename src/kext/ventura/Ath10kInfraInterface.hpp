// Ventura (macOS 13) Skywalk-side interface object.
//
// Measured on 13.7.8: airportd's per-interface step drops the driver's unit and
// reports "-3903 Wireless is not available" even though the medium advertises
// IFM_IEEE80211 and SystemConfiguration calls the unit Wi-Fi. Both kernel gates
// on that path cast to IO80211SkywalkInterface:
//
//   IO80211APIUserClient::commonStart(provider)          safeMetaCast(provider, IO80211SkywalkInterface)
//   IO80211Controller::attachInterface(OSObject*, IOService*)
//                                                       safeMetaCast(iface, IO80211SkywalkInterface)
//                                                       + role dword at [[iface+0x110]+0x50] == 1
//
// A v1 IO80211Interface derives from IOEthernetInterface in both Ventura
// bundles, so it can never satisfy those casts. This class derives from the
// live IO80211InfraInterface (IO80211InfraInterface : IO80211SkywalkInterface)
// and is attached to the controller the same way AirportItlwm's Sonoma build
// does: new -> init(controller) -> setInterfaceRole(1) -> setInterfaceId(1) ->
// attach(controller) -> controller->attachInterface(this, controller).
#pragma once

#ifdef ATH10K_VENTURA

#include "Ath10kVenturaABI.hpp"

// Live IO80211Family only; these are the same symbols the sealed drivers use.
#include "IO80211InfraInterface.h"

class Ath10kPCIDevice;

class Ath10kInfraInterface : public IO80211InfraInterface {
    OSDeclareDefaultStructors(Ath10kInfraInterface)

public:
    // Creates and attaches the Skywalk-side interface to the controller. Called
    // once, after Apple's IO80211Controller::start() has published the v1
    // interface and stored nothing in its infra ivar yet.
    static Ath10kInfraInterface *createAndAttach(IO80211Controller *controller,
                                                Ath10kPCIDevice *owner);

private:
    Ath10kPCIDevice *_owner = nullptr;
    void noteUserClientAttempt(UInt32 type);
};

#endif
