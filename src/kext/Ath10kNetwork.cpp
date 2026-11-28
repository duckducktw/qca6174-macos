#include "Ath10kPCIDevice.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/network/IOGatedOutputQueue.h>
#include <IOKit/network/IONetworkMedium.h>
#include <sys/kpi_mbuf.h>

const OSString *Ath10kPCIDevice::newModelString() const {
    return OSString::withCString("Ath10k WLAN Adapter");
}
bool Ath10kPCIDevice::configureInterface(IONetworkInterface *interface) {
#ifdef ATH10K_VENTURA
    // Publish the IFM_IEEE80211 medium around the base call: the interface's
    // ifmedia list (the SIOCGIFMEDIA answer Apple80211 filters on) is built from
    // the controller's medium dictionary, and something between start() and
    // attach leaves it as Ethernet autoselect (0x20). Publish before *and*
    // after the base implementation, and record the dictionary keys at each
    // point so the replacement is visible in ioreg instead of inferred.
    char mediumKeys[96];
    {
        const IONetworkMedium *primaryMedium = nullptr;
        const bool mediaOk = createVenturaMediumTables(&primaryMedium, true) &&
            (primaryMedium == nullptr ||
             (setCurrentMedium(primaryMedium) && setSelectedMedium(primaryMedium)));
        venturaMediumDictionaryKeys(mediumKeys, sizeof(mediumKeys));
        if (IOService *probe = getProvider()) {
            probe->setProperty("Ath10kMediumConfigPhase", mediaOk ? "ieee80211" : "failed");
            probe->setProperty("Ath10kMediumPreBase", mediumKeys);
        }
    }
#endif
    if(!IOEthernetController::configureInterface(interface))return false;
#ifdef ATH10K_VENTURA
    {
        const IONetworkMedium *primaryMedium = nullptr;
        const bool mediaOk = createVenturaMediumTables(&primaryMedium, true) &&
            (primaryMedium == nullptr ||
             (setCurrentMedium(primaryMedium) && setSelectedMedium(primaryMedium)));
        venturaMediumDictionaryKeys(mediumKeys, sizeof(mediumKeys));
        if (IOService *probe = getProvider()) {
            probe->setProperty("Ath10kMediumPostBase", mediumKeys);
            probe->setProperty("Ath10kMediumPostBaseOk", mediaOk);
        }
    }
#endif
    interface->setProperty("IOModel","Ath10k WLAN Adapter");
    interface->setProperty("IOUserVisibleName","Ath10k WLAN Adapter");
    interface->setProperty("Product Name","Ath10k WLAN Adapter");
    return true;
}
bool Ath10kPCIDevice::createWorkLoop()
{
    // IONetworkController sets a policy on this loop's thread. The Sequoia
    // IO80211WorkQueue::getThread() returns NULL and cannot fill that role.
    // NOTE: referencing IO80211WorkLoop::workLoop() here (like AirportItlwm)
    // makes OpenCore skip the whole kext on 13.7 - the binary links, but the
    // injected kext never appears in kmutil - so keep the plain work loop.
    if (!_networkWorkLoop) _networkWorkLoop=IOWorkLoop::workLoop();
    if (!_networkWorkLoop) return false;
#ifdef ATH10K_NATIVE
    if (!_nativeWorkQueue) _nativeWorkQueue=IO80211WorkQueue::workQueue();
    return _nativeWorkQueue!=nullptr;
#else
    return true;
#endif
}
IOWorkLoop *Ath10kPCIDevice::getWorkLoop() const {return _networkWorkLoop;}
IOOutputQueue *Ath10kPCIDevice::createOutputQueue()
{
    return IOGatedOutputQueue::withTarget(this,getWorkLoop(),Ath10k::kOutputQueueCapacity);
}
IOReturn Ath10kPCIDevice::getHardwareAddress(IOEthernetAddress *address)
{
    if(!address || !_wmiInit.ready){
        // IONetworkController::attachInterface() runs prepare() -> publishProperties()
        // -> getHardwareAddress() before the radio is up (super::start() is the very
        // first thing start() does, firmware comes seconds later). Returning
        // kIOReturnNotReady there makes publishProperties() fail, prepare() return
        // 0xe00002bc and attachInterface() false *before* createInterface() is even
        // called (measured: Ath10kPubAttach=failed, Ath10kCreateIfaceCalls stayed 1).
        // The board address from the WMI init block is already known in that window,
        // so answer with it instead of failing.
        if(address){
            bool anyByte=false;
            for(UInt32 i=0;i<6;++i)if(_wmiInit.mac[i])anyByte=true;
            if(anyByte){
                for(UInt32 i=0;i<6;++i)address->bytes[i]=_wmiInit.mac[i];
                if(IOService *probe=getProvider())probe->setProperty("Ath10kHwAddrSource","wmi-early");
                return kIOReturnSuccess;
            }
        }
        return kIOReturnNotReady;
    }
#ifdef ATH10K_NATIVE
    const UInt8 *mac=_nativeMACValid?_nativeMACCurrent:_wmiInit.mac;
#else
    const UInt8 *mac=_wmiInit.mac;
#endif
    for(UInt32 i=0;i<6;++i)address->bytes[i]=mac[i];
    return kIOReturnSuccess;
}
UInt32 Ath10kPCIDevice::getFeatures() const
{
    // Tags stay in the Ethernet frame. Ask macOS for software VLAN handling.
    return (IOEthernetController::getFeatures()&~kIONetworkFeatureHardwareVlan)|kIONetworkFeatureSoftwareVlan;
}
IOReturn Ath10kPCIDevice::selectMedium(const IONetworkMedium *medium)
{
    if(!medium || medium->getType()!=kIOMediumEthernetAuto)return kIOReturnUnsupported;
    return setSelectedMedium(medium)?kIOReturnSuccess:kIOReturnError;
}
IOReturn Ath10kPCIDevice::setPromiscuousMode(bool active)
{
    return active?kIOReturnUnsupported:kIOReturnSuccess;
}
IOReturn Ath10kPCIDevice::multicastAction(OSObject *owner,void *mode,void *list,void *count,void *)
{
    auto *self=OSDynamicCast(Ath10kPCIDevice,owner);if(!self)return kIOReturnBadArgument;
    if(mode){self->_connection.multicast.enabled=*static_cast<bool *>(mode);return kIOReturnSuccess;}
    return self->_connection.multicast.configure(static_cast<const UInt8 *>(list),*static_cast<UInt32 *>(count))?kIOReturnSuccess:kIOReturnBadArgument;
}
IOReturn Ath10kPCIDevice::setMulticastMode(bool enabled) {
    return _wmiWorkLoop?_wmiWorkLoop->runAction(multicastAction,this,&enabled,nullptr,nullptr):kIOReturnNotReady;
}
IOReturn Ath10kPCIDevice::setMulticastList(IOEthernetAddress *list,UInt32 count) {
    if(count>4096 || (count && !list))return kIOReturnBadArgument;
    static_assert(sizeof(IOEthernetAddress)==6,"Ethernet multicast ABI");
    return _wmiWorkLoop?_wmiWorkLoop->runAction(multicastAction,this,nullptr,list,&count):kIOReturnNotReady;
}
IOReturn Ath10kPCIDevice::enable(IONetworkInterface *)
{
    if(!_wmiLive.active)return kIOReturnNotReady;
    _networkEnabled=true;
    if(_connection.stage==Ath10k::LinkStage::Connected && (!_connection.secure || _connection.portAuthorized))networkLink(true);
    return kIOReturnSuccess;
}
IOReturn Ath10kPCIDevice::disable(IONetworkInterface *)
{
    _networkEnabled=false;networkLink(false);return kIOReturnSuccess;
}
void Ath10kPCIDevice::networkLink(bool up)
{
    up=up && _networkEnabled && _wmiLive.active && _connection.disconnect==Ath10k::DisconnectStage::None;
    if(!up){_dataTXStalled=false;flushNetworkRX();_rxReorder.reset([](Ath10k::RXBatch *b){IOFree(b,sizeof(*b));});_rxDefrag.reset();_rxBatch.reset();}
    if(_networkLinkUp==up)return;
    _networkLinkUp=up;
    if(!up && getOutputQueue()){getOutputQueue()->stop();getOutputQueue()->flush();}
    // Report the negotiated PHY rate, not just valid/active: without a speed the
    // family leaves the link at 0 Mbps and System Settings shows "Transmit Rate: 0,
    // MCS Index: 0". Approximate PHY (HT/VHT, per-stream base rate x chains, +11%
    // for a 400 ns guard interval) is enough for the UI.
    if (IOService *probe = getProvider())
        probe->setProperty("Ath10kLinkRateDbg",
                           static_cast<UInt64>((_connection.width << 16) |
                                               ((_connection.nss & 0xff) << 8) |
                                               (_connection.shortGI ? 1 : 0)), 32);
    UInt64 speed = 0;
    if (up) {
        UInt32 perStream = 65;                       // HT20, 800 ns GI
        if (_connection.vhtEnabled) perStream = _connection.width >= 80 ? 292
                                      : (_connection.width >= 40 ? 146 : 72);
        else if (_connection.width >= 40) perStream = 135;
        if (_connection.shortGI) perStream = perStream * 111 / 100;
        const UInt32 nss = _connection.nss ? _connection.nss : 1;
        speed = static_cast<UInt64>(perStream) * nss;
    }
    setLinkStatus(kIONetworkLinkValid|(up?kIONetworkLinkActive:0), nullptr, speed);
#ifndef ATH10K_NATIVE
    // Push the same state onto the published interface. setLinkStatus() above only
    // touches the controller, so `ifconfig en1` kept reporting "status: active" (and
    // kept its old IPv4) after a teardown - airportd reads that and refuses the user's
    // next join with error 37 "Already associated, and network transition not
    // permitted", which surfaces as -3903.
    venturaInterfaceLink(up);
#endif
    if(up && getOutputQueue())getOutputQueue()->start();
}
bool Ath10kPCIDevice::prepareNetwork()
{
    if(!_wmiInit.ready || !_wmiLive.active)return false;
    if(!prepareRXReserve())return false;
    // Allocate the TX window before link-up, outside the output hot path.
    for(auto &b:_dataBuffers)
        if(!b.payload.allocate(2048,128,_dmaMapper,Ath10kDMABuffer::Role::Payload) ||
           !b.command.allocate(256,128,_dmaMapper) || !b.fragments.allocate(16,16,_dmaMapper))return false;
    auto *dict=OSDictionary::withCapacity(1);
    auto *medium=IONetworkMedium::medium(kIOMediumEthernetAuto,0);
    if(!dict || !medium){if(dict)dict->release();if(medium)medium->release();return false;}
    bool ok=IONetworkMedium::addMedium(dict,medium) && publishMediumDictionary(dict);
    if(ok)setCurrentMedium(medium);
    medium->release();dict->release();if(!ok)return false;
    setProperty("IOInterfaceName","Ath10k WLAN Adapter");
    setProperty("IOModel","Ath10k WLAN Adapter");
    setProperty("IOUserVisibleName","Ath10k WLAN Adapter");
    setProperty("Product Name","Ath10k WLAN Adapter");
    setLinkStatus(kIONetworkLinkValid);
    // No implicit selection of a nearby open AP. Configuration is explicit and
    // remains disabled in the distributed personality until the user sets it.
    if(getProperty("Ath10kOpenConnect")==kOSBooleanTrue){
        auto *ssid=OSDynamicCast(OSData,getProperty("Ath10kConnectSSID"));
        auto *bssid=OSDynamicCast(OSData,getProperty("Ath10kConnectBSSID"));
        auto *power=OSDynamicCast(OSNumber,getProperty("Ath10kConnectPowerHalfDbm"));
        if(!ssid || !ssid->getLength() || ssid->getLength()>32 || !bssid || bssid->getLength()!=6 ||
           !power || !power->unsigned32BitValue() || power->unsigned32BitValue()>20){
            _connection.error="invalid-connection-config";_connection.stage=Ath10k::LinkStage::Failed;
        }else{
            _connection.ssidLength=static_cast<UInt8>(ssid->getLength());
            bcopy(ssid->getBytesNoCopy(),_connection.ssid,ssid->getLength());
            bcopy(bssid->getBytesNoCopy(),_connection.bssid,6);
            bcopy(_wmiInit.mac,_connection.local,6);
            UInt8 nz=0;for(UInt32 i=0;i<6;++i)nz|=_connection.bssid[i];
            if(!nz || (_connection.bssid[0]&1)){
                _connection.error="invalid-bssid";_connection.stage=Ath10k::LinkStage::Failed;
            }else{
                _connection.txPowerHalfDbm=static_cast<UInt8>(power->unsigned32BitValue());
                _connection.stage=Ath10k::LinkStage::Scan;
            }
        }
    }
#ifdef ATH10K_NATIVE
    return prepareNativeInterface();
#else
    return attachInterface(&_networkInterface);
#endif
}
void Ath10kPCIDevice::publishConnection()
{
    setProperty("Ath10kQoSEnabled",_connection.ap.wmmPresent);
    setProperty("Ath10kWMMParameterCount",_connection.ap.wmmCount,32);
    setProperty("Ath10kAPMaintenanceStep",_connection.maintenance.step,32);
    setProperty("Ath10kAPMaintenanceCompleted",_connection.maintenance.completed,32);
    setProperty("Ath10kAPMaintenanceTXBlocked",_connection.maintenance.blocked);
    setProperty("Ath10kTXAdmissionDropped",_connection.txAdmissionDropped,32);
    const char *tidKeys[]={"Ath10kTXTID0","Ath10kTXTID1","Ath10kTXTID2","Ath10kTXTID3","Ath10kTXTID4","Ath10kTXTID5","Ath10kTXTID6","Ath10kTXTID7"};
    for(UInt32 i=0;i<8;++i)setProperty(tidKeys[i],_connection.txTID[i],32);
    const char *acKeys[]={"Ath10kTXACBE","Ath10kTXACBK","Ath10kTXACVI","Ath10kTXACVO"};
    const char *fwKeys[]={"Ath10kFWTxACBE","Ath10kFWTxACBK","Ath10kFWTxACVI","Ath10kFWTxACVO"};
    const char *retryKeys[]={"Ath10kFWRetryACBE","Ath10kFWRetryACBK","Ath10kFWRetryACVI","Ath10kFWRetryACVO"};
    const char *failureKeys[]={"Ath10kFWFailACBE","Ath10kFWFailACBK","Ath10kFWFailACVI","Ath10kFWFailACVO"};
    for(UInt32 i=0;i<4;++i){setProperty(acKeys[i],_connection.txAC[i],32);setProperty(fwKeys[i],_connection.fwStats.tx[i],32);
        setProperty(retryKeys[i],_connection.fwStats.retries[i],32);setProperty(failureKeys[i],_connection.fwStats.failures[i],32);}
    setProperty("Ath10kFirmwareStatsSerial",_connection.fwStats.serial,32);
    setProperty("Ath10kFirmwareStatsStep",_connection.stats.step,32);
    setProperty("Ath10kFirmwareStatsCompleted",_connection.stats.completed,32);
    const UInt64 statsNow=Ath10k::uptimeNS()/1000000u;
    setProperty("Ath10kFirmwareStatsAgeMs",_connection.fwStats.observedMs && statsNow>=_connection.fwStats.observedMs?statsNow-_connection.fwStats.observedMs:0xffffffffu,64);
    setProperty("Ath10kLinkNoiseAgeMs",_connection.fwStats.noiseObservedMs && statsNow>=_connection.fwStats.noiseObservedMs?statsNow-_connection.fwStats.noiseObservedMs:0xffffffffu,64);
    setProperty("Ath10kLinkRateAgeMs",_connection.fwStats.rateObservedMs && statsNow>=_connection.fwStats.rateObservedMs?statsNow-_connection.fwStats.rateObservedMs:0xffffffffu,64);
    setProperty("Ath10kLinkNoiseValid",_connection.fwStats.noiseValid);
    setProperty("Ath10kLinkNoiseDbm",static_cast<UInt32>(_connection.fwStats.noiseDbm),32);
    setProperty("Ath10kLinkRateValid",_connection.fwStats.rateValid);
    setProperty("Ath10kLinkTXRateKbps",_connection.fwStats.txKbps,32);
    setProperty("Ath10kLinkRXRateKbps",_connection.fwStats.rxKbps,32);
    setProperty("Ath10kConnectionPowerHalfDbm",_connection.txPowerHalfDbm,32);
    setProperty("Ath10kConnectionChannelPowerLimitHalfDbm",Ath10k::stationPowerLimit(_connection),32);
    setProperty("Ath10kStationPowerSaveRequested",_connection.radio.requestedPS);
    setProperty("Ath10kStationPowerSaveApplied",_connection.radio.appliedPS);
    setProperty("Ath10kTXPowerSaveWakeRequested",_connection.radio.txWakeRequested);
    setProperty("Ath10kTXPowerSaveWakeRequests",UInt64(_connection.radio.txWakeRequests),32);
    setProperty("Ath10kDataTxWatchdogReason",_connection.dataWatchdogReason);
    setProperty("Ath10kDataTxWatchdogSlot",UInt64(_connection.dataWatchdogSlot),32);
    setProperty("Ath10kDataTxWatchdogID",UInt64(_connection.dataWatchdogID),32);
    setProperty("Ath10kDataTxWatchdogAgeMs",_connection.dataWatchdogAgeMs);
    setProperty("Ath10kStationDTIMPeriod",_connection.radio.appliedDTIM,32);
    setProperty("Ath10kAPDTIMPeriod",_connection.ap.timPresent?_connection.ap.dtimPeriod:0,32);
    setProperty("Ath10kStationTXPowerBlocked",_connection.radio.powerBlocked);
    setProperty("Ath10kStationPowerCeilingHalfDbm",_connection.radio.vdevPower,32);
    setProperty("Ath10kSpectrumRequests",UInt64(_connection.spectrum.received),32);
    setProperty("Ath10kSpectrumCompleted",UInt64(_connection.spectrum.completed),32);
    setProperty("Ath10kSpectrumDropped",UInt64(_connection.spectrum.dropped),32);
    setProperty("Ath10kManagementAttempts",UInt64(_connection.managementAttempts),32);
    setProperty("Ath10kAPLivenessProbeActive",_connection.probe.active);
    setProperty("Ath10kAPLivenessProbeAttempts",_connection.probe.attempts,32);
    setProperty("Ath10kStationRadioStep",_connection.radio.step,32);
    setProperty("Ath10kStationRadioCompleted",_connection.radio.completed,32);
    setProperty("Ath10kStationAPPowerConstraintDb",_connection.powerConstraintDb,32);
    setProperty("Ath10kStationBeaconMisses",_connection.beaconMisses,32);
    setProperty("Ath10kStationBeaconTimeoutMs",_connection.beaconTimeoutMs,32);
    setProperty("Ath10kStationMulticastDrops",_connection.multicast.dropped,32);
    setProperty("Ath10kStationDFS",_regulatory.stationDFS);
    setProperty("Ath10kConnectionChannelDefinitionValid",Ath10k::channelDefinitionAllowed(
        _connection.operatingChannel,_connection.centerChannel,_connection.width,_connection.regulatory.stationDFS));
    setProperty("Ath10kChannelPolicy","regdb-board-firmware-infrastructure-sta-20dbm");
    setProperty("Ath10kDisconnectStage",static_cast<UInt32>(_connection.disconnect),32);
    setProperty("Ath10kDisconnectCount",_connection.disconnectCount,32);
    const auto &cd=_connection.diagnostics;
    setProperty("Ath10kConnectionLastDisconnectReason",cd.lastDisconnectReason);
    setProperty("Ath10kConnectionLastCancelOrigin",cd.lastCancelOrigin);
    setProperty("Ath10kConnectionLastDisconnectStage",cd.lastDisconnectStage,32);
    setProperty("Ath10kConnectionLastDisconnectSecurity",cd.lastDisconnectSecurity,32);
    setProperty("Ath10kConnectionLastDisconnectPeers",cd.lastDisconnectPeers,32);
    setProperty("Ath10kConnectionRollbackCount",cd.rollbacks,32);
    setProperty("Ath10kConnectionLastRollbackStage",cd.lastRollbackStage,32);
    setProperty("Ath10kConnectionWMICommands",cd.commands,32);
    setProperty("Ath10kConnectionWMILastCommand",cd.lastCommand,32);
    setProperty("Ath10kConnectionWMILastStage",cd.lastCommandStage,32);
    setProperty("Ath10kConnectionWMILastSecurity",cd.lastCommandSecurity,32);
    setProperty("Ath10kConnectionWMILastDisconnect",cd.lastCommandDisconnect,32);
    setProperty("Ath10kConnectionWMILastAPStep",cd.lastCommandAP,32);
    setProperty("Ath10kConnectionWMILastRadioStep",cd.lastCommandRadio,32);
    setProperty("Ath10kConnectionPeerMaps",cd.maps,32);
    setProperty("Ath10kConnectionPeerUnmaps",cd.unmaps,32);
    setProperty("Ath10kConnectionPeerUnknownUnmaps",cd.unknownUnmaps,32);
    setProperty("Ath10kConnectionPeerEventID",cd.peer,32);
    setProperty("Ath10kConnectionPeerEventVdev",cd.vdev,32);
    setProperty("Ath10kConnectionPeerEventOwn",cd.own);
    // The SDK's OSData overload takes void * and copies the supplied bytes.
    UInt8 peerEventMAC[sizeof(cd.peerMAC)];
    bcopy(cd.peerMAC,peerEventMAC,sizeof(peerEventMAC));
    setProperty("Ath10kConnectionPeerEventMAC",peerEventMAC,sizeof(peerEventMAC));
    setProperty("Ath10kConnectionPeerEventBefore",cd.peerBefore,32);
    setProperty("Ath10kConnectionPeerEventAfter",cd.peerAfter,32);
    setProperty("Ath10kConnectionMLMECount",cd.mlmeCount,32);
    setProperty("Ath10kConnectionMLMEReason",cd.mlmeReason,32);
    setProperty("Ath10kConnectionMLMESubtype",cd.mlmeSubtype,32);
    setProperty("Ath10kConnectionMLMEChannel",cd.mlmeChannel,32);
    setProperty("Ath10kConnectionPeerCreateIssued",_connection.peerCreateIssued);
    setProperty("Ath10kConnectionVdevStartIssued",_connection.vdevStartIssued);
    setProperty("Ath10kConnectionState",_connection.disconnect!=Ath10k::DisconnectStage::None?"disconnecting":_connection.secure && _connection.stage==Ath10k::LinkStage::Connected?(_connection.portAuthorized?"connected-wpa2":"wpa2-handshake"):Ath10k::linkStageName(_connection.stage));
    setProperty("Ath10kSecurityPhase",_connection.securityPhase,32);
    setProperty("Ath10kSecurityGeneration",_connection.securityGeneration,32);
    setProperty("Ath10kSecurityRekeyKind",_connection.rekeyKind,32);
    setProperty("Ath10kSecurityEAPOLHold",_connection.eapolPause);
    setProperty("Ath10kSecurityIndications",_connection.securityInd.received,32);
    setProperty("Ath10kSecurityIndIgnored",_connection.securityInd.ignored,32);
    setProperty("Ath10kSecurityIndLateClears",_connection.securityInd.lateClears,32);
    setProperty("Ath10kSecurityIndPeer",_connection.securityInd.peer,32);
    setProperty("Ath10kSecurityIndFlags",_connection.securityInd.flags,32);
    setProperty("Ath10kSecurityIndDisconnectStage",_connection.securityInd.disconnect,32);
    setProperty("Ath10kSecurityIndMapped",_connection.securityInd.mapped);
    setProperty("Ath10kSecurityIndRetired",_connection.securityInd.retired);
    setProperty("Ath10kSecurityIndAcknowledged",_connection.securityInd.ack);
    setProperty("Ath10kSecurityIndAction",_connection.securityInd.action);
    setProperty("Ath10kConnectionCancelRequested",_connection.cancelRequested);
    setProperty("Ath10kConnectionVdevStarted",_connection.vdevStarted);
    setProperty("Ath10kConnectionVdevUp",_connection.vdevUp);
    setProperty("Ath10kPortAuthorized",_connection.portAuthorized);
    setProperty("Ath10kConnectionError",_connection.error);
    setProperty("Ath10kConnectionStatusCode",_connection.associationStatus,32);
    setProperty("Ath10kConnectionChannel",_connection.operatingChannel,32);
    setProperty("Ath10kConnectionRxChannel",_connection.ap.rxChannel,32);
    setProperty("Ath10kConnectionAdvertisedChannel",_connection.ap.advertisedChannel,32);
    setProperty("Ath10kConnectionAID",_connection.aid,32);
    setProperty("Ath10kConnectionPeerCount",_connection.peerCount,32);
    setProperty("Ath10kConnectionPeerID",_connection.peerId,32);
    setProperty("Ath10kEthernetLinkUp",_networkLinkUp);
    setProperty("Ath10kDataTxPauseMask",_connection.txPauseMask,32);
    setProperty("Ath10kCSAPhase",Ath10k::channelSwitchPhaseName(_connection.csa.phase));
    setProperty("Ath10kCSAStatus",_connection.csa.status);
    setProperty("Ath10kCSATargetChannel",UInt64(_connection.csa.primary),32);
    setProperty("Ath10kCSATargetCenter",UInt64(_connection.csa.center),32);
    setProperty("Ath10kCSACount",UInt64(_connection.csa.count),32);
    setProperty("Ath10kCSATxBlocked",_connection.csa.blocked);
    setProperty("Ath10kCSACompleted",UInt64(_connection.csa.completed),32);
    setProperty("Ath10kCSARejected",UInt64(_connection.csa.rejected),32);
    setProperty("Ath10kDataTxPauseEvents",_connection.txPauseEvents,32);
    setProperty("Ath10kDataTxPackets",_connection.txPackets,32);
    setProperty("Ath10kDataTxErrors",_connection.txErrors,32);
    setProperty("Ath10kDataRxPackets",_connection.rxPackets,32);
    setProperty("Ath10kDataRxDropped",_connection.rxDropped,32);
    setProperty("Ath10kPacketDMAOwned",_connection.txOwned || _connection.dataPending);
    const char *htPHY[]={"HT20-1SS","HT20-2SS","HT40-1SS","HT40-2SS"};
    const char *vhtPHY[]={"VHT20-1SS","VHT20-2SS","VHT40-1SS","VHT40-2SS","VHT80-1SS","VHT80-2SS"};
    const UInt32 phyIndex=(_connection.width==80?4:(_connection.width==40?2:0))+(_connection.nss==2?1:0);
    setProperty("Ath10kConnectionPHY",_connection.vhtEnabled?vhtPHY[phyIndex]:(_connection.htEnabled?htPHY[phyIndex&3]:"legacy"));
    setProperty("Ath10kConnectionWidthMHz",_connection.width,32);
    setProperty("Ath10kConnectionNSS",_connection.nss,32);
    setProperty("Ath10kConnectionCenterChannel",_connection.centerChannel,32);
    setProperty("Ath10kConnectionShortGI",_connection.shortGI);
    setProperty("Ath10kFirmwareHTCaps",_wmi.htCapability,32);
    setProperty("Ath10kFirmwareVHTCaps",_wmi.vhtCapability,32);
    setProperty("Ath10kFirmwareVHTMCS",_wmi.vhtMCS,32);
    setProperty("Ath10kFirmwareRFChains",_wmi.rfChains,32);
    setProperty("Ath10kLocalHTCaps",_connection.localHT,32);setProperty("Ath10kLocalVHTCaps",_connection.localVHT,32);
    setProperty("Ath10kNegotiatedVHTRxMap",_connection.vhtRxMap,32);setProperty("Ath10kNegotiatedVHTTxMap",_connection.vhtTxMap,32);
    setProperty("Ath10kConnectionPHYReason",_connection.phyReason);
    setProperty("Ath10kAPVHTPresent",_connection.ap.vhtPresent);
    setProperty("Ath10kAPVHTOperationPresent",_connection.ap.vhtOperation);
    setProperty("Ath10kAPVHTRxMap",Ath10k::htcReadLE16(_connection.ap.vht+4),32);
    setProperty("Ath10kAPVHTTxMap",Ath10k::htcReadLE16(_connection.ap.vht+8),32);
    setProperty("Ath10kAPVHTBasicMap",Ath10k::htcReadLE16(_connection.ap.vhtOp+3),32);
    setProperty("Ath10kAPHTCaps",Ath10k::htcReadLE16(_connection.ap.ht),32);
    setProperty("Ath10kAPHTAmpduExponent",_connection.ap.ht[2]&3,32);
    setProperty("Ath10kAPHTMPDUDensity",(_connection.ap.ht[2]>>2)&7,32);
    setProperty("Ath10kLocalHTAmpduExponent",3,32);
    setProperty("Ath10kPeerHTCaps",_connection.peerHT,32);
    setProperty("Ath10kPeerVHTCaps",_connection.peerVHT,32);
    setProperty("Ath10kAMPDULimit",Ath10k::kAMPDULimit,32);
    setProperty("Ath10kAMSDULimit",Ath10k::kAMSDULimit,32);
    setProperty("Ath10kAggregationConfigTransportComplete",_rxSetup.completed);
    setProperty("Ath10kBAAdded",_connection.baAdded,32);
    setProperty("Ath10kBADeleted",_connection.baDeleted,32);
    setProperty("Ath10kBAIgnored",_connection.baIgnored,32);
    setProperty("Ath10kBAOverflow",_connection.baOverflow,32);
    UInt32 baCount=0,baMask=0;for(const auto &ba:_connection.ba)if(ba.window){++baCount;baMask|=1u<<ba.tid;}
    setProperty("Ath10kBAActive",baCount,32);setProperty("Ath10kBATidMask",baMask,32);
    setProperty("Ath10kBAFrames",_connection.baFrames,32);
    setProperty("Ath10kBAGaps",_connection.baGaps,32);
    setProperty("Ath10kDataRxAMSDUSubframes",_connection.rxAmsduSubframes,32);
    setProperty("Ath10kDataRxChainedBuffers",_connection.rxChainedBuffers,32);
    setProperty("Ath10kDataRxChainDrops",_connection.rxChainDrops,32);
    setProperty("Ath10kRxSetupChainDrops",_rxSetup.rxChainDrops,32);
    setProperty("Ath10kConnectionWMMConfigured",_connection.wmmConfigured);
    setProperty("Ath10kDataTxCapacity",Ath10k::kDataTxSlots,32);
    setProperty("Ath10kDataTxCEObservedTotalMs",_connection.dataCETimeMs,64);
    setProperty("Ath10kDataTxHTTObservedTotalMs",_connection.dataHTTTimeMs,64);
    setProperty("Ath10kDataTxCEObservedMaxMs",_connection.dataCEMaxMs,64);
    setProperty("Ath10kDataTxHTTObservedMaxMs",_connection.dataHTTMaxMs,64);
    setProperty("Ath10kDataTxCEObservedCount",_connection.dataCECount,32);
    setProperty("Ath10kDataTxHTTObservedCount",_connection.dataHTTCount,32);
    setProperty("Ath10kDataTxPending",_connection.dataPending,32);
    setProperty("Ath10kDataTxDelayedFrames",_connection.dataDelayed,32);
    setProperty("Ath10kDataTxWatchdogTimeouts",_connection.dataTimeouts,32);
    setProperty("Ath10kDataTxQueueWatchdogArmed",_connection.dataStallArmed);
    setProperty("Ath10kDataTxPauseWatchdogArmed",_connection.dataPauseArmed);
    setProperty("Ath10kDataTxHighWater",_connection.dataHigh,32);
    setProperty("Ath10kDataTxStalls",_connection.dataStalls,32);
    setProperty("Ath10kDataTxStallManagement",_connection.stallManagement,32);
    setProperty("Ath10kDataTxStallFirmwarePause",_connection.stallFirmwarePause,32);
    setProperty("Ath10kDataTxStallSlots",_connection.stallSlots,32);
    const char *drops[]={"Ath10kRxDropStatus","Ath10kRxDropIntegrity","Ath10kRxDropLength","Ath10kRxDropPeer",
        "Ath10kRxDropDecap","Ath10kRxDropAggregate","Ath10kRxDropSecurity","Ath10kRxDropReplay","Ath10kRxDropKey",
        "Ath10kRxDropPlaintext","Ath10kRxDropAllocation","Ath10kRxDropPort","Ath10kRxDropEAPOL",
        "Ath10kRxDropFragment","Ath10kRxDropDuplicate"};
    for(UInt32 i=0;i<Ath10k::ConnectionState::DropCount;++i)setProperty(drops[i],_connection.rxReasons[i],32);
    setProperty("Ath10kRxLastDropReason",drops[_connection.rxLastReason]);
    auto &security=_connection.rxSecurity;
    const char *securityCounters[]={"Ath10kRxSecurityHeaderType","Ath10kRxSecurityDirection",
        "Ath10kRxSecurityProtected","Ath10kRxSecurityMoreFragments","Ath10kRxSecurityFragmentNumber",
        "Ath10kRxSecurityTransmitter","Ath10kRxSecurityMixedCipher"};
    for(UInt32 i=0;i<static_cast<UInt32>(Ath10k::RXSecurityFault::Count);++i)
        setProperty(securityCounters[i],UInt64(security.counts[i]),32);
    setProperty("Ath10kRxSecurityEvidenceValid",security.valid);
    setProperty("Ath10kRxSecurityLastReason",security.valid?Ath10k::rxSecurityFaultName(security.last):"none");
    setProperty("Ath10kRxSecurityFC",UInt64(security.fc),32);
    setProperty("Ath10kRxSecuritySequenceControl",UInt64(security.sequenceControl),32);
    setProperty("Ath10kRxSecurityHTTPeer",UInt64(security.peer),32);
    setProperty("Ath10kRxSecurityAttention",UInt64(security.attention),32);
    setProperty("Ath10kRxSecurityMPDUInfo",UInt64(security.info),32);
    setProperty("Ath10kRxSecurityFirstMPDUInfo",UInt64(security.firstInfo),32);
    setProperty("Ath10kRxSecurityMSDUFlags",UInt64(security.msduFlags),32);
    setProperty("Ath10kRxSecurityMSDUCount",UInt64(security.msduCount),32);
    setProperty("Ath10kRxSecurityDecap",UInt64(security.decap),32);
    setProperty("Ath10kRxSecurityLength",UInt64(security.length),32);
    setProperty("Ath10kRxSecurityOriginalHeader",security.originalHeader,sizeof(security.originalHeader));
    setProperty("Ath10kRxSecurityNativeHeaderValid",security.nativeValid);
    setProperty("Ath10kRxSecurityNativeHeader",security.nativeHeader,sizeof(security.nativeHeader));
    setProperty("Ath10kRxSecurityExpectedBSSID",security.expectedBSSID,sizeof(security.expectedBSSID));
    setProperty("Ath10kRxFirmwareReorder",_connection.firmwareReorder);
    setProperty("Ath10kRxReorderHeld",_rxReorder.held,32);setProperty("Ath10kRxReorderQueued",_rxReorder.queued,32);
    setProperty("Ath10kRxReorderReleased",_rxReorder.released,32);setProperty("Ath10kRxReorderTimeouts",_rxReorder.timeouts,32);
    setProperty("Ath10kRxReorderDuplicates",_rxReorder.duplicates,32);setProperty("Ath10kRxReorderLate",_rxReorder.late,32);
    setProperty("Ath10kRxReorderHoles",_rxReorder.holes,32);setProperty("Ath10kRxReorderOverflow",_rxReorder.overflow,32);
    setProperty("Ath10kRxFragmentCompleted",_rxDefrag.completed,32);setProperty("Ath10kRxFragmentWaiting",_rxDefrag.waiting,32);
    setProperty("Ath10kRxFragmentRejected",_rxDefrag.rejected,32);setProperty("Ath10kRxFragmentTimeouts",_rxDefrag.timeouts,32);
    setProperty("Ath10kHTTRxChainWaits",_rxSetup.rxChainWaits,32);
    setProperty("Ath10kHTTRxFragmentIndications",_rxSetup.fragIndications,32);
    setProperty("Ath10kHTTRxPNIndications",_rxSetup.pnIndications,32);
    setProperty("Ath10kDataTxCompletions",_connection.dataCompleted,32);
    setProperty("Ath10kDataTxNoAck",_connection.dataNoAck,32);
    setProperty("Ath10kDataTxDiscard",_connection.dataDiscard,32);
    setProperty("Ath10kDataTxBytes",_connection.txBytes,64);
    setProperty("Ath10kDataRxBytes",_connection.rxBytes,64);
    setProperty("Ath10kDataTxLatencyTotalMs",_connection.dataLatencyMs,64);
    setProperty("Ath10kDataTxLatencyMaxMs",_connection.dataLatencyMaxMs,64);
    setProperty("Ath10kPayloadCache",ATH10K_PAYLOAD_CACHE!=0);
    setProperty("Ath10kRxDeliveryBatchEnabled",ATH10K_RX_BATCH!=0);
    setProperty("Ath10kRuntimeIRQWorker",_runtimeIRQArmed);
    setProperty("Ath10kRuntimeIRQWorkers",_runtimeIRQWorkers,32);
    setProperty("Ath10kRuntimeIRQReschedules",_runtimeIRQReschedules,32);
    setProperty("Ath10kRuntimeMSIWorker",_runtimeIRQArmed && _irqMessaged);
    setProperty("Ath10kRuntimeMSIUnmaskStep",_runtimeIRQUnmaskStep,32);
    setProperty("Ath10kRuntimeMSIUnmaskEnableValue",_runtimeIRQUnmaskEnableValue,32);
    setProperty("Ath10kRuntimeMSIWorkers",_runtimeIRQWorkers,32);
    setProperty("Ath10kRuntimeMSIReschedules",_runtimeIRQReschedules,32);
    setProperty("Ath10kDataTxWakeups",_perf.txWakeups,32);
    setProperty("Ath10kDataTxResumePending",Ath10k::kDataTxSlots*3/4,32);
    setProperty("Ath10kOutputQueueCapacity",Ath10k::kOutputQueueCapacity,32);
    setProperty("Ath10kRxDeliveryFlushes",_perf.rxFlushes,32);
    setProperty("Ath10kRxDeliveryBatchMax",_perf.rxFlushMax,32);
    setProperty("Ath10kRxDirectBuffers",_perf.rxDirect,32);
    setProperty("Ath10kRxAssemblyCopies",_perf.rxAssembly,32);
    UInt32 count=0,hits=0,misses=0,refills=0,failures=0;
    if(_rxReserveLock){
        IOSimpleLockLock(_rxReserveLock);
        count=_rxReserveCount;hits=_perf.reserveHits;misses=_perf.reserveMisses;
        refills=_perf.reserveRefills;failures=_perf.reserveFailures;
        IOSimpleLockUnlock(_rxReserveLock);
    }
    setProperty("Ath10kRxReserveEnabled",_rxReserveTimer!=nullptr);
    setProperty("Ath10kRxReserveAvailable",count,32);
    setProperty("Ath10kRxReserveHits",hits,32);setProperty("Ath10kRxReserveMisses",misses,32);
    setProperty("Ath10kRxReserveRefills",refills,32);setProperty("Ath10kRxReserveFailures",failures,32);
    const Ath10k::Duration *durations[]={&_perf.worker,&_perf.rx,&_perf.delivery};
    const char *total[]={"Ath10kRuntimeWorkerTotalNS","Ath10kRxWorkerTotalNS","Ath10kRxDeliveryTotalNS"};
    const char *maximum[]={"Ath10kRuntimeWorkerMaxNS","Ath10kRxWorkerMaxNS","Ath10kRxDeliveryMaxNS"};
    const char *counts[]={"Ath10kRuntimeWorkerCount","Ath10kRxWorkerCount","Ath10kRxDeliveryCount"};
    for(UInt32 i=0;i<3;++i){setProperty(total[i],durations[i]->totalNS,64);
        setProperty(maximum[i],durations[i]->maxNS,64);setProperty(counts[i],durations[i]->count,32);}
    const auto &rate=_perf.rate;
    setProperty("Ath10kRxRateValid",rate.valid);
    setProperty("Ath10kRxRatePreamble",UInt64(rate.preamble),32);
    setProperty("Ath10kRxRateMCS",UInt64(rate.mcs),32);setProperty("Ath10kRxRateNSS",UInt64(rate.nss),32);
    setProperty("Ath10kRxRateWidthMHz",UInt64(rate.width),32);setProperty("Ath10kRxRateShortGI",rate.shortGI);
    setProperty("Ath10kRxRateSamples",rate.samples,32);setProperty("Ath10kRxRateInvalid",rate.invalid,32);
    setProperty("Ath10kRxRateLegacy",rate.legacy,32);setProperty("Ath10kRxRateHT",rate.ht,32);
    setProperty("Ath10kRxRateVHT",rate.vht,32);setProperty("Ath10kRxRateMU",rate.mu,32);

}
UInt32 Ath10kPCIDevice::outputPacket(mbuf_t packet,void *)
{
    if(powerIOBlocked() || !_networkLinkUp || !_networkEnabled || !_wmiLive.active || _connection.stage!=Ath10k::LinkStage::Connected){
        freePacket(packet);return kIOReturnOutputDropped;
    }
    if(!Ath10k::dataTXAdmissionOpen(_connection)){
        _dataTXStalled=true;
        ++_connection.dataStalls;
        if(_connection.txOwned || _connection.eapolPause || !_connection.portAuthorized)++_connection.stallManagement;
        else if(_connection.txPauseMask)++_connection.stallFirmwarePause;
        else ++_connection.stallSlots;
        return kIOReturnOutputStall;
    }
    UInt32 length=static_cast<UInt32>(mbuf_pkthdr_len(packet));
    UInt8 data[Ath10k::kEthernetFrameMax];
    if(length<14 || length>sizeof(data) || mbuf_copydata(packet,0,length,data) || !Ath10k::ethernetFrameAllowed(data,length) ||
       !Ath10k::sameMAC(data+6,_connection.local)){
        ++_connection.txErrors;freePacket(packet);return kIOReturnOutputDropped;
    }
    if(Ath10k::txPriorityTID(_connection,data,length)==255){++_connection.txAdmissionDropped;freePacket(packet);return kIOReturnOutputDropped;}
    uint64_t now=0,ns=0;clock_get_uptime(&now);absolutetime_to_nanoseconds(now,&ns);
    const char *error=submitDataPacket(data,length,ns/1000000u);
    freePacket(packet);
    if(error){
        ++_connection.txErrors;_connection.error=error;_connection.stage=Ath10k::LinkStage::Failed;
        // stop()/flush() from the output action deadlocks IOOutputQueue. The
        // timer will tear down the runtime after this action has returned.
        return kIOReturnOutputDropped;
    }
    return kIOReturnOutputSuccess;
}
void Ath10kPCIDevice::deliverNetworkRX(const UInt8 *data,UInt8 status,UInt16 peer,UInt16 indicatedLength,UInt32 availableBytes)
{
    if(_connection.disconnect!=Ath10k::DisconnectStage::None || !_networkEnabled || !_networkInterface || _connection.stage!=Ath10k::LinkStage::Connected)return;
    if(!data || availableBytes<300){_rxBatch.failed=true;Ath10k::rxDrop(_connection,Ath10k::ConnectionState::Length);return;}
    const UInt32 mode=(Ath10k::connLE32(data+32)>>8)&3;
    const UInt16 fc=availableBytes>=324?Ath10k::htcReadLE16(data+300):0;
    if(status==1 && availableBytes>=324 && (mode==0 || mode==1) &&
       ((fc&0x400) || (Ath10k::htcReadLE16(data+322)&15))){
        uint64_t tick=0,ns=0;clock_get_uptime(&tick);absolutetime_to_nanoseconds(tick,&ns);
        UInt32 normalized=0;
        const int result=_rxDefrag.receive(_connection,data,availableBytes,peer,indicatedLength,ns/1000000u,_rxNormalized,normalized);
        if(result<=0){_rxBatch.failed=true;return;}
        data=_rxNormalized;availableBytes=normalized;indicatedLength=0;
    }else if(status==1 && mode==0){
        UInt32 normalized=0;
        if(!Ath10k::normalizeRawRX(data,availableBytes,indicatedLength,_rxNormalized,normalized)){
            _rxBatch.failed=true;Ath10k::rxDrop(_connection,Ath10k::ConnectionState::Decap);return;}
        data=_rxNormalized;availableBytes=normalized;indicatedLength=0;
    }
    if(!Ath10k::appendRXBatch(_connection,_rxBatch,data,availableBytes,status,peer,indicatedLength)){
        setProperty("Ath10kDataRxLastAttention",Ath10k::connLE32(data+4),32);
        setProperty("Ath10kDataRxLastDecap",(Ath10k::connLE32(data+32)>>8)&3,32);
        setProperty("Ath10kDataRxLastDescriptorLength",Ath10k::connLE32(data+24)&0x3fff,32);
        setProperty("Ath10kDataRxLastIndicatedLength",indicatedLength,32);
        setProperty("Ath10kDataRxLastPeer",peer,32);setProperty("Ath10kDataRxLastStatus",status,32);
    }
}
void Ath10kPCIDevice::releaseRXBatch(Ath10k::RXBatch &b)
{
    auto &s=_connection;
    if(b.failed || !b.count || !_networkEnabled || !_networkInterface || s.stage!=Ath10k::LinkStage::Connected ||
       s.disconnect!=Ath10k::DisconnectStage::None)return;
    UInt32 eapols=0;
    if(s.secure)for(UInt32 i=0;i<b.count;++i){const UInt8 *frame=b.data+b.offsets[i];
        if(frame[12]==0x88 && frame[13]==0x8e){++eapols;
            if(b.lengths[i]>512 || !Ath10k::sameMAC(frame+6,s.bssid)){
                Ath10k::rxDrop(s,Ath10k::ConnectionState::EAPOL);return;}}
        else if(!(Ath10k::connLE32(b.descriptor+12)&(1u<<13))){Ath10k::rxDrop(s,Ath10k::ConnectionState::Plaintext);return;}
    }
    if(eapols>4-s.eapolCount){Ath10k::rxDrop(s,Ath10k::ConnectionState::EAPOL);return;}
    if(!Ath10k::acceptSecureRX(s,b.descriptor,b.data+b.offsets[0],b.lengths[0],b.peer)){
        if(s.rxLastReason==Ath10k::ConnectionState::Security){
            s.rxSecurity.msduCount=b.count;s.rxSecurity.nativeValid=b.nativeHeaderValid;
            for(UInt32 i=0;i<24;++i)s.rxSecurity.nativeHeader[i]=b.nativeHeaderValid?b.nativeHeader[i]:0;
        }
        Ath10k::rxDrop(s,s.rxLastReason);return;}
    if(b.count>1)s.rxAmsduSubframes+=b.count;
    for(UInt32 i=0;i<b.count;++i){const UInt8 *frame=b.data+b.offsets[i];const UInt32 length=b.lengths[i];
        if(s.secure && frame[12]==0x88 && frame[13]==0x8e){
            if(length>512 || s.eapolCount==4 || !Ath10k::sameMAC(frame+6,s.bssid)){Ath10k::rxDrop(s,Ath10k::ConnectionState::EAPOL);continue;}
            bcopy(frame,s.eapolFrames[s.eapolWrite],length);s.eapolLength[s.eapolWrite]=static_cast<UInt16>(length);
            s.eapolWrite=(s.eapolWrite+1)%4;++s.eapolCount;
        }else if(_networkLinkUp){
            if(!s.multicast.accepts(frame)){++s.multicast.dropped;continue;}
            mbuf_t packet=takeRXPacket(length);
            if(!packet || mbuf_copyback(packet,0,length,frame,MBUF_DONTWAIT)){
                if(packet)freePacket(packet);
                Ath10k::rxDrop(s,Ath10k::ConnectionState::Allocation);continue;}
            const UInt64 started=Ath10k::uptimeNS();
            _networkInterface->inputPacket(packet,length,_rxDeliveryBatch?IONetworkInterface::kInputOptionQueuePacket:0);
            _perf.delivery.add(Ath10k::uptimeNS()-started);
            if(_rxDeliveryBatch && ++_rxDeliveryQueued>=Ath10k::kRXDeliveryBatchLimit)flushNetworkRX();
            ++s.rxPackets;s.rxBytes+=length;
        }else Ath10k::rxDrop(s,Ath10k::ConnectionState::Port);
    }
}
void Ath10kPCIDevice::flushNetworkRX()
{
    if(!_rxDeliveryQueued)return;
    if(_networkInterface)_networkInterface->flushInputQueue();
    ++_perf.rxFlushes;if(_rxDeliveryQueued>_perf.rxFlushMax)_perf.rxFlushMax=_rxDeliveryQueued;
    _rxDeliveryQueued=0;
}
void Ath10kPCIDevice::finishNetworkRX()
{
    uint64_t tick=0,ns=0;clock_get_uptime(&tick);absolutetime_to_nanoseconds(tick,&ns);
    _rxReorder.receive(_connection,_rxBatch,ns/1000000u,[](const Ath10k::RXBatch &b){
        auto *copy=static_cast<Ath10k::RXBatch *>(IOMalloc(sizeof(b)));
        if(copy)bcopy(&b,copy,sizeof(b));
        return copy;
    },[this](Ath10k::RXBatch &b){releaseRXBatch(b);},[](Ath10k::RXBatch *b){IOFree(b,sizeof(*b));});
}
void Ath10kPCIDevice::serviceRXReorder(UInt64 nowMs)
{
    _rxDefrag.poll(nowMs);
    _rxReorder.poll(_connection,nowMs,[this](Ath10k::RXBatch &b){releaseRXBatch(b);},
        [](Ath10k::RXBatch *b){IOFree(b,sizeof(*b));});
}

// All methods below run under the shared IONetworkController/work-loop gate.
const char *Ath10kPCIDevice::submitNetworkCE(UInt32 ce,Ath10kDMABuffer &buffer,UInt32 bytes,UInt32 &next)
{
    Ath10k::CESnapshot snapshot{};
    const UInt32 entries=Ath10k::kCEHostConfig[ce].srcEntries;
    if(!bytes || bytes>Ath10k::kCEHostConfig[ce].srcSizeMax ||
       !Ath10k::dmaSpan32(buffer.dmaAddress(),bytes))return "connection-ce-size";
    if(!Ath10k::readCESnapshot(_regs,ce,snapshot) || !validateBMIPipeStructure(ce,snapshot) ||
       snapshot.srcWriteIndex>=entries || snapshot.currentSrcRead!=snapshot.srcWriteIndex ||
       !_ceSrcRings[ce].syncFromDevice())return "connection-ce-not-idle";
    auto *ring=static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[ce].virtualAddress());
    auto &desc=ring[snapshot.srcWriteIndex];bzero(&desc,sizeof(desc));
    desc.address=static_cast<UInt32>(buffer.dmaAddress());desc.nbytes=static_cast<UInt16>(bytes);
    desc.flags=static_cast<UInt16>((ce==3?_htcWMIControl.endpoint:_htcHTTData.endpoint)<<2);
    if(!buffer.syncForDevice() || !_ceSrcRings[ce].syncForDevice())return "connection-ce-sync";
    if(ce==3 && !_wmi.credits.reserve(bytes))return "connection-wmi-credit";
    next=Ath10k::ceRingNext(snapshot.srcWriteIndex,entries);
    // Ownership begins before the doorbell, including ambiguous MMIO failures.
    if(ce==3)_connection.wmiOwned=true;else _connection.txOwned=true;
    if(ce==3 && bytes>=12){
        auto &d=_connection.diagnostics;d.lastCommand=Ath10k::connLE32(static_cast<const UInt8 *>(buffer.virtualAddress())+8)&0xffffffu;
        if(d.lastCommand==0x6005 && bytes>=92){
            const auto *f=static_cast<const UInt8 *>(buffer.virtualAddress());const UInt32 flags=Ath10k::connLE32(f+36);
            auto &m=_connection.maintenance;m.peerRatesValid=true;
            m.peerNSS=static_cast<UInt8>(Ath10k::connLE32(f+64));m.peerWidth=flags&0x04000000u?80:(flags&0x2000u?40:20);
        }
        ++d.commands;d.lastCommandStage=static_cast<UInt32>(_connection.stage);d.lastCommandSecurity=_connection.securityPhase;
        d.lastCommandDisconnect=static_cast<UInt32>(_connection.disconnect);
        d.lastCommandAP=_connection.maintenance.step;d.lastCommandRadio=_connection.radio.step;
        if(d.commands<=192)IOLog("Ath10kMac: CONNECTION WMI submit cmd=0x%x stage=%u security=%u disconnect=%u ap-step=%u radio-step=%u peers=%u credit-pending=%u\n",
            d.lastCommand,d.lastCommandStage,d.lastCommandSecurity,d.lastCommandDisconnect,d.lastCommandAP,d.lastCommandRadio,
            _connection.peerCount,_wmi.credits.outstanding);
    }
    OSSynchronizeIO();UInt32 value=0xffffffffu;
    const UInt32 doorbell=Ath10k::ceBaseAddress(ce)+Ath10k::kCESrcWriteIndex;
    if(!_regs.write32(doorbell,next) || !_regs.read32(doorbell,value) || value!=next)return "connection-ce-doorbell";
    return nullptr;
}
const char *Ath10kPCIDevice::submitNetworkPacket(const UInt8 *data,UInt32 bytes,bool mgmt,UInt64 nowMs,AthSecurity::ManagementTX protection)
{
    if(powerIOBlocked() || _connection.maintenance.step || _connection.csa.blocked || _connection.radio.blocked || _connection.radio.powerBlocked || _connection.txOwned || _connection.dataPending || !_wmiLive.active || _dmaQuarantine)return "connection-tx-busy";
    if(!Ath10k::packetProtectionAllowed(data,bytes,_connection,mgmt,protection))return "packet-protection";
    if(!_packetBuffer.allocated() && !_packetBuffer.allocate(2048,128,_dmaMapper,Ath10kDMABuffer::Role::Payload))return "packet-allocation";
    if(!_packetCommand.allocated() && !_packetCommand.allocate(256,128,_dmaMapper))return "packet-command-allocation";
    if(!_packetFragments.allocated() && !_packetFragments.allocate(16,16,_dmaMapper))return "packet-fragment-allocation";
    if(!Ath10k::dmaSpan32(_packetBuffer.dmaAddress(),2048) ||
       !Ath10k::dmaSpan32(_packetFragments.dmaAddress(),16))return "packet-dma-span";
    if(!data || bytes<14 || bytes>Ath10k::kEthernetFrameMax || (!mgmt && !Ath10k::ethernetFrameAllowed(data,bytes)))return "packet-size";
    bzero(_packetBuffer.virtualAddress(),2048);bcopy(data,_packetBuffer.virtualAddress(),bytes);
    auto *frags=static_cast<UInt8 *>(_packetFragments.virtualAddress());bzero(frags,16);
    Ath10k::htcWriteLE32(frags,static_cast<UInt32>(_packetBuffer.dmaAddress()));Ath10k::htcWriteLE32(frags+4,bytes);
    // Firmware has 1056 descriptor IDs configured. Rotate IDs even with one
    // packet in flight so a late duplicate cannot complete the next packet.
    _connection.txId=static_cast<UInt16>((_connection.txId+1)%1056);_connection.txComplete=false;_connection.txStatus=0;_connection.txManagement=mgmt;
    _connection.lastTxPMF=mgmt && protection==AthSecurity::ManagementTX::HardwareCCMP;
    _connection.lastTxSAE=mgmt && bytes>=30 && (data[0]&0xfcu)==0xb0u && Ath10k::htcReadLE16(data+24)==3;
    _connection.lastTxSpectrum=mgmt && _connection.spectrum.inFlight && data==_connection.spectrum.frame;
    _connection.lastTxProbe=mgmt && _connection.probe.active && bytes>=24 && Ath10k::htcReadLE16(data)==0x40;
    UInt32 length=Ath10k::buildHTTPacket(static_cast<UInt8 *>(_packetCommand.virtualAddress()),256,data,bytes,
        static_cast<UInt32>(_packetBuffer.dmaAddress()),static_cast<UInt32>(_packetFragments.dmaAddress()),
        _htcHTTData.endpoint,_connection,mgmt,protection);
    if(!length || !_packetBuffer.syncForDevice() || !_packetFragments.syncForDevice())return "packet-sync";
    _connection.lastTxEAPOL=!mgmt && bytes>=14 && data[12]==0x88 && data[13]==0x8e;
    _connection.txSinceMs=nowMs;
    const char *error=submitNetworkCE(4,_packetCommand,length,_connection.txNext);
    if(!error)++_connection.httSequence;
    return error;
}
const char *Ath10kPCIDevice::serviceConnection(UInt64 nowMs)
{
    using Ath10k::LinkStage;auto &s=_connection;
#ifndef ATH10K_NATIVE
    // Must run before the Disabled early-return below: after a teardown the state
    // machine sits at stage Disabled, and that is exactly when a join parked during
    // the drain has to be replayed (HANDOFF §6.39).
    venturaPendingJoinService(nowMs);
#endif
    if(s.stage==LinkStage::Disabled)return nullptr;
    if(s.diagnostics.mlmeCount!=s.diagnostics.mlmeLogged){
        auto &d=s.diagnostics;d.mlmeLogged=d.mlmeCount;
        IOLog("Ath10kMac: AP MLME subtype=0x%02x reason=%u rx-channel=%u stage=%u security=%u peers=%u first-error=%s\n",
            d.mlmeSubtype,d.mlmeReason,d.mlmeChannel,static_cast<UInt32>(s.stage),s.securityPhase,s.peerCount,s.error);
    }
    const char *dataError=serviceDataTX(nowMs);if(dataError)return dataError;
#ifndef ATH10K_NATIVE
    // In-kernel WPA2 handshake: answers M1/M3 and hands the derived keys to the
    // security ladder below. Runs before it so the install sees fresh material.
    if(const char *secureError=venturaSecurityService(nowMs))return secureError;
#endif
    if(s.stage==LinkStage::Failed && s.disconnect==Ath10k::DisconnectStage::None){
        if(s.cancelRequested || (s.error && (!strcmp(s.error,"authentication-rejected") || !strcmp(s.error,"association-rejected") || !strcmp(s.error,"ap-disconnected") || !strcmp(s.error,"peer-unmapped") || !strcmp(s.error,"vdev-start-rejected"))))s.cancelRequested=true;
        else return s.error;
    }
    if(s.txOwned){
        Ath10k::CESnapshot snapshot{};
        if(!Ath10k::readCESnapshot(_regs,4,snapshot) || !validateBMIPipeStructure(4,snapshot) || snapshot.srcWriteIndex!=s.txNext)
            return "packet-ce-state";
        if(snapshot.currentSrcRead==s.txNext && s.txComplete){
            if(!_packetCommand.syncFromDevice() || !_ceSrcRings[4].syncFromDevice())return "packet-completion-sync";
            s.txOwned=false;
            if(s.txStatus){
                if(s.lastTxEAPOL)s.eapolTxFailed=true;
                ++s.txErrors;
                if(s.txManagement && !s.lastTxProbe && !s.lastTxSpectrum && !s.lastTxPMF && !s.lastTxSAE){
                    const bool mlme=s.stage==LinkStage::Auth || s.stage==LinkStage::Assoc;
                    const bool reply=s.stage==LinkStage::Auth?s.authReply:s.assocReply;
                    if(!mlme || (!reply && s.managementAttempts>=3)){s.error="management-tx-failed";s.cancelRequested=true;}
                    else if(!reply)s.managementRetryMs=nowMs+250;
                }
            }
            else if(!s.txManagement)++s.txPackets;
            if(s.lastTxEAPOL)IOLog("Ath10kMac: EAPOL TX complete id=%u status=%u security=%u peers=%u\n",s.txId,s.txStatus,s.securityPhase,s.peerCount);
            if(s.lastTxSpectrum){
                s.spectrum.inFlight=false;s.spectrum.length=0;
                if(s.txStatus)++s.spectrum.dropped;else ++s.spectrum.completed;
                s.txWakePending=true;
            }
            if(!s.txManagement && getOutputQueue())getOutputQueue()->service(1); // IOBasicOutputQueue::kServiceAsync, avoid recursive output
        }else if(nowMs-s.txSinceMs>=5000)return "packet-completion-timeout";
    }
    if((s.txWakePending || _dataTXStalled) && Ath10k::dataTXAdmissionOpen(s) && getOutputQueue()){
        s.txWakePending=false;_dataTXStalled=false;
        getOutputQueue()->service(1);
    }
    if(s.stats.step){const char *error=serviceStatsMaintenance(nowMs);if(error)return error;if(s.stats.step)return nullptr;}
    if(s.radio.step){const char *error=serviceRadioMaintenance(nowMs);if(error)return error;if(s.radio.step || s.radio.blocked)return nullptr;}
    if(s.maintenance.step){const char *error=serviceAPMaintenance(nowMs);if(error)return error;if(s.maintenance.step)return nullptr;}
    if(s.cancelRequested && !_wmiScan.completed)_wmiScan.cancelRequested=true;
    if(s.cancelRequested && s.wmiOwned){
        Ath10k::CESnapshot snap{};
        if(!Ath10k::readCESnapshot(_regs,3,snap) || !validateBMIPipeStructure(3,snap) || snap.srcWriteIndex!=s.wmiNext)return "cancel-ce-state";
        if(snap.currentSrcRead==s.wmiNext && !_wmi.credits.outstanding){
            if(!_connectionCommand.syncFromDevice() || !_ceSrcRings[3].syncFromDevice())return "cancel-ce-sync";
            s.wmiOwned=false;if(s.stage==LinkStage::Up)s.vdevUp=true;
        }else return nowMs-s.sinceMs>=5000?"cancel-ce-timeout":nullptr;
    }
    if(s.cancelRequested && s.submitted && ((s.stage==LinkStage::Start && !s.startReply) || (s.stage==LinkStage::Peer && !s.peerMapped) || (s.stage==LinkStage::Barrier && !_connectionEcho.replyReceived))){
        // The serial DMA command and its credit have been drained above. An
        // absent semantic reply is precisely why we are cancelling: requiring
        // that reply again prevented rollback from ever reaching DELETE/STOP.
        // Compensating commands and their replies/barrier remain mandatory.
        s.rollback.active=true;s.rollback.startPending=s.stage==LinkStage::Start;
        if(s.stage==LinkStage::Start)s.vdevStartIssued=true;
        if(s.stage==LinkStage::Peer)s.peerCreateIssued=true;
        if(s.stage==LinkStage::Barrier){s.rollback.echoPending=true;s.rollback.echoCookie=_connectionEcho.cookie;}
        ++s.diagnostics.rollbacks;s.diagnostics.lastRollbackStage=static_cast<UInt32>(s.stage);
        IOLog("Ath10kMac: CONNECTION ROLLBACK stage=%u cmd=0x%x elapsed=%llu first-error=%s; CE3 drained, DELETE/STOP/ECHO required\n",
            static_cast<UInt32>(s.stage),s.diagnostics.lastCommand,static_cast<unsigned long long>(nowMs-s.sinceMs),s.error);
        s.submitted=false;
    }
    // Once teardown starts, submitted belongs to its DELETE/STOP/barrier command.
    // A repeated abort must not reinterpret that command as an INSTALL_KEY wait.
    if(s.disconnect==Ath10k::DisconnectStage::None && s.cancelRequested && s.stage==LinkStage::Connected && s.secure && s.submitted &&
       ((s.securityPhase<=2 && !s.securityAck) || (s.securityPhase==4 && !_connectionEcho.replyReceived)))
        return nowMs-s.sinceMs>=5000?"cancel-security-response-timeout":nullptr;
    if(s.disconnect==Ath10k::DisconnectStage::None && s.cancelRequested && s.csa.submitted &&
       ((s.csa.phase==Ath10k::ChannelSwitchPhase::Restart && !s.csa.restartReply) ||
        (s.csa.phase==Ath10k::ChannelSwitchPhase::Barrier && !_connectionEcho.replyReceived)))
        return nowMs-s.csa.sinceMs>=5000?"cancel-channel-switch-response-timeout":nullptr;
    Ath10k::stationWatchdog(s,nowMs,!_wmiScan.completed);
    if(s.radio.step || s.radio.appliedPS || (s.stage==LinkStage::Connected && s.vdevUp && s.operatingChannel && s.disconnect==Ath10k::DisconnectStage::None)){
        const char *error=serviceRadioMaintenance(nowMs);if(error)return error;
        if(s.radio.step || s.radio.blocked)return nullptr;
    }
    if(s.cancelRequested && s.stage<=LinkStage::Scan && !s.wmiOwned && !s.txOwned){
        Ath10k::resetDisconnected(s);_controlOwner=nullptr;publishConnection();return nullptr;
    }
    if(s.cancelRequested && s.disconnect==Ath10k::DisconnectStage::None && !s.wmiOwned && !s.txOwned){
        // Completed serial command/packet ownership is drained before teardown.
        s.cancelRequested=false;s.disconnect=Ath10k::DisconnectStage::Drain;s.sinceMs=nowMs;s.submitted=false;
        s.portAuthorized=false;networkLink(false);
        s.diagnostics.lastDisconnectReason=Ath10k::connectionHasError(s)?s.error:"connection-cancelled";s.diagnostics.lastCancelOrigin=s.diagnostics.cancelOrigin;
        s.diagnostics.lastDisconnectStage=static_cast<UInt32>(s.stage);
        s.diagnostics.lastDisconnectSecurity=s.securityPhase;s.diagnostics.lastDisconnectPeers=s.peerCount;
        IOLog("Ath10kMac: DISCONNECT begin reason=%s stage=%u security=%u peers=%u\n",
              s.error,static_cast<UInt32>(s.stage),s.securityPhase,s.peerCount);
    }
    if(s.disconnect!=Ath10k::DisconnectStage::None)return serviceDisconnect(nowMs);
    if(s.stage==LinkStage::Connected && s.csa.phase!=Ath10k::ChannelSwitchPhase::Idle)return serviceChannelSwitch(nowMs);
    if(s.stage==LinkStage::Connected && s.pmfTXPending && !s.maintenance.step && !s.stats.step)return nullptr;
    if(s.stage==LinkStage::Connected && s.probe.active && !s.cancelRequested){
        if(s.txOwned || s.dataPending || s.wmiOwned || s.submitted || _wmi.credits.outstanding || !_wmiScan.completed ||
           s.radio.appliedPS || s.radio.step || s.radio.blocked || s.radio.powerBlocked)return nullptr;
        if(nowMs>=s.probe.deadlineMs){
            if(s.probe.attempts>=3){s.error="ap-probe-timeout";s.cancelRequested=true;s.probe={};return nullptr;}
            const bool mayInitiate=Ath10k::regulatoryPower(s.regulatory,s.operatingChannel,s.centerChannel,s.width)!=0 &&
                Ath10k::countryPower(s.regulatory.country,s.operatingChannel,20,s.regulatory.indoor,true,s.regulatory.stationDFS,true)!=0 &&
                (!s.regulatory.boardCountry || Ath10k::countryPower(s.regulatory.boardCountry,s.operatingChannel,20,s.regulatory.indoor,true,s.regulatory.stationDFS,true)!=0);
            if(mayInitiate){UInt8 frame[128]{};const UInt32 n=Ath10k::buildAPProbe(frame,sizeof(frame),s);
                if(!n)return "ap-probe-build";
                const char *error=submitNetworkPacket(frame,n,true,nowMs);if(error)return error;s.lastTxProbe=true;}
            ++s.probe.attempts;s.probe.deadlineMs=nowMs+1000;
        }
        return nullptr;
    }
    if(s.stage==LinkStage::Connected){
        if(!s.secure || (s.portAuthorized && !s.rekeyKind)){
            const char *error=serviceAPMaintenance(nowMs);if(error || s.maintenance.step || s.maintenance.blocked)return error;
            if(s.spectrum.pending){
                auto &r=s.spectrum;if(!r.sinceMs)r.sinceMs=nowMs?nowMs:1;
                if(nowMs>=r.sinceMs && nowMs-r.sinceMs>=5000){r.pending=false;r.length=0;++r.dropped;s.txWakePending=true;return nullptr;}
                // Native PMF service owns protection, key epochs and the shared
                // management PN lease. Never submit this robust report raw.
                if(s.pmfRequired && s.pairInstalled)return nullptr;
                if(s.txOwned || s.dataPending || s.wmiOwned || s.submitted || s.eapolPause || s.txPauseMask ||
                   _wmi.credits.outstanding || !_wmiScan.completed || _wmiScan.txOwned || s.radio.appliedPS || s.radio.blocked || s.radio.powerBlocked)return nullptr;
                Ath10k::htcWriteLE16(r.frame+22,UInt16((s.sequence&4095)<<4));
                if(r.frame[25]==3 && r.length==31)r.frame[29]=s.radio.appliedPower/2;
                r.pending=false;r.inFlight=true;
                error=submitNetworkPacket(r.frame,r.length,true,nowMs);if(error)return error;
                ++s.sequence;return nullptr;
            }
            if(s.spectrum.inFlight)return nullptr;
            return serviceStatsMaintenance(nowMs);}
        if(nowMs-s.sinceMs>=15000){s.error="wpa2-handshake-timeout";s.cancelRequested=true;return nullptr;}
        if(!s.securityPhase || s.securityPhase>=5 || (s.securityPhase==3 && !s.authorizeRequested))return nullptr;
        if(!s.submitted){
            if(s.txOwned || s.wmiOwned || s.dataPending || !_wmiScan.completed || _wmiScan.txOwned || _wmi.credits.outstanding)return nullptr;
#ifdef ATH10K_NATIVE
            // Gate the firmware peer authorization as well as the later host
            // controlled-port publication. PMF keys must be ready for both.
            if(s.securityPhase==3 && !nativePMFPortReady())return "native-pmf-peer-not-ready";
#endif
            UInt32 bytes=Ath10k::buildSecurityWMI(static_cast<UInt8 *>(_connectionCommand.virtualAddress()),256,_htcWMIControl.endpoint,s);
            if(!bytes)return "security-command-build";
            if(s.securityPhase==4){_connectionEcho={};_connectionEcho.cookie=0xa7100057;_connectionEcho.submitted=true;}
            s.submitted=true;s.securityAck=false;
            const char *error=submitNetworkCE(3,_connectionCommand,bytes,s.wmiNext);if(error)return error;
            ++s.wmiSequence;return nullptr;
        }
        if(s.wmiOwned){
            Ath10k::CESnapshot snapshot{};
            if(!Ath10k::readCESnapshot(_regs,3,snapshot) || !validateBMIPipeStructure(3,snapshot) || snapshot.srcWriteIndex!=s.wmiNext)return "security-ce-state";
            if(snapshot.currentSrcRead!=s.wmiNext || _wmi.credits.outstanding)return nullptr;
            if(!_connectionCommand.syncFromDevice() || !_ceSrcRings[3].syncFromDevice())return "security-ce-sync";
            s.wmiOwned=false;
            AthSecurity::wipe(_connectionCommand.virtualAddress(),256);
        }
        if(s.securityPhase<=2 && !s.securityAck)return nullptr;
        if(s.securityPhase==4 && (!_connectionEcho.replyReceived || (s.rekeyKind && _rxSetup.pendingMPDUs)))return nullptr;
        if(s.securityPhase==1){
#ifdef ATH10K_NATIVE
            if(!nativePMFPairwiseAcknowledged(s.temporalKey))return "native-pmf-pairwise-epoch";
#endif
            bcopy(s.temporalKey,s.installedPair,16);s.pairInstalled=true;
        }
        if(s.securityPhase==2){bcopy(s.groupKey,s.installedGroups[s.groupIndex],32);s.groupInstalled[s.groupIndex]=true;}
        if(s.securityPhase==1 && s.rekeySkipGroup)s.securityPhase=2;
        if(s.securityPhase==2){AthSecurity::wipe(s.temporalKey,16);AthSecurity::wipe(s.groupKey,32);}
        ++s.securityPhase;s.submitted=false;
        if(s.rekeyKind && s.securityPhase==3)s.securityPhase=4;
        if(s.securityPhase==5){
            if(s.rekeyKind){
                flushNetworkRX();
                _rxReorder.discardForKeyChange([](Ath10k::RXBatch *b){IOFree(b,sizeof(*b));});
                _rxDefrag.reset();_rxBatch.reset();
                if(s.rekeyKind==2){bzero(s.pairPN,sizeof(s.pairPN));bzero(s.pairSeqValid,sizeof(s.pairSeqValid));}
                if(!s.rekeySkipGroup){UInt64 pn=0;for(unsigned i=0;i<6;++i)pn|=UInt64(s.groupRSC[i])<<(8*i);for(auto &v:s.groupPN)v=pn;bzero(s.groupSeqValid,sizeof(s.groupSeqValid));}
                s.rekeyKind=0;s.rekeySkipGroup=false;
            }
#ifdef ATH10K_NATIVE
            if(!nativePMFPortReady())return "native-pmf-port-not-ready";
#endif
            ++s.securityGeneration;s.portAuthorized=true;networkLink(true);
            IOLog("Ath10kMac: WPA2 port authorized generation=%u peer=%u peers=%u channel=%u ap-step=%u\n",
                s.securityGeneration,s.peerId,s.peerCount,s.operatingChannel,s.maintenance.step);
#ifndef ATH10K_NATIVE
            // The client's join does not poll to completion: Apple80211Associate2 waits
            // on the kern_event stream and gives up after 10.2 s with -3905 (measured,
            // even though this driver had the link up in ~2 s). Post what it waits for.
            venturaPostLinkEvents(true);
#endif
            if(getOutputQueue())getOutputQueue()->service(1);
        }
        publishConnection();return nullptr;
    }
    if(s.stage==LinkStage::Scan){
        if(!_wmiScan.completed || !_networkEnabled)return nullptr;
        if(_rxSetup.major!=3 || (_wmi.serviceBitmap[76/4]&(1u<<(76%4))))return "management-tx-abi-not-supported";
        s.firmwareHT=_wmi.htCapability;s.firmwareVHT=_wmi.vhtCapability;
        s.firmwareMFPSupport=payload().kSupportsMFP;s.pmfQoSTransported=_wmiRadio[Ath10k::kWMIPMFQoSIndex].completed;
        s.rfChains=static_cast<UInt8>(_wmiInit.requestedChainMask==1?1:2);
        s.regulatory=_regulatory;
#ifdef ATH10K_NATIVE
        const char *error=nativeSelectBSS(nowMs);
#else
        const char *error=Ath10k::selectOpenBSS(s,_wmiBSS);
#endif
        if(error)return error;
        s.stage=LinkStage::Start;s.sinceMs=nowMs;
        if(!_connectionCommand.allocated() && !_connectionCommand.allocate(256,128,_dmaMapper))return "connection-command-allocation";
    }
#ifdef ATH10K_NATIVE
    if(s.stage==LinkStage::Auth && s.securityAKM==8){
        const char *error=serviceNativeSAE(nowMs);
        if(error){s.error=error;s.cancelRequested=true;discardNativeSecurity();return nullptr;}
        if(!s.authReply || s.txOwned || !s.txComplete)return nullptr;
    }
#endif
    if(nowMs-s.sinceMs>=5000 && !s.wmiOwned && !s.txOwned){
        Ath10k::connectionFailure(s,"connection-stage-timeout");s.cancelRequested=true;
        IOLog("Ath10kMac: CONNECTION timeout stage=%u cmd=0x%x start-reply=%u peer-mapped=%u echo-reply=%u peers=%u\n",
            static_cast<UInt32>(s.stage),s.diagnostics.lastCommand,s.startReply,s.peerMapped,_connectionEcho.replyReceived,s.peerCount);
        return nullptr;
    }
    const bool management=s.stage==LinkStage::Auth || s.stage==LinkStage::Assoc;
    if(management && s.submitted && !s.txOwned && s.txComplete &&
       !(s.stage==LinkStage::Auth?s.authReply:s.assocReply) && nowMs>=s.managementRetryMs){
        if(s.managementAttempts>=3){s.error="management-response-timeout";s.cancelRequested=true;return nullptr;}
        s.submitted=false;
    }
    if(!s.submitted){
        if(s.txOwned || s.wmiOwned)return nullptr;
        if(management){
            if(s.txPauseMask)return nullptr;
            UInt8 frame[256];UInt32 length=0;
#ifdef ATH10K_NATIVE
            if(s.stage==LinkStage::Assoc && s.securityAKM==8)length=buildNativeSAEAssociation(frame,sizeof(frame));
            else
#endif
            length=Ath10k::buildStationManagement(frame,sizeof(frame),s);
            if(!length)return "management-build";
            s.submitted=true;
            const char *error=submitNetworkPacket(frame,length,true,nowMs);if(error)return error;
            ++s.managementAttempts;s.managementRetryMs=nowMs+1000;
            ++s.sequence;
        }else{
            UInt32 bytes=Ath10k::buildConnectionWMI(static_cast<UInt8 *>(_connectionCommand.virtualAddress()),256,_htcWMIControl.endpoint,s);
            if(!bytes)return "connection-command-build";
            if(s.stage==LinkStage::Start){s.radio.targetDTIM=s.ap.timPresent?s.ap.dtimPeriod:0;s.radio.targetBeacon=s.ap.beaconInterval;}
            if(s.stage==LinkStage::Barrier){_connectionEcho={};_connectionEcho.cookie=0xa7100047;_connectionEcho.submitted=true;}
            if(s.stage==LinkStage::Start)s.vdevStartIssued=true;
            if(s.stage==LinkStage::Peer)s.peerCreateIssued=true;
            s.submitted=true;
            const char *error=submitNetworkCE(3,_connectionCommand,bytes,s.wmiNext);if(error)return error;
            ++s.wmiSequence;
        }
        return nullptr;
    }
    if(s.wmiOwned){
        Ath10k::CESnapshot snapshot{};
        if(!Ath10k::readCESnapshot(_regs,3,snapshot) || !validateBMIPipeStructure(3,snapshot) || snapshot.srcWriteIndex!=s.wmiNext)
            return "connection-wmi-state";
        if(snapshot.currentSrcRead!=s.wmiNext || _wmi.credits.outstanding)return nullptr;
        if(!_connectionCommand.syncFromDevice() || !_ceSrcRings[3].syncFromDevice())return "connection-wmi-completion-sync";
        s.wmiOwned=false;if(s.stage==LinkStage::Up)s.vdevUp=true;
    }
    if(s.stage==LinkStage::PeerAssoc && s.ap.wmmPresent && !s.wmmConfigured){
        s.wmmConfigured=true;s.submitted=false;s.sinceMs=nowMs;return nullptr;
    }
    if(management && (s.txOwned || !s.txComplete))return nullptr;
    if(s.stage==LinkStage::Start && !s.startReply)return nullptr;
    if(s.stage==LinkStage::Start){s.radio.appliedDTIM=s.radio.targetDTIM;s.radio.appliedBeacon=s.radio.targetBeacon;}
    if(s.stage==LinkStage::Peer && !s.peerMapped)return nullptr;
    if(s.stage==LinkStage::Auth && !s.authReply)return nullptr;
    if(s.stage==LinkStage::Assoc && !s.assocReply)return nullptr;
    if(s.stage==LinkStage::Barrier && !_connectionEcho.replyReceived)return nullptr;
    s.stage=static_cast<LinkStage>(static_cast<UInt32>(s.stage)+1);s.submitted=false;s.sinceMs=nowMs;
    s.managementAttempts=0;s.managementRetryMs=0;
    if(s.stage==LinkStage::Connected){
        s.lastBeaconMs=nowMs;s.radio.appliedPower=s.radio.vdevPower=s.txPowerHalfDbm;
        s.maintenance.blocked=true;s.portAuthorized=!s.secure;networkLink(!s.secure);
        IOLog("Ath10kMac: ASSOCIATION complete aid=%u peer=%u; controlled port follows security state\n",s.aid,s.peerId);
    }
    publishConnection();return nullptr;
}

const char *Ath10kPCIDevice::serviceAPMaintenance(UInt64 nowMs)
{
    auto &s=_connection;auto &m=s.maintenance;
    if(!m.step){
        if(s.cancelRequested || s.disconnect!=Ath10k::DisconnectStage::None){m.blocked=false;m.draining=false;return nullptr;}
        if(s.stats.step || s.submitted || s.radio.step || s.radio.blocked || s.radio.powerBlocked || s.rekeyKind || s.eapolPause ||
           s.csa.phase!=Ath10k::ChannelSwitchPhase::Idle || s.probe.active || !s.portAuthorized)return nullptr;
        const auto desired=Ath10k::desiredAPConfig(s);
        if(m.valid && Ath10k::sameAPConfig(desired,m.applied)){m.blocked=false;m.draining=false;return nullptr;}
        // Finish scan cancellation before starting the independent DMA drain
        // deadline. Scan service retains its own bounded firmware timeout.
        if(!m.draining && !_wmiScan.completed){
            m.blocked=true;_wmiScan.cancelRequested=true;return nullptr;
        }
        if(!m.draining){m.draining=true;m.blocked=true;m.sinceMs=nowMs;}
        if(nowMs-m.sinceMs>=5000)return "ap-maintenance-drain-timeout";
        if(s.wmiOwned || s.txOwned || s.dataPending || !_wmiScan.completed || _wmiScan.txOwned || _wmi.credits.outstanding)return nullptr;
        m.target=desired;m.ratesPending=s.htEnabled && (!m.peerRatesValid || m.peerNSS!=desired.nss || m.peerWidth!=desired.width);
        ++m.generation;m.step=1;m.submitted=false;m.sinceMs=nowMs;
    }
    if(nowMs-m.sinceMs>=5000)return "ap-maintenance-timeout";
    if(!m.submitted){
        // Skip unchanged fields, retaining the complete transaction through ECHO.
        while(m.step<8){bool skip=false;const auto &a=m.applied;const auto &t=m.target;
            if(m.step==1){skip=!t.wmmPresent;
                if(m.valid){skip=true;for(UInt32 i=0;i<16;++i)if(a.wmm[i]!=t.wmm[i])skip=false;
                    if(a.wmmPresent!=t.wmmPresent || a.wmmCount!=t.wmmCount)skip=false;
                    if(!t.wmmPresent)skip=true;}}
            else if(m.step==2)skip=m.valid && a.slot==t.slot;
            else if(m.step==3)skip=m.valid && a.preamble==t.preamble;
            else if(m.step==4 || m.step==5)skip=m.valid && a.protection==t.protection;
            else if(m.step==6)skip=!s.htEnabled || (m.valid && a.smps==t.smps);
            else if(m.step==7)skip=!s.htEnabled || (m.valid && a.nss==t.nss);
            if(!skip)break;
            ++m.step;
        }
        if(m.step==8 && m.ratesPending)m.step=9;
        if(s.wmiOwned || _wmi.credits.outstanding)return nullptr;
        const UInt32 n=Ath10k::buildAPMaintenance(static_cast<UInt8 *>(_connectionCommand.virtualAddress()),256,_htcWMIControl.endpoint,s);
        if(!n)return "ap-maintenance-build";
        if(m.step==8){_connectionEcho={};_connectionEcho.cookie=Ath10k::apMaintenanceCookie(s);_connectionEcho.submitted=true;}
        m.submitted=true;m.sinceMs=nowMs;
        const char *error=submitNetworkCE(3,_connectionCommand,n,s.wmiNext);if(error)return error;
        ++s.wmiSequence;return nullptr;
    }
    if(s.wmiOwned){Ath10k::CESnapshot snap{};
        if(!Ath10k::readCESnapshot(_regs,3,snap) || !validateBMIPipeStructure(3,snap) || snap.srcWriteIndex!=s.wmiNext)return "ap-maintenance-ce-state";
        if(snap.currentSrcRead!=s.wmiNext || _wmi.credits.outstanding)return nullptr;
        if(!_connectionCommand.syncFromDevice() || !_ceSrcRings[3].syncFromDevice())return "ap-maintenance-ce-sync";
        s.wmiOwned=false;
    }
    if(m.step==8){
        if(!_connectionEcho.replyReceived || _connectionEcho.cookie!=Ath10k::apMaintenanceCookie(s))return nullptr;
        m.applied=m.target;m.valid=true;Ath10k::applyPeerRates(s,m.target.nss,m.target.width);
        m.peerRatesValid=true;m.peerNSS=m.target.nss;m.peerWidth=m.target.width;
        ++m.completed;m.step=0;m.submitted=false;
        m.blocked=!Ath10k::sameAPConfig(Ath10k::desiredAPConfig(s),m.applied);m.draining=false;m.sinceMs=nowMs;
        if(!m.blocked && !s.cancelRequested && !s.csa.blocked && !s.radio.blocked && !s.radio.powerBlocked && !s.eapolPause && getOutputQueue())getOutputQueue()->service(1);
        publishConnection();return nullptr;
    }
    if(m.step==11){m.ratesPending=false;m.step=8;}else ++m.step;
    m.submitted=false;m.sinceMs=nowMs;return nullptr;
}

const char *Ath10kPCIDevice::serviceStatsMaintenance(UInt64 nowMs)
{
    auto &s=_connection;auto &m=s.stats;
    if(!m.step){
        if(!m.nextMs){m.nextMs=nowMs+10000;return nullptr;}
        if(nowMs<m.nextMs || s.stage!=Ath10k::LinkStage::Connected || !s.portAuthorized || s.cancelRequested ||
           s.disconnect!=Ath10k::DisconnectStage::None || s.submitted || s.rekeyKind || s.eapolPause ||
           s.radio.step || s.radio.blocked || s.radio.powerBlocked || s.maintenance.step || s.maintenance.blocked ||
           s.probe.active || s.csa.phase!=Ath10k::ChannelSwitchPhase::Idle || s.wmiOwned ||
           !_wmiScan.completed || _wmiScan.txOwned || _wmi.credits.outstanding)return nullptr;
        m.step=1;m.submitted=false;++m.generation;m.sinceMs=nowMs;
    }
    if(nowMs-m.sinceMs>=5000)return "stats-maintenance-timeout";
    if(!m.submitted){
        if(s.wmiOwned || _wmi.credits.outstanding)return nullptr;
        const UInt32 n=Ath10k::buildStatsRequest(static_cast<UInt8 *>(_connectionCommand.virtualAddress()),256,_htcWMIControl.endpoint,s);
        if(!n)return "stats-maintenance-build";
        if(m.step==2){_connectionEcho={};_connectionEcho.cookie=Ath10k::statsCookie(s);_connectionEcho.submitted=true;}
        m.submitted=true;m.sinceMs=nowMs;
        const char *error=submitNetworkCE(3,_connectionCommand,n,s.wmiNext);if(error)return error;
        ++s.wmiSequence;return nullptr;
    }
    if(s.wmiOwned){Ath10k::CESnapshot snap{};
        if(!Ath10k::readCESnapshot(_regs,3,snap) || !validateBMIPipeStructure(3,snap) || snap.srcWriteIndex!=s.wmiNext)return "stats-maintenance-ce-state";
        if(snap.currentSrcRead!=s.wmiNext || _wmi.credits.outstanding)return nullptr;
        if(!_connectionCommand.syncFromDevice() || !_ceSrcRings[3].syncFromDevice())return "stats-maintenance-ce-sync";
        s.wmiOwned=false;
    }
    if(m.step==1){m.step=2;m.submitted=false;m.sinceMs=nowMs;return nullptr;}
    if(!_connectionEcho.replyReceived || _connectionEcho.cookie!=Ath10k::statsCookie(s))return nullptr;
    // Statistics may be absent or have an unsupported layout. Only the WMI
    // credit/CE/ECHO barrier confirms ownership; a stats event never does.
    m.step=0;m.submitted=false;++m.completed;m.nextMs=nowMs+10000;
    publishConnection();return nullptr;
}

const char *Ath10kPCIDevice::serviceRadioMaintenance(UInt64 nowMs)
{
    auto &s=_connection;auto &r=s.radio;
    if(!r.step){
        // Own the complete WMI transaction, including the period after CE completion.
        if(s.submitted || s.stats.step || s.maintenance.step || s.csa.submitted || s.csa.phase>Ath10k::ChannelSwitchPhase::Drain ||
           s.disconnect>Ath10k::DisconnectStage::Drain){r.blocked=false;return nullptr;}
        bool desired=r.requestedPS && s.disconnect==Ath10k::DisconnectStage::None && s.ap.timPresent && s.portAuthorized && !s.cancelRequested && !s.rekeyKind && !s.eapolPause &&
            !s.probe.active && !s.spectrum.pending && !s.spectrum.inFlight && !s.pmfTXPending && !r.txWakeRequested && nowMs>=r.txWakeHoldUntilMs && s.csa.phase==Ath10k::ChannelSwitchPhase::Idle && _wmiScan.completed;
        UInt8 power=Ath10k::stationOperationalPowerLimit(s);
        if(!power){s.error="ap-power-constraint-forbids-tx";s.cancelRequested=true;r.powerBlocked=true;desired=false;
            if(!r.appliedPS){r.blocked=false;return nullptr;}
            power=r.appliedPower;
        }
        const bool timing=!s.cancelRequested && s.ap.timPresent && (r.appliedDTIM!=s.ap.dtimPeriod || r.appliedBeacon!=s.ap.beaconInterval);
        if(desired==r.appliedPS && power==r.appliedPower && !timing){
            if(r.blocked){s.txWakePending=true;}r.blocked=false;
            if(!s.cancelRequested){r.powerBlocked=false;}return nullptr;}
        if(!r.blocked){r.blocked=true;r.sinceMs=nowMs;}
        if(nowMs-r.sinceMs>=5000)return "radio-maintenance-drain-timeout";
        // A wake-only PS disable on CE3 must not wait for firmware-owned
        // CE4 payloads: those payloads may be waiting for this very wake.
        // Power reduction/timing updates still require the normal drain.
        const bool wakeOnly=s.dataPending && r.appliedPS && !desired && !r.powerBlocked && power>=r.appliedPower;
        if(s.wmiOwned || s.txOwned || (s.dataPending && !wakeOnly) || _wmi.credits.outstanding || _wmiScan.txOwned)return nullptr;
        r.wakeOnly=wakeOnly;
        r.targetPS=desired;r.targetPower=power;r.targetDTIM=s.ap.dtimPeriod;r.targetBeacon=s.ap.beaconInterval;
        r.submitted=false;r.step=wakeOnly?6:(timing?(r.appliedPS?10:9):(desired&&!r.configuredPS?1:6));
        if(wakeOnly){r.targetPower=r.appliedPower;r.targetDTIM=r.appliedDTIM;r.targetBeacon=r.appliedBeacon;}
        ++r.generation;r.sinceMs=nowMs;
    }
    if(nowMs-r.sinceMs>=5000)return "radio-maintenance-timeout";
    if(!r.submitted){
        if(s.wmiOwned || _wmi.credits.outstanding)return nullptr;
        const UInt32 n=Ath10k::buildRadioMaintenance(static_cast<UInt8 *>(_connectionCommand.virtualAddress()),256,_htcWMIControl.endpoint,s);
        if(!n)return "radio-maintenance-build";
        if(r.step==8){_connectionEcho={};_connectionEcho.cookie=Ath10k::radioCookie(s);_connectionEcho.submitted=true;}
        r.submitted=true;r.sinceMs=nowMs;
        const char *error=submitNetworkCE(3,_connectionCommand,n,s.wmiNext);if(error)return error;
        ++s.wmiSequence;return nullptr;
    }
    if(s.wmiOwned){
        Ath10k::CESnapshot snap{};
        if(!Ath10k::readCESnapshot(_regs,3,snap) || !validateBMIPipeStructure(3,snap) || snap.srcWriteIndex!=s.wmiNext)return "radio-maintenance-ce-state";
        if(snap.currentSrcRead!=s.wmiNext || _wmi.credits.outstanding)return nullptr;
        if(!_connectionCommand.syncFromDevice() || !_ceSrcRings[3].syncFromDevice())return "radio-maintenance-ce-sync";
        s.wmiOwned=false;
    }
    if(r.step==8){
        if(!_connectionEcho.replyReceived || _connectionEcho.cookie!=Ath10k::radioCookie(s))return nullptr;
        r.appliedPS=r.targetPS;r.configuredPS=r.configuredPS||r.targetPS;r.appliedPower=r.targetPower;
        r.appliedDTIM=r.targetDTIM;r.appliedBeacon=r.targetBeacon;s.txPowerHalfDbm=r.targetPower;
        r.step=0;r.submitted=false;r.wakeOnly=false;++r.completed;s.lastBeaconMs=nowMs;
        r.powerBlocked=!Ath10k::stationOperationalPowerLimit(s) || Ath10k::stationOperationalPowerLimit(s)<r.appliedPower;
        const bool stale=Ath10k::stationOperationalPowerLimit(s)!=r.targetPower ||
            (s.ap.timPresent && (s.ap.dtimPeriod!=r.targetDTIM || s.ap.beaconInterval!=r.targetBeacon));
        r.blocked=stale;r.sinceMs=nowMs;
        if(!r.blocked && !r.powerBlocked && !s.csa.blocked && !s.probe.active && !s.eapolPause && !s.cancelRequested && getOutputQueue())getOutputQueue()->service(1);
        publishConnection();return nullptr;
    }
    if(r.step==10){r.appliedPS=false;r.step=9;}
    else if(r.step==9)r.step=11;
    else if(r.step==11){r.configuredPS=false;r.step=r.targetPS?1:6;}
    else if(r.step==6 && (r.wakeOnly || !Ath10k::stationOperationalPowerLimit(s)))r.step=8;
    else ++r.step;
    r.submitted=false;r.sinceMs=nowMs;return nullptr;
}

const char *Ath10kPCIDevice::serviceChannelSwitch(UInt64 nowMs)
{
    using P=Ath10k::ChannelSwitchPhase;auto &s=_connection;auto &c=s.csa;
    auto abort=[&](const char *reason)->const char *{c.status=reason;c.blocked=true;s.error=reason;
        s.cancelRequested=true;c.phase=P::Idle;publishConnection();return nullptr;};
    if(c.phase==P::Announced){
        if(!_wmiScan.completed)_wmiScan.cancelRequested=true;
        if(!c.dueMs || c.armedGeneration!=c.generation || c.count<c.armedCount){
            UInt64 interval=s.ap.beaconInterval?UInt64(s.ap.beaconInterval)*1024/1000:100;
            const UInt64 delay=UInt64(c.count)*interval;
            if(delay>30000)return abort("channel-switch-countdown-too-long");
            const UInt64 due=nowMs+delay;
            if(!c.dueMs || due<c.dueMs)c.dueMs=due;
            c.armedCount=c.count;c.armedGeneration=c.generation;
        }
        // Stop submitting ahead of the deadline so outstanding DMA can drain.
        if(c.mode || c.dueMs<=nowMs+100)c.blocked=true;
        if(nowMs<c.dueMs)return nullptr;
        c.blocked=true;c.phase=P::Drain;c.sinceMs=nowMs;c.status="draining";
        IOLog("Ath10kMac: CSA drain old=%u new=%u center=%u width=%u mode=%u\n",s.operatingChannel,c.primary,c.center,c.width,c.mode);
    }
    if(nowMs-c.sinceMs>=5000){
        // Unknown DMA ownership is escalated to the existing quiesce path.
        if(s.wmiOwned)return "channel-switch-command-timeout";
        return abort("channel-switch-timeout");
    }
    if(c.phase==P::Drain){
        if(!_wmiScan.completed){_wmiScan.cancelRequested=true;return nullptr;}
        if(_wmiScan.txOwned || s.txOwned || s.dataPending || s.wmiOwned || _wmi.credits.outstanding || s.rekeyKind)return nullptr;
        c.phase=P::Down;c.submitted=false;c.sinceMs=nowMs;
    }
    if(c.phase==P::Confirm){
        if(!c.confirmed)return nullptr;
        ++c.completed;c.phase=P::Idle;c.blocked=false;c.status="complete";s.lastBeaconMs=nowMs;
        if(!s.radio.powerBlocked && getOutputQueue())getOutputQueue()->service(1);
        IOLog("Ath10kMac: CSA complete channel=%u center=%u width=%u; keys/replay state retained\n",s.operatingChannel,s.centerChannel,s.width);
        publishConnection();return nullptr;
    }
    if(!c.submitted){
        if(s.wmiOwned || _wmi.credits.outstanding)return nullptr;
        if(c.phase==P::Restart){s.radio.targetDTIM=s.ap.timPresent?s.ap.dtimPeriod:0;s.radio.targetBeacon=s.ap.beaconInterval;
            const UInt8 limit=Ath10k::stationPowerLimitFor(s,c.primary,c.center,c.width,c.countryPresent?c.country:nullptr,c.countryLength);
            if(!limit)return abort("channel-switch-power-constraint-forbids-tx");
            if(limit<c.power)c.power=limit;}
        UInt32 bytes=Ath10k::buildChannelSwitchWMI(static_cast<UInt8 *>(_connectionCommand.virtualAddress()),256,_htcWMIControl.endpoint,s);
        if(!bytes)return abort("channel-switch-build");
        if(c.phase==P::Barrier){_connectionEcho={};_connectionEcho.cookie=Ath10k::channelSwitchCookie(s);_connectionEcho.submitted=true;}
        c.submitted=true;c.sinceMs=nowMs;s.sinceMs=nowMs;
        const char *error=submitNetworkCE(3,_connectionCommand,bytes,s.wmiNext);if(error)return error;
        ++s.wmiSequence;publishConnection();return nullptr;
    }
    if(s.wmiOwned){
        Ath10k::CESnapshot snapshot{};
        if(!Ath10k::readCESnapshot(_regs,3,snapshot) || !validateBMIPipeStructure(3,snapshot) || snapshot.srcWriteIndex!=s.wmiNext)return "channel-switch-ce-state";
        if(snapshot.currentSrcRead!=s.wmiNext || _wmi.credits.outstanding)return nullptr;
        if(!_connectionCommand.syncFromDevice() || !_ceSrcRings[3].syncFromDevice())return "channel-switch-ce-sync";
        s.wmiOwned=false;
    }
    if(c.phase==P::Restart){
        if(!c.restartReply)return nullptr;
        if(c.failed)return abort("vdev-restart-rejected");
        const UInt8 oldWidth=s.width,oldPeerWidth=Ath10k::peerBandwidth(s),oldNSS=s.nss;
        s.operatingChannel=c.primary;s.centerChannel=c.center;s.txPowerHalfDbm=c.power;s.width=c.width;
        if(c.countryPresent){s.ap.countryPresent=true;s.ap.countryLength=c.countryLength;
            for(UInt32 i=0;i<c.countryLength;++i)s.ap.country[i]=c.country[i];}
        s.radio.appliedDTIM=s.radio.targetDTIM;s.radio.appliedBeacon=s.radio.targetBeacon;
        s.radio.vdevPower=s.radio.appliedPower=c.power;s.radio.powerBlocked=Ath10k::stationOperationalPowerLimit(s)<c.power;
        s.ap.rxChannel=s.ap.advertisedChannel=c.primary;s.ap.htOp[0]=c.primary;
        if(s.htEnabled){s.ap.htOp[1]=(s.ap.htOp[1]&~7u)|(s.width>=40?(4|(s.centerChannel>s.operatingChannel?1:3)):0);
            s.shortGI=(s.peerHT&s.localHT&(s.width>=40?0x40:0x20))!=0;}
        if(s.vhtEnabled){s.ap.vhtOp[0]=s.width==80?1:0;s.ap.vhtOp[1]=s.width==80?c.center:0;s.ap.vhtOp[2]=0;}
        const auto desired=Ath10k::desiredAPConfig(s);Ath10k::applyPeerRates(s,desired.nss,desired.width);
        s.maintenance.blocked=true;
        if(oldWidth==s.width && oldPeerWidth==desired.width && oldNSS==desired.nss){c.phase=P::Up;c.submitted=false;c.sinceMs=nowMs;publishConnection();return nullptr;}
    }
    if(c.phase==P::Down)s.vdevUp=false;
    if(c.phase==P::Up)s.vdevUp=true;
    if(c.phase==P::Barrier && (!_connectionEcho.replyReceived || _connectionEcho.cookie!=Ath10k::channelSwitchCookie(s)))return nullptr;
    c.phase=static_cast<P>(static_cast<UInt32>(c.phase)+1);c.submitted=false;c.sinceMs=nowMs;
    publishConnection();return nullptr;
}

const char *Ath10kPCIDevice::serviceDisconnect(UInt64 nowMs)
{
    using Ath10k::DisconnectStage;auto &s=_connection;
    if(nowMs-s.sinceMs>=5000)return "disconnect-stage-timeout";
    if(s.disconnect==DisconnectStage::Drain){
        if(!_wmiScan.completed){_wmiScan.cancelRequested=true;return nullptr;}
        if(s.txOwned || s.dataPending || s.wmiOwned || _wmi.credits.outstanding)return nullptr;
        const bool started=s.vdevStarted || (s.vdevStartIssued && !s.startReply) ||
            (s.stage>=Ath10k::LinkStage::Peer && s.stage<Ath10k::LinkStage::Failed);
        s.disconnect=started?(s.vdevUp || (s.stage>=Ath10k::LinkStage::Barrier && s.stage<Ath10k::LinkStage::Failed)?DisconnectStage::Down:DisconnectStage::ClearPair):
            (s.peerCreateIssued?DisconnectStage::DeletePeer:DisconnectStage::Barrier);s.sinceMs=nowMs;
    }
    if(!s.submitted){
        // Linux DISABLE_KEY is a no-op if the peer already disappeared. Do not
        // issue a new key command to an absent peer; STOP/ECHO and RX/DMA
        // ownership still have to complete before wiping the association.
        if(!s.peerCount && (s.disconnect==DisconnectStage::ClearPair || s.disconnect==DisconnectStage::ClearGroup))
            s.disconnect=s.peerCreateIssued?DisconnectStage::DeletePeer:DisconnectStage::Stop;
        while((s.disconnect==DisconnectStage::ClearPair && !s.pairInstalled && s.securityPhase<2) ||
              (s.disconnect==DisconnectStage::ClearGroup && !s.groupInstalled[s.groupIndex] && s.securityPhase<3) ||
              (s.disconnect==DisconnectStage::DeletePeer && !s.peerCount && !s.peerCreateIssued) ||
              (s.disconnect==DisconnectStage::Stop && !s.vdevStarted && !(s.vdevStartIssued && !s.startReply) && !(s.stage>=Ath10k::LinkStage::Peer && s.stage<Ath10k::LinkStage::Failed)))
            s.disconnect=static_cast<DisconnectStage>(static_cast<UInt32>(s.disconnect)+1);
        UInt32 bytes=Ath10k::buildDisconnectWMI(static_cast<UInt8 *>(_connectionCommand.virtualAddress()),256,_htcWMIControl.endpoint,s);
        if(!bytes)return "disconnect-command-build";
        if(s.disconnect==DisconnectStage::Barrier){
            _connectionEcho={};_connectionEcho.cookie=0xa7590000u|((s.disconnectCount+1)&0xffff);_connectionEcho.submitted=true;
        }
        if(s.disconnect==DisconnectStage::DeletePeer)s.deleteReply=false;
        if(s.disconnect==DisconnectStage::Stop)s.stopReply=false;
        s.securityAck=false;s.submitted=true;
        const char *error=submitNetworkCE(3,_connectionCommand,bytes,s.wmiNext);if(error)return error;
        ++s.wmiSequence;publishConnection();return nullptr;
    }
    if(s.wmiOwned){
        Ath10k::CESnapshot snapshot{};
        if(!Ath10k::readCESnapshot(_regs,3,snapshot) || !validateBMIPipeStructure(3,snapshot) || snapshot.srcWriteIndex!=s.wmiNext)return "disconnect-ce-state";
        if(snapshot.currentSrcRead!=s.wmiNext || _wmi.credits.outstanding)return nullptr;
        if(!_connectionCommand.syncFromDevice() || !_ceSrcRings[3].syncFromDevice())return "disconnect-ce-sync";
        s.wmiOwned=false;AthSecurity::wipe(_connectionCommand.virtualAddress(),256);
    }
    // An early unmap of every AP peer also makes the key removal complete.
    // We have already drained CE3 and returned its WMI credit above; a NONE for
    // another cast/ID cannot release a live peer's pending key operation.
    if((s.disconnect==DisconnectStage::ClearPair || s.disconnect==DisconnectStage::ClearGroup) &&
       !s.securityAck && s.peerCount)return nullptr;
    if(s.disconnect==DisconnectStage::DeletePeer){
        if(s.peerCount)return nullptr;
        // Linux also waits for PEER_DELETE_RESP when SYNC_DELETE_CMDS is advertised.
        if((_wmi.serviceBitmap[92/4]&(1u<<(92%4))) && !s.deleteReply)return nullptr;
        s.peerCreateIssued=false;
    }
    if(s.disconnect==DisconnectStage::Stop && !s.stopReply)return nullptr;
    if(s.disconnect==DisconnectStage::Stop){s.vdevStartIssued=s.vdevStarted=s.vdevUp=false;}
    if(s.disconnect==DisconnectStage::Barrier){
        if(!_connectionEcho.replyReceived || _rxSetup.pendingMPDUs)return nullptr;
        Ath10k::resetDisconnected(s);_connectionEcho={};_controlOwner=nullptr;
        IOLog("Ath10kMac: DISCONNECT complete count=%u; runtime and RX ring retained, ready to reconnect\n",s.disconnectCount);
#ifndef ATH10K_NATIVE
        // Report the link as down. Without this the framework keeps the interface's
        // link state at "up" and airportd refuses the user's next join with
        // error 37 "Already associated, and network transition not permitted"
        // (measured in its WiFiManager log while the driver itself said "disabled").
        venturaPostLinkEvents(false);
#endif
    }else{
        s.disconnect=static_cast<DisconnectStage>(static_cast<UInt32>(s.disconnect)+1);
        if(!s.secure && s.disconnect==DisconnectStage::ClearPair)s.disconnect=DisconnectStage::DeletePeer;
        s.submitted=false;s.sinceMs=nowMs;
    }
    publishConnection();return nullptr;
}

// Data-only pipeline. Management/EAPOL retain the existing serial path; the
// two paths never own CE4 concurrently. Work-loop gating serializes all access.
const char *Ath10kPCIDevice::submitDataPacket(const UInt8 *data,UInt32 bytes,UInt64 nowMs)
{
    auto &s=_connection;
    if(powerIOBlocked() || !data || bytes<14 || bytes>Ath10k::kEthernetFrameMax || !Ath10k::ethernetFrameAllowed(data,bytes) || !_wmiLive.active || _dmaQuarantine ||
       !Ath10k::dataTXAdmissionOpen(s))return "data-tx-not-ready";
    const UInt8 tid=Ath10k::txPriorityTID(s,data,bytes);
    if(tid==255){++s.txAdmissionDropped;return nullptr;}
    UInt32 index=0;while(index<Ath10k::kDataTxSlots && s.dataTX[index].owned)++index;
    if(index==Ath10k::kDataTxSlots)return "data-tx-slot-accounting";
    auto &buf=_dataBuffers[index];auto &slot=s.dataTX[index];
    if((!buf.payload.allocated() && !buf.payload.allocate(2048,128,_dmaMapper,Ath10kDMABuffer::Role::Payload)) ||
       (!buf.command.allocated() && !buf.command.allocate(256,128,_dmaMapper)) ||
       (!buf.fragments.allocated() && !buf.fragments.allocate(16,16,_dmaMapper)))return "data-tx-allocation";
    if(!Ath10k::dmaSpan32(buf.payload.dmaAddress(),2048) || !Ath10k::dmaSpan32(buf.command.dmaAddress(),256) ||
       !Ath10k::dmaSpan32(buf.fragments.dmaAddress(),16))return "data-tx-dma-span";
    const UInt32 entries=Ath10k::kCEHostConfig[4].srcEntries;
    Ath10k::CESnapshot snap{};
    if(!Ath10k::readCESnapshot(_regs,4,snap) || !validateBMIPipeStructure(4,snap) ||
       snap.srcWriteIndex>=entries || snap.currentSrcRead>=entries)return "data-tx-ce-state";
    if(!s.dataPending){
        if(snap.currentSrcRead!=snap.srcWriteIndex)return "data-tx-ce-not-idle";
        s.dataWrite=s.dataRead=snap.srcWriteIndex;
    }
    if(snap.srcWriteIndex!=s.dataWrite || Ath10k::ceRingNext(s.dataWrite,entries)==snap.currentSrcRead)return "data-tx-ce-producer";
    if(!_ceSrcRings[4].syncFromDevice())return "data-tx-ring-sync";
    bcopy(data,buf.payload.virtualAddress(),bytes); // DMA length bounds the payload; no per-packet 2KiB clear.
    auto *frags=static_cast<UInt8 *>(buf.fragments.virtualAddress());bzero(frags,16);
    Ath10k::htcWriteLE32(frags,static_cast<UInt32>(buf.payload.dmaAddress()));Ath10k::htcWriteLE32(frags+4,bytes);
    do{s.txId=static_cast<UInt16>((s.txId+1)%1056);}while(Ath10k::dataTxSlotForID(s,s.txId)>=0);
    const UInt32 length=Ath10k::buildHTTPacket(static_cast<UInt8 *>(buf.command.virtualAddress()),256,data,bytes,
        static_cast<UInt32>(buf.payload.dmaAddress()),static_cast<UInt32>(buf.fragments.dmaAddress()),_htcHTTData.endpoint,s,false);
    if(!length || !buf.payload.syncForDevice() || !buf.fragments.syncForDevice() || !buf.command.syncForDevice())return "data-tx-buffer-sync";
    auto *ring=static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[4].virtualAddress());auto &desc=ring[s.dataWrite];
    bzero(&desc,sizeof(desc));desc.address=static_cast<UInt32>(buf.command.dmaAddress());desc.nbytes=static_cast<UInt16>(length);
    desc.flags=static_cast<UInt16>(_htcHTTData.endpoint<<2);
    if(!_ceSrcRings[4].syncForDevice())return "data-tx-ring-publish";
    const UInt32 next=Ath10k::ceRingNext(s.dataWrite,entries);
    slot={};slot.id=s.txId;slot.ceNext=next;slot.bytes=bytes;slot.sinceMs=nowMs;slot.owned=true;
    ++s.dataPending;s.dataWrite=next;if(s.dataPending>s.dataHigh)s.dataHigh=s.dataPending;
    // Quarantine on any ambiguous write: ownership precedes the doorbell.
    OSSynchronizeIO();UInt32 back=0xffffffffu;
    const UInt32 address=Ath10k::ceBaseAddress(4)+Ath10k::kCESrcWriteIndex;
    if(!_regs.write32(address,next) || !_regs.read32(address,back) || back!=next)return "data-tx-doorbell";
    if(tid<8)++s.txTID[tid];
    ++s.txAC[tid<8?Ath10k::txAccessCategory(tid):0];
    ++s.httSequence;return nullptr;
}
const char *Ath10kPCIDevice::serviceDataTX(UInt64 nowMs)
{
    serviceRXReorder(nowMs);
    auto &s=_connection;if(!s.dataPending){
        if(s.radio.txWakeRequested){s.radio.txWakeRequested=false;s.radio.txWakeHoldUntilMs=nowMs>~UInt64(0)-1000?~UInt64(0):nowMs+1000;}
        s.dataStallArmed=s.dataPauseArmed=false;return nullptr;
    }
    const UInt32 entries=Ath10k::kCEHostConfig[4].srcEntries,mask=entries-1;
    Ath10k::CESnapshot snap{};
    if(!Ath10k::readCESnapshot(_regs,4,snap) || !validateBMIPipeStructure(4,snap) ||
       snap.srcWriteIndex!=s.dataWrite || snap.currentSrcRead>=entries)return "data-tx-completion-ce";
    const UInt32 moved=(snap.currentSrcRead-s.dataRead)&mask,queued=(s.dataWrite-s.dataRead)&mask;
    if(moved>queued)return "data-tx-ce-overrun";
    if(moved && !_ceSrcRings[4].syncFromDevice())return "data-tx-completion-sync";
    const bool blocked=s.txPauseMask || !_wmiScan.completed;
    bool wake=false,progress=false;
    UInt32 oldest=Ath10k::kDataTxSlots;UInt64 oldestAge=0;
    auto report=[&](const char *reason,UInt32 index,UInt64 age){
        const auto &slot=s.dataTX[index];
        s.dataWatchdogReason=reason;s.dataWatchdogSlot=index;s.dataWatchdogID=slot.id;s.dataWatchdogAgeMs=age;
        IOLog("Ath10kMac: DATA TX watchdog reason=%s slot=%u id=%u age-ms=%llu bytes=%u pending=%u ce-consumed=%u htt-complete=%u ce-read=%u ce-write=%u pause=0x%x scan-active=%u stage=%u disconnect=%u channel=%u\n",
            reason,index,slot.id,age,slot.bytes,s.dataPending,slot.ceConsumed,slot.complete,
            snap.currentSrcRead,snap.srcWriteIndex,s.txPauseMask,!_wmiScan.completed,
            static_cast<UInt32>(s.stage),static_cast<UInt32>(s.disconnect),s.operatingChannel);
    };
    for(UInt32 i=0;i<Ath10k::kDataTxSlots;++i){
        auto &slot=s.dataTX[i];if(!slot.owned)continue;
        if(!slot.ceConsumed){
            const UInt32 distance=(slot.ceNext-s.dataRead)&mask;
            if(distance && distance<=moved){
                if(!_dataBuffers[i].command.syncFromDevice())return "data-tx-command-sync";
                slot.ceConsumed=true;
                progress=true;
                const UInt64 latency=nowMs-slot.sinceMs;++s.dataCECount;s.dataCETimeMs+=latency;
                if(latency>s.dataCEMaxMs)s.dataCEMaxMs=latency;
            }
        }
        if(slot.complete && !slot.httObserved){
            slot.httObserved=true;const UInt64 latency=nowMs-slot.sinceMs;
            ++s.dataHTTCount;s.dataHTTTimeMs+=latency;if(latency>s.dataHTTMaxMs)s.dataHTTMaxMs=latency;
        }
        if(slot.ceConsumed && slot.complete){
            if(slot.status){++s.txErrors;if(slot.status==2)++s.dataNoAck;else ++s.dataDiscard;}
            else {++s.txPackets;s.txBytes+=slot.bytes;}
            const UInt64 elapsed=nowMs-slot.sinceMs;s.dataLatencyMs+=elapsed;
            if(elapsed>s.dataLatencyMaxMs)s.dataLatencyMaxMs=elapsed;
            ++s.dataCompleted;slot.owned=false;--s.dataPending;wake=true;
            progress=true;
        }else{
            const UInt64 age=nowMs>=slot.sinceMs?nowMs-slot.sinceMs:0;
            if(oldest==Ath10k::kDataTxSlots || age>oldestAge){oldest=i;oldestAge=age;}
            // CE consumption is bounded by command age even during pauses.
            // A firmware-retained payload may legitimately predate teardown:
            // give Drain its own stage deadline instead of failing immediately
            // when an old payload's controlled port closes.
            const bool ceTimeout=!slot.ceConsumed && age>=5000;
            const bool drainTimeout=s.disconnect!=Ath10k::DisconnectStage::None &&
                nowMs>=s.sinceMs && nowMs-s.sinceMs>=5000;
            if(ceTimeout || drainTimeout){
                ++s.dataTimeouts;report(ceTimeout?"ce-timeout":"drain-timeout",i,age);
                return "data-tx-completion-timeout";
            }
            if(age>=5000 && !slot.waitReported){
                slot.waitReported=true;++s.dataDelayed;report("firmware-retained",i,age);
            }
        }
    }
    s.dataRead=snap.currentSrcRead;
    if(s.dataPending && oldest==Ath10k::kDataTxSlots)return "data-tx-slot-accounting";
    if(!s.dataPending){
        if(s.radio.txWakeRequested){s.radio.txWakeRequested=false;s.radio.txWakeHoldUntilMs=nowMs>~UInt64(0)-1000?~UInt64(0):nowMs+1000;}
        s.dataStallArmed=s.dataPauseArmed=false;
    }
    else if(blocked){
        // STOP/Wake and scan can legitimately defer HTT replies. Do not charge
        // that interval to a full-queue stall; a stuck pause remains bounded.
        s.dataStallArmed=false;
        if(!s.dataPauseArmed){s.dataPauseArmed=true;s.dataPauseSinceMs=nowMs;}
        const UInt64 pauseLimit=_wmiScan.completed?30000:Ath10k::kWMIScanHostMs;
        if(nowMs>=s.dataPauseSinceMs && nowMs-s.dataPauseSinceMs>=pauseLimit){
            ++s.dataTimeouts;report("pause-timeout",oldest,oldestAge);
            return "data-tx-pause-timeout";
        }
    }else{
        s.dataPauseArmed=false;
        // Match Linux's pending-ID ownership: keep each payload until its real
        // HTT reply, and detect loss of queue progress rather than frame age.
        if(s.dataPending<Ath10k::kDataTxSlots)s.dataStallArmed=false;
        else{
            if(progress || !s.dataStallArmed){s.dataStallArmed=true;s.dataStallSinceMs=nowMs;}
            if(s.radio.appliedPS && !s.radio.txWakeRequested && s.stage==Ath10k::LinkStage::Connected &&
               s.portAuthorized && s.disconnect==Ath10k::DisconnectStage::None &&
               nowMs>=s.dataStallSinceMs && nowMs-s.dataStallSinceMs>=1000){
                s.radio.txWakeRequested=true;++s.radio.txWakeRequests;
            }
            if(nowMs>=s.dataStallSinceMs && nowMs-s.dataStallSinceMs>=5000){
                ++s.dataTimeouts;report("queue-stalled",oldest,oldestAge);
                return "data-tx-completion-timeout";
            }
        }
    }
    if(wake && _dataTXStalled && Ath10k::dataTXAdmissionOpen(s) &&
       s.dataPending<=Ath10k::kDataTxSlots*3/4 && getOutputQueue()){
        _dataTXStalled=false;++_perf.txWakeups;getOutputQueue()->service(1);
    }
    return nullptr;
}
