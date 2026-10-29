// Sequoia WCL adapter for the Ath10k radio core. Experimental, x86_64 only.
// ABI definitions and message layouts are pinned in vendor/itlwm/SOURCE.json.
#include "../Ath10kPCIDevice.hpp"
#include "../../shared/Ath10kSupplicant.hpp"
#include "Ath10kNativeWire.hpp"
#include "Ath10kNativeCapture.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/network/IONetworkMedium.h>
#include <sys/kpi_mbuf.h>
#include <sys/random.h>
extern "C" {
#include <corecrypto/cchmac.h>
#include <corecrypto/ccsha1.h>
#include <corecrypto/ccaes.h>
#include <corecrypto/ccmode.h>
}

OSDefineMetaClassAndStructors(Ath10kNativeInterface,IO80211InfraProtocol)

// The BSD data path remains mbuf-based, as in the reference WCL adapter.
// Subtype 3 and prepareBSDInterface attach it to the real Wi-Fi protocol.
class Ath10kBSDInterface : public IOEthernetInterface {
    OSDeclareDefaultStructors(Ath10kBSDInterface)
    Ath10kNativeInterface *wifi=nullptr;
    bool attached=false;
public:
    bool initWithController(Ath10kPCIDevice *owner,Ath10kNativeInterface *protocol) {
        wifi=protocol;return IOEthernetInterface::init(owner);
    }
    static IOReturn prepare(OSObject *,void *arg,void *,void *,void *) {
        auto self=static_cast<Ath10kBSDInterface *>(arg);
        auto owner=self->wifi->controller;
        auto queue=owner->getWorkQueue();
        if(!queue || !queue->inGate() || queue->onThread())return kIOReturnNotPermitted;
        return self->wifi->prepareBSDInterface(self->getIfnet(),0);
    }
    IOReturn attachToDataLinkLayer(IOOptionBits options,void *parameter) override {
        if(!setInterfaceSubType(3))return kIOReturnNotReady;
        auto result=IOEthernetInterface::attachToDataLinkLayer(options,parameter);
        if(result!=kIOReturnSuccess)return result;
        char name[IFNAMSIZ];
        snprintf(name,sizeof(name),"%s%u",ifnet_name(getIfnet()),ifnet_unit(getIfnet()));
        wifi->setProperty("IOInterfaceName",name);
        wifi->setProperty(kIOInterfaceNamePrefix,ifnet_name(getIfnet()));
        wifi->setProperty(kIOInterfaceUnit,UInt64(ifnet_unit(getIfnet())),32);
        UInt8 builtin=1;wifi->setProperty("built-in",&builtin,sizeof(builtin));
        result=wifi->controller->_nativeGate->runAction(prepare,this);
        if(result!=kIOReturnSuccess){
            IOEthernetInterface::detachFromDataLinkLayer(options,parameter);return result;
        }
        attached=true;wifi->registerService();
        // Firmware was initialized before the WCL interface existed. Publish
        // its readiness only after the BSD/Wi-Fi binding is actually usable.
        ownerReady();return result;
    }
    void detachFromDataLinkLayer(IOOptionBits options,void *parameter) override {
        IOEthernetInterface::detachFromDataLinkLayer(options,parameter);attached=false;
    }
    IOService *getProvider() const override {
        return attached?static_cast<IOService *>(wifi):IOEthernetInterface::getProvider();
    }
    void ownerReady(){wifi->controller->nativeRequest(AthNativeOp::NativeReady,nullptr);}
};
OSDefineMetaClassAndStructors(Ath10kBSDInterface,IOEthernetInterface)

struct Ath10kKernelCrypto {
    static bool hmac(const UInt8 *key,size_t keyLength,const UInt8 *data,size_t length,UInt8 *out) {
        const auto di=ccsha1_di();if(!di)return false;
        cchmac(di,keyLength,key,length,data,out);return true;
    }
    static bool decrypt(const UInt8 *key,const UInt8 *data,UInt8 *out) {
        auto mode=ccaes_ecb_decrypt_mode();if(!mode || mode->size>1024)return false;
        // Fixed upper bound: no variable-sized kernel stack allocation.
        alignas(16) UInt8 context[1024]{};
        auto ctx=reinterpret_cast<ccecb_ctx *>(context);
        bool ok=mode->init(mode,ctx,16,key)==0 && mode->ecb(ctx,1,data,out)==0;
        AthSecurity::wipe(context,sizeof(context));return ok;
    }
    static bool random(UInt8 *out,size_t length) {read_random(out,length);return true;}
};
class Ath10kPCIDevice::NativeSecurity {
public:
    AthSecurity::Supplicant<Ath10kKernelCrypto> supplicant;
    bool ready=false,replyPending=false,keysPending=false,authorizePending=false;
    unsigned keyOperation=0;
    UInt8 reply[160]{},keys[56]{};
    UInt32 replyLength=0;
    UInt64 pairDeadline=0;
    void reset(){supplicant.reset();AthSecurity::wipe(reply,sizeof(reply));AthSecurity::wipe(keys,sizeof(keys));
        ready=replyPending=keysPending=authorizePending=false;replyLength=keyOperation=0;pairDeadline=0;}
    ~NativeSecurity(){reset();}
};

IO80211WorkQueue *Ath10kPCIDevice::getWorkQueue() const {
    return _nativeWorkQueue;
}
bool Ath10kPCIDevice::getLogPipes(CCPipe **a,CCPipe **b,CCPipe **c) {
    if(!a || !b || !c)return false;*a=_nativePipes[0];*b=_nativePipes[1];*c=_nativePipes[2];
    return *a && *b && *c;
}
void Ath10kPCIDevice::nativeStartupPhase(const char *phase) {
    setProperty("Ath10kNativeStartupPhase",phase);
    if(auto provider=getProvider())provider->setProperty("Ath10kNativeStartupPhase",phase);
    IOLog("Ath10kMac: native startup phase=%s\n",phase);
}
bool Ath10kPCIDevice::prepareNativeLogs() {
    if(!getWorkLoop())return false;
    static const char *names[]={"DriverLogs","DatapathEvents","StateSnapshots"};
    static const char *createPhases[]={"log-pipe-create","datapath-pipe-create","snapshot-pipe-create"};
    static const char *startPhases[]={"log-pipe-start","datapath-pipe-start","snapshot-pipe-start"};
    for(unsigned i=0;i<3;++i){
        CCPipeOptions options{};if(!nativeCaptureOptions(i,options))return false;
        strlcpy(options.file_name,names[i],sizeof(options.file_name));
        if(i!=1)strlcpy(options.name,i==0?"wlan0":"0",sizeof(options.name));
        strlcpy(options.directory_name,"WiFi",sizeof(options.directory_name));
        nativeStartupPhase(createPhases[i]);
        _nativePipes[i]=CCPipe::withOwnerNameCapacity(this,"br.com.jonasw8.driver.ath10kmac",names[i],&options);
        if(!_nativePipes[i])return false;
        nativeStartupPhase(startPhases[i]);
        if(!(_nativePipesStarted[i]=_nativePipes[i]->CCPipe::startPipe()))return false;
        if(i==0){
            // The owner logger must exist before creating dependent pipes.
            nativeStartupPhase("logger-create");
            CCStreamOptions loggerOptions{};
            CCStream *logger=CCStream::withPipeAndName(_nativePipes[0],"Ath10kMac",&loggerOptions);
            _nativeLogger=OSDynamicCast(CCLogStream,logger);
            if(!_nativeLogger){OSSafeReleaseNULL(logger);return false;}
        }
    }
    nativeStartupPhase("fault-stream-create");
    CCStreamOptions faultOptions{};faultOptions.stream_type=1;faultOptions.console_level=~0ULL;
    _nativeFaultStream=CCStream::withPipeAndName(_nativePipes[2],"FaultReporter",&faultOptions);
    auto data=OSDynamicCast(CCDataStream,_nativeFaultStream);if(!data)return false;
    nativeStartupPhase("core-fault-reporter-create");
    _nativeCoreFault=CCFaultReporter::withStreamWorkloop(data,getWorkLoop());if(!_nativeCoreFault)return false;
    nativeStartupPhase("wifi-fault-reporter-create");
    _nativeFault=IO80211FaultReporter::allocWithParams(_nativeCoreFault);
    return OSDynamicCast(CommonFaultReporter,_nativeFault)!=nullptr;
}
void Ath10kPCIDevice::releaseNativeLogs() {
    OSSafeReleaseNULL(_nativeFault);OSSafeReleaseNULL(_nativeCoreFault);
    OSSafeReleaseNULL(_nativeFaultStream);OSSafeReleaseNULL(_nativeLogger);
    for(unsigned i=0;i<3;++i){
        if(_nativePipesStarted[i] && _nativePipes[i])_nativePipes[i]->CCPipe::stopPipe();
        _nativePipesStarted[i]=false;OSSafeReleaseNULL(_nativePipes[i]);
    }
}
static IOReturn nativeVersion(apple80211_version_data *data,const char *value) {
    if(!data)return kIOReturnBadArgument;bzero(data,sizeof(*data));data->version=APPLE80211_VERSION;
    data->string_len=strlcpy(data->string,value,sizeof(data->string));return kIOReturnSuccess;
}
IOReturn Ath10kPCIDevice::getDRIVER_VERSION(IO80211SkywalkInterface *,apple80211_version_data *data) {
    return nativeVersion(data,"Ath10kMac Sequoia WCL experimental");
}
IOReturn Ath10kPCIDevice::getHARDWARE_VERSION(IO80211SkywalkInterface *,apple80211_version_data *data) {
    return nativeVersion(data,"Qualcomm Atheros QCA9377 Lunnen 1a3b:2b51");
}
IOReturn Ath10kPCIDevice::getCARD_CAPABILITIES(IO80211SkywalkInterface *,apple80211_capability_data *data) {
    if(!data)return kIOReturnBadArgument;bzero(data,sizeof(*data));data->version=APPLE80211_VERSION;
    data->capabilities[0]=(1<<APPLE80211_CAP_AES_CCM)|(1<<APPLE80211_CAP_TKIP);
    data->capabilities[1]=(1<<(APPLE80211_CAP_WPA2-8))|(1<<(APPLE80211_CAP_TKIPMIC-8));
    data->capabilities[2]=(1<<(APPLE80211_CAP_WME-16));data->capabilities[5]=0x40;
    return kIOReturnSuccess;
}
IOReturn Ath10kPCIDevice::getPOWER(IO80211SkywalkInterface *,apple80211_power_data *data){return nativeRequest(AthNativeOp::PowerGet,data);}
IOReturn Ath10kPCIDevice::setPOWER(IO80211SkywalkInterface *,apple80211_power_data *data){return nativeRequest(AthNativeOp::PowerSet,data);}
IOReturn Ath10kPCIDevice::getCOUNTRY_CODE(IO80211SkywalkInterface *,apple80211_country_code_data *data){return nativeRequest(AthNativeOp::CountryGet,data);}
IOReturn Ath10kPCIDevice::setCOUNTRY_CODE(IO80211SkywalkInterface *,apple80211_country_code_data *data){return nativeRequest(AthNativeOp::CountrySet,data);}

bool Ath10kNativeInterface::initWithController(Ath10kPCIDevice *owner) {
    controller=owner;IOEthernetAddress address{};
    return IO80211InfraInterface::init() && owner->getHardwareAddress(&address)==kIOReturnSuccess &&
        IO80211SkywalkInterface::init(owner,reinterpret_cast<ether_addr *>(&address));
}
IOReturn Ath10kNativeInterface::perform(AthNativeOp op,void *data){return controller?controller->nativeRequest(op,data):kIOReturnNotAttached;}
IOReturn Ath10kNativeInterface::setMacAddress(ether_addr &address) {
    IOEthernetAddress current{};
    if(!controller || controller->getHardwareAddress(&current)!=kIOReturnSuccess)return kIOReturnNotReady;
    // A firmware restart with a changed vdev address is not implemented yet.
    // Never claim a new address while transmitting with the permanent address.
    return bcmp(&current,&address,6)==0?kIOReturnSuccess:kIOReturnUnsupported;
}
IONetworkInterface *Ath10kPCIDevice::createInterface() {
    auto interface=new Ath10kBSDInterface;
    if(interface && interface->initWithController(this,_nativeInterface))return interface;
    OSSafeReleaseNULL(interface);return nullptr;
}
bool Ath10kPCIDevice::prepareNativeInterface() {
    _nativeStopping=false;
    // Strict profile/OS guard; unrelated adapters must not enter the WCL path.
    const bool lunnen9377=_vendorId==0x168c && _deviceId==0x42 &&
        _subsystemVendorId==0x1a3b && _subsystemId==0x2b51;
    const bool liteon6174=_vendorId==0x168c && _deviceId==0x3e &&
        _subsystemVendorId==0x11ad && _subsystemId==0x0807;
    if(!lunnen9377 && !liteon6174)return false;
    _nativeGate=IOCommandGate::commandGate(this);
    if(!_nativeGate || getWorkLoop()->addEventSource(_nativeGate)!=kIOReturnSuccess)return false;
    _nativeSecurity=new NativeSecurity;if(!_nativeSecurity)return false;
    _nativeInterface=new Ath10kNativeInterface;
    if(!_nativeInterface || !_nativeInterface->initWithController(this))return false;
    _nativeInterface->setInterfaceRole(1);_nativeInterface->setInterfaceId(1);
    if(!_nativeInterface->attach(this))return false;
    _nativeAttached=true;
    if(!IO80211Controller::attachInterface(_nativeInterface,this))return false;
    _nativeControllerAttached=true;
    if(!IONetworkController::attachInterface(&_networkInterface,false))return false;
    IOSkywalkEthernetInterface::RegistrationInfo registration{};
    if(!_nativeInterface->initRegistrationInfo(&registration,1,sizeof(registration)))return false;
    if(!_nativeInterface->mExpansionData || !_nativeInterface->mExpansionData2)return false;
    _nativeInterface->mExpansionData->fRegistrationInfo=static_cast<IOSkywalkNetworkInterface::RegistrationInfo *>(IOMalloc(sizeof(registration)));
    _nativeInterface->mExpansionData2->fRegistrationInfo=static_cast<IOSkywalkEthernetInterface::RegistrationInfo *>(IOMalloc(sizeof(registration)));
    if(!_nativeInterface->mExpansionData->fRegistrationInfo || !_nativeInterface->mExpansionData2->fRegistrationInfo)return false;
    bcopy(&registration,_nativeInterface->mExpansionData->fRegistrationInfo,sizeof(registration));
    bcopy(&registration,_nativeInterface->mExpansionData2->fRegistrationInfo,sizeof(registration));
    _nativeInterface->deferBSDAttach(true);
    if(!_nativeInterface->start(this))return false;
    _nativeStarted=true;_networkEnabled=true;
    _networkInterface->registerService();
    setProperty("Ath10kNativeABI","Sequoia-15.2-WCL-experimental");
    return true;
}
IOReturn Ath10kPCIDevice::nativeStopAction(OSObject *owner,void *,void *,void *,void *) {
    auto self=OSDynamicCast(Ath10kPCIDevice,owner);
    if(!self)return kIOReturnBadArgument;
    self->_nativeStopping=true;
    return kIOReturnSuccess;
}
void Ath10kPCIDevice::stopNativeRequests() {
    if(_nativeGate && _nativeGate->getWorkLoop())_nativeGate->runAction(nativeStopAction);
    else _nativeStopping=true;
}
void Ath10kPCIDevice::releaseNativeInterface() {
    // stop() has already cancelled the runtime worker under its workloop.
    stopNativeRequests();
    if(_networkInterface){
        IONetworkController::detachInterface(_networkInterface,true);
        OSSafeReleaseNULL(_networkInterface);
    }
    if(_nativeInterface){
        if(_nativeStarted)_nativeInterface->stop(this);
        if(_nativeControllerAttached)IO80211Controller::detachInterface(_nativeInterface,true);
        if(_nativeAttached)_nativeInterface->detach(this);
        OSSafeReleaseNULL(_nativeInterface);
    }
    _nativeStarted=_nativeAttached=_nativeControllerAttached=_nativeReadyPublished=false;
    if(_nativeSecurity){delete _nativeSecurity;_nativeSecurity=nullptr;}
    AthSecurity::wipe(&_nativeJoin,sizeof(_nativeJoin));
    if(_nativeGate){getWorkLoop()->removeEventSource(_nativeGate);OSSafeReleaseNULL(_nativeGate);}
}
IOReturn Ath10kPCIDevice::nativeRequest(AthNativeOp op,void *data) {
    if(!_nativeGate || _nativeStopping || !_wmiLive.active)return kIOReturnNotReady;
    return _nativeGate->runAction(nativeAction,reinterpret_cast<void *>(uintptr_t(op)),data);
}
IOReturn Ath10kPCIDevice::nativeAction(OSObject *target,void *op,void *data,void *,void *) {
    auto self=OSDynamicCast(Ath10kPCIDevice,target);
    return self?self->nativeRequestGated(static_cast<AthNativeOp>(uintptr_t(op)),data):kIOReturnBadArgument;
}
static UInt32 channelFlags(UInt32 channel){return (channel<=14?APPLE80211_C_FLAG_2GHZ:APPLE80211_C_FLAG_5GHZ)|APPLE80211_C_FLAG_20MHZ;}
IOReturn Ath10kPCIDevice::nativeRequestGated(AthNativeOp op,void *data) {
    if(_nativeStopping || !_wmiLive.active)return kIOReturnNotReady;
    auto &s=_connection;using N=AthNativeOp;
    if(!data && op!=N::setCLEAR_PMKSA_CACHE && op!=N::setWCL_SCAN_ABORT &&
       op!=N::setWCL_JOIN_ABORT && op!=N::setWCL_LEAVE_NETWORK && op!=N::setWCL_LINK_UP_DONE &&
       op!=N::NativeReady)return kIOReturnBadArgument;
    switch(op){
    case N::NativeReady:{
        if(!_nativeStarted || !_nativeInterface || !_networkInterface)return kIOReturnNotReady;
        if(_nativeReadyPublished)return kIOReturnSuccess;
        UInt32 reset=1;postMessage(_nativeInterface,AirportWCL::ResetInterface,&reset,sizeof(reset),true);
        AirportWCL::DriverAvailability ready{};ready.version=3;ready.available=1;ready.reason=0xe0821803;
        postMessage(_nativeInterface,AirportWCL::DriverAvailable,&ready,sizeof(ready),true);
        _nativeReadyPublished=true;return kIOReturnSuccess;}
    case N::PowerGet:{auto d=static_cast<apple80211_power_data *>(data);bzero(d,sizeof(*d));d->version=1;d->num_radios=1;d->power_state[0]=_nativeRadioOn?1:0;return kIOReturnSuccess;}
    case N::PowerSet:{auto d=static_cast<apple80211_power_data *>(data);if(d->num_radios!=1 || d->power_state[0]>1)return kIOReturnBadArgument;
        _nativeRadioOn=d->power_state[0];if(!_nativeRadioOn){_wmiScan.cancelRequested=true;s.cancelRequested=true;s.portAuthorized=false;networkLink(false);}
        return kIOReturnSuccess;}
    case N::CountryGet:{auto d=static_cast<apple80211_country_code_data *>(data);bzero(d,sizeof(*d));d->version=1;
        if(_regulatory.country)bcopy(_regulatory.country->alpha2,d->cc,2);return kIOReturnSuccess;}
    case N::CountrySet:{auto d=static_cast<apple80211_country_code_data *>(data);
        return _regulatory.country && !bcmp(d->cc,_regulatory.country->alpha2,2)?kIOReturnSuccess:kIOReturnUnsupported;}
    case N::getHW_ADDR:{AirportWCL::HardwareAddress address{};
        if(!AirportWCL::copyHardwareAddress(_wmiInit.mac,address))return kIOReturnNotReady;bcopy(&address,data,sizeof(address));return kIOReturnSuccess;}
    case N::getCHANNEL:{auto d=static_cast<apple80211_channel_data *>(data);bzero(d,sizeof(*d));d->version=1;
        d->channel.version=1;d->channel.channel=s.operatingChannel;d->channel.flags=channelFlags(s.operatingChannel);return s.operatingChannel?kIOReturnSuccess:kIOReturnNotReady;}
    case N::getOP_MODE:{auto d=static_cast<apple80211_opmode_data *>(data);bzero(d,sizeof(*d));d->version=1;d->op_mode=APPLE80211_M_STA;return kIOReturnSuccess;}
    case N::getNSS:{auto d=static_cast<apple80211_nss_data *>(data);bzero(d,sizeof(*d));d->version=1;d->nss=1;return kIOReturnSuccess;}
    case N::getRSSI:{if(s.signalDbm==INT32_MIN)return kIOReturnNotReady;auto d=static_cast<apple80211_rssi_data *>(data);bzero(d,sizeof(*d));d->version=1;d->num_radios=1;d->rssi_unit=APPLE80211_UNIT_DBM;d->rssi[0]=d->aggregate_rssi=s.signalDbm;return kIOReturnSuccess;}
    case N::getTXPOWER:{auto d=static_cast<apple80211_txpower_data *>(data);bzero(d,sizeof(*d));d->version=1;d->txpower_unit=APPLE80211_UNIT_DBM;d->txpower=s.txPowerHalfDbm/2;return kIOReturnSuccess;}
    case N::getRATE:{if(!s.fwStats.rateValid)return kIOReturnNotReady;auto d=static_cast<apple80211_rate_data *>(data);bzero(d,sizeof(*d));d->version=1;d->num_radios=1;d->rate[0]=s.fwStats.txKbps/500;return kIOReturnSuccess;}
    case N::getPOWERSAVE:{auto d=static_cast<apple80211_powersave_data *>(data);bzero(d,sizeof(*d));d->version=1;d->powersave_level=s.radio.appliedPS?1:0;return kIOReturnSuccess;}
    case N::setPOWERSAVE:{auto d=static_cast<apple80211_powersave_data *>(data);if(d->powersave_level>1)return kIOReturnUnsupported;s.radio.requestedPS=d->powersave_level;return kIOReturnSuccess;}
    case N::getSUPPORTED_CHANNELS:case N::getHW_SUPPORTED_CHANNELS:{auto d=static_cast<apple80211_sup_channel_data *>(data);bzero(d,sizeof(*d));d->version=1;
        for(unsigned ch=1;ch<=196 && d->num_channels<APPLE80211_MAX_CHANNELS;++ch){
            if(ch>14 && (ch<36 || ch%4))continue;
            if(!Ath10k::regulatoryPower(_regulatory,ch,ch,20,false))continue;
            auto &c=d->supported_channels[d->num_channels++];c.version=1;c.channel=ch;c.flags=channelFlags(ch);
        }return kIOReturnSuccess;}
    case N::getCHANNELS_INFO:case N::getCOUNTRY_CHANNELS_INFO:{auto d=static_cast<AirportWCL::NativeChannelInfo *>(data);bzero(d,sizeof(*d));
        if(_regulatory.country)bcopy(_regulatory.country->alpha2,d->country,2);
        for(unsigned ch=1;ch<=196;++ch){if(ch>14 && (ch<36 || ch%4))continue;
            if(!Ath10k::regulatoryPower(_regulatory,ch,ch,20,false))continue;
            AirportWCL::appendChannel(*d,ch,ch>14,ch>=52 && ch<=144,true,false,false);
        }return kIOReturnSuccess;}
    case N::getWCL_TRAFFIC_COUNTERS:{AirportWCL::TrafficCounters counters{};counters.infraTx=s.txPackets;counters.infraRx=s.rxPackets;counters.timestampNanoseconds=Ath10k::uptimeNS();bcopy(&counters,data,sizeof(counters));return kIOReturnSuccess;}
    case N::getWCL_BSS_INFO:{if(!_nativeBSSBytes)return kIOReturnNotReady;bcopy(_nativeBSSMessage,data,_nativeBSSBytes);return kIOReturnSuccess;}
    case N::getBEACON_INFO:{
        if(s.stage!=Ath10k::LinkStage::Connected || s.cancelRequested ||
           s.disconnect!=Ath10k::DisconnectStage::None)return kIOReturnNotReady;
        return nativeBeaconInfo(s.beaconFrame,s.beaconFrameBytes,data,0x808)?kIOReturnSuccess:kIOReturnNotReady;}
    case N::getWCL_EXTENDED_BSS_INFO:{
        if(s.stage!=Ath10k::LinkStage::Connected || s.cancelRequested ||
           s.disconnect!=Ath10k::DisconnectStage::None)return kIOReturnNotReady;
        NativeExtendedBSSInput input{};input.rates=s.ap.rates;input.rateCount=s.ap.rateCount;
        input.htRates=s.htRates;input.htRateCount=s.htEnabled?s.htRateCount:0;
        input.vhtMap=s.vhtEnabled?s.vhtTxMap:0xffff;
        if(s.secure){input.rsn=s.ap.rsn;input.rsnLength=s.ap.rsnLength;}
        return nativeExtendedBSS(input,data,0x1e0)?kIOReturnSuccess:kIOReturnBadArgument;}
    case N::setWCL_SCAN_REQ:{AirportWCL::ScanRequest scan{};
        // The native ABI owns a fixed request containing at most 400 channels.
        if(!AirportWCL::decodeScan(data,0x1318,scan))return kIOReturnBadArgument;
        if(!_nativeRadioOn || powerIOBlocked() || _wmiEvents.rfKill)return kIOReturnNotReady;
        if(_nativeScanPending || _nativeJoinPending || !_wmiScan.completed || s.wmiOwned || s.txOwned || _wmi.credits.outstanding ||
           (s.stage!=Ath10k::LinkStage::Disabled && (!s.portAuthorized || s.stage!=Ath10k::LinkStage::Connected)))return kIOReturnBusy;
        // The radio core's bounded passive scan is used; report results only
        // for requested channels/SSID once collection has completed.
        UInt32 generation=_wmiScan.generation+1;Ath10k::resetInPlace(_wmiScan);_wmiScan.generation=generation;
        _wmiScan.id=0xa000|((0x36+generation)&0xfff);Ath10k::resetInPlace(_wmiBSS);_nativeScan=scan;_nativeScanPending=true;
        return kIOReturnSuccess;}
    case N::setWCL_SCAN_ABORT:{if(_nativeScanPending)_wmiScan.cancelRequested=true;return kIOReturnSuccess;}
    case N::setWCL_ASSOCIATE:{AirportWCL::JoinRequest join{};
        if(!AirportWCL::decodeJoin(data,0x2cc,join))return kIOReturnBadArgument;
        bool personal=join.upperAuth==APPLE80211_AUTHTYPE_WPA2_PSK;
        bool open=join.upperAuth==0 && join.keyLength==0 && join.lowerAuth==1;
        if((!personal && !open) || (personal && (join.lowerAuth!=1 || join.keyLength!=32 || (join.keyCipher!=6 && join.keyCipher!=7)))){
            AthSecurity::wipe(&join,sizeof(join));return kIOReturnUnsupported;}
        if(!_nativeRadioOn || powerIOBlocked() || _wmiEvents.rfKill || !_networkEnabled){AthSecurity::wipe(&join,sizeof(join));return kIOReturnNotReady;}
        if(_nativeJoinPending || _nativeScanPending || !_wmiScan.completed || s.stage!=Ath10k::LinkStage::Disabled || s.disconnect!=Ath10k::DisconnectStage::None){AthSecurity::wipe(&join,sizeof(join));return kIOReturnBusy;}
        s.regulatory=_regulatory;s.hiddenSSID=true;s.secure=personal;s.ssidLength=join.ssidLength;
        bcopy(join.ssid,s.ssid,join.ssidLength);bcopy(join.bssid,s.bssid,6);bcopy(_wmiInit.mac,s.local,6);
        s.firmwareHT=_wmi.htCapability;s.firmwareVHT=_wmi.vhtCapability;s.rfChains=1;
        const char *error=Ath10k::selectOpenBSS(s,_wmiBSS);
        if(error){AthSecurity::wipe(&join,sizeof(join));Ath10k::resetDisconnected(s);return kIOReturnUnsupported;}
        if(personal){UInt8 nonce[32]{};Ath10kKernelCrypto::random(nonce,sizeof(nonce));
            bool ok=_nativeSecurity->supplicant.begin(join.key,nonce,s.local,s.bssid,s.ap.rsn,s.ap.rsnLength);
            AthSecurity::wipe(nonce,sizeof(nonce));if(!ok){AthSecurity::wipe(&join,sizeof(join));Ath10k::resetDisconnected(s);return kIOReturnUnsupported;}
            _nativeSecurity->ready=true;}
        _nativeJoin=join;AthSecurity::wipe(_nativeJoin.key,sizeof(_nativeJoin.key));AthSecurity::wipe(&join,sizeof(join));
        _nativeJoinPending=true;_nativeAssocSent=_nativeBeaconSent=false;_nativeJoinDeadline=Ath10k::uptimeNS()/1000000u+30000;
        s.error="none";s.stage=Ath10k::LinkStage::Scan;return kIOReturnSuccess;}
    case N::setWCL_JOIN_ABORT:case N::setWCL_LEAVE_NETWORK:{
        s.cancelRequested=true;s.portAuthorized=false;if(!_wmiScan.completed)_wmiScan.cancelRequested=true;networkLink(false);
        if(_nativeSecurity)_nativeSecurity->reset();
        if(op==N::setWCL_JOIN_ABORT){
            _nativeJoinPending=false;AthSecurity::wipe(&_nativeJoin,sizeof(_nativeJoin));
            postMessage(_nativeInterface,AirportWCL::JoinAbortComplete,nullptr,0,true);
        }
        return kIOReturnSuccess;}
    case N::setCLEAR_PMKSA_CACHE:return kIOReturnSuccess; // No PMKSA cache is retained.
    case N::setWCL_LINK_UP_DONE:return s.portAuthorized?kIOReturnSuccess:kIOReturnNotReady;
    case N::setWCL_LINK_STATE_UPDATE:{
        auto bytes=static_cast<const UInt8 *>(data);
        if(bytes[6]){
            if(!Ath10k::sameMAC(bytes,s.bssid))return kIOReturnBadArgument;
            if(bytes[8])_nativeInterface->setCurrentApAddress(reinterpret_cast<ether_addr *>(data));
            if(bytes[7])_nativeInterface->setLinkState(kIO80211NetworkLinkUp,0,false,0);
        }else{
            _nativeInterface->setCurrentApAddress(nullptr);
            UInt32 debounce=AirportWCL::read32(bytes+12);
            _nativeInterface->setLinkState(kIO80211NetworkLinkDown,0,debounce!=0,debounce);
        }return kIOReturnSuccess;}

    default:return kIOReturnUnsupported;
    }
}

void Ath10kPCIDevice::serviceNative(UInt64 now) {
    if(!_nativeInterface || _nativeStopping || !_nativeStarted)return;
    auto &s=_connection;auto sec=_nativeSecurity;
    auto post=[&](UInt32 id,void *payload,size_t bytes){postMessage(_nativeInterface,id,payload,bytes,true);};
    if(_nativeScanPending && _wmiScan.completed){
        if(!_wmiScan.failed && !_wmiScan.cancelRequested){
            for(unsigned i=0;i<_wmiBSS.count;++i){
                const auto &b=_wmiBSS.entries[i];
                if(!nativeScanMatches(_nativeScan,b))continue;
                size_t length=nativeBSSMessage(_wmiBSS.entries[i],_nativeBSSMessage,sizeof(_nativeBSSMessage));
                if(length)post(AirportWCL::ScanResult,_nativeBSSMessage,length);
            }
        }
        UInt32 status=_wmiScan.cancelRequested?kIOReturnAborted:(_wmiScan.failed?kIOReturnError:kIOReturnSuccess);
        _nativeScanPending=false;post(AirportWCL::ScanDone,&status,sizeof(status));
        _nativeBSSBytes=0;
    }
    const bool associated=s.stage==Ath10k::LinkStage::Connected && s.disconnect==Ath10k::DisconnectStage::None && !s.cancelRequested;
    if(_nativeJoinPending && associated && !_nativeAssocSent){
        AirportWCL::AssociationEvent event{};bcopy(s.bssid,event.bssid,6);event.status=s.associationStatus;
        event.driverStatus=event.status?kIOReturnError:kIOReturnSuccess;
        post(AirportWCL::AssocComplete,&event,sizeof(event));_nativeAssocSent=event.status==0;
    }
    if(_nativeJoinPending && associated && _nativeAssocSent && s.beaconFrameBytes && !_nativeBeaconSent){
        _nativeBSSBytes=nativeBSSMessage(s.ap,_nativeBSSMessage,sizeof(_nativeBSSMessage));
        if(_nativeBSSBytes){
            post(AirportWCL::ScanResult,_nativeBSSMessage,_nativeBSSBytes);
            AirportWCL::LinkStatusEvent link{};bcopy(s.bssid,link.bssid,6);link.up=1;link.reason=255;
            AirportWCL::FirstBeaconEvent beacon{};bcopy(s.bssid,beacon.peer.bssid,6);
            post(AirportWCL::LinkStatus,&link,sizeof(link));post(AirportWCL::FirstBeacon,&beacon,sizeof(beacon));
            _nativeBeaconSent=true;
        }
    }
    if(sec && sec->ready && associated && s.secure){
        if(sec->authorizePending && !s.txOwned && s.txComplete){
            if(s.txStatus || s.eapolTxFailed){s.error="native-eapol-tx-failed";s.cancelRequested=true;}
            else if(s.securityPhase==3)s.authorizeRequested=true;
            sec->authorizePending=false;
        }
        if(sec->keysPending && !s.wmiOwned && !s.submitted && !s.txOwned && !s.dataPending &&
           !s.stats.step && !s.maintenance.step && !s.radio.step && _wmiScan.completed && !_wmi.credits.outstanding){
            const bool initial=sec->keyOperation==AthSecurity::Supplicant<Ath10kKernelCrypto>::Keys;
            const bool pair=sec->keyOperation==AthSecurity::Supplicant<Ath10kKernelCrypto>::PairKeys;
            if((initial && !s.securityPhase) || (!initial && s.securityPhase==5 && s.portAuthorized)){
                const unsigned keyBytes=s.groupCipher==AthSecurity::cipherTKIP?32:16;
                bool sameGroup=s.groupInstalled[sec->keys[49]] && AthSecurity::equal(sec->keys+16,s.installedGroups[sec->keys[49]],keyBytes);
                bcopy(sec->keys,s.temporalKey,16);bcopy(sec->keys+16,s.groupKey,32);
                s.groupIndex=sec->keys[49];bcopy(sec->keys+50,s.groupRSC,6);
                if(initial){UInt64 pn=0;for(unsigned i=0;i<6;++i)pn|=UInt64(s.groupRSC[i])<<(8*i);for(auto &v:s.groupPN)v=pn;}
                s.rekeyKind=initial?0:(pair?2:1);s.rekeySkipGroup=pair && sameGroup;
                s.securityPhase=initial || pair?1:2;s.submitted=false;s.securityAck=false;s.authorizeRequested=false;
                s.portAuthorized=false;s.sinceMs=now;sec->keysPending=false;AthSecurity::wipe(sec->keys,sizeof(sec->keys));
            }
        }
        if(sec->replyPending && !sec->keysPending && !s.txOwned && !s.dataPending && !s.wmiOwned &&
           !s.maintenance.step && !s.radio.blocked && !s.radio.powerBlocked && !s.radio.appliedPS && !s.csa.blocked && !s.txPauseMask &&
           (!s.securityPhase || s.securityPhase==3 || s.securityPhase==5)){
            const char *error=submitNetworkPacket(sec->reply,sec->replyLength,false,now);
            if(error){s.error=error;s.cancelRequested=true;}
            else {sec->replyPending=false;sec->authorizePending=s.securityPhase==3;
                AthSecurity::wipe(sec->reply,sizeof(sec->reply));sec->replyLength=0;}
        }
        if(!sec->replyPending && !sec->keysPending && !sec->authorizePending && s.eapolCount && !s.rekeyKind){
            auto result=sec->supplicant.receive(s.eapolFrames[s.eapolRead],s.eapolLength[s.eapolRead]);
            AthSecurity::wipe(s.eapolFrames[s.eapolRead],512);s.eapolRead=(s.eapolRead+1)%4;--s.eapolCount;
            using Supplicant=AthSecurity::Supplicant<Ath10kKernelCrypto>;
            if(result==Supplicant::Fatal){s.error="native-wpa2-handshake-failed";s.cancelRequested=true;}
            else if(result!=Supplicant::Ignore){
                if(sec->supplicant.outputLength && sec->supplicant.outputLength<=sizeof(sec->reply)){
                    bcopy(sec->supplicant.output,sec->reply,sec->supplicant.outputLength);sec->replyLength=sec->supplicant.outputLength;sec->replyPending=true;}
                if(result==Supplicant::Keys || result==Supplicant::GroupKeys || result==Supplicant::PairKeys){
                    bcopy(sec->supplicant.keys,sec->keys,sizeof(sec->keys));sec->keysPending=true;sec->keyOperation=result;
                }
            }
        }
        if(sec->supplicant.pairwisePending()){
            s.eapolPause=true;if(!sec->pairDeadline)sec->pairDeadline=now+5000;
            if(now>=sec->pairDeadline){sec->supplicant.cancelPairwiseAttempt();sec->pairDeadline=0;s.eapolPause=false;}
        }else {sec->pairDeadline=0;s.eapolPause=sec->replyPending || sec->keysPending || sec->authorizePending;}
    }
    if(_nativeJoinPending){
        bool success=associated && s.portAuthorized && _nativeBeaconSent;
        bool failed=s.cancelRequested || s.stage==Ath10k::LinkStage::Failed || s.disconnect!=Ath10k::DisconnectStage::None || now>=_nativeJoinDeadline;
        if(success || failed){
            AirportWCL::ConnectionEvent event{};event.status=success?0:1;
            bcopy(_nativeJoin.bssid,event.peers[0].bssid,6);event.peers[0].driverStatus=success?kIOReturnSuccess:kIOReturnError;
            post(AirportWCL::ConnectComplete,&event,sizeof(event));_nativeJoinPending=false;
            AthSecurity::wipe(&_nativeJoin,sizeof(_nativeJoin));
            if(failed){s.cancelRequested=true;s.portAuthorized=false;networkLink(false);if(sec)sec->reset();}
        }
    }
    bool up=associated && s.portAuthorized && _networkLinkUp;
    // Native WCL consumes the Sequoia RSSI snapshot, not legacy 227/229 LQM.
    const int quality=up && s.signalDbm!=INT32_MIN?(s.signalDbm>=-70?100:(s.signalDbm>=-85?50:20)):-2;
    if(quality!=_nativeReportedQuality){
        _nativeInterface->setLQM(static_cast<unsigned long long>(static_cast<long long>(quality)));
        _nativeReportedQuality=quality;
    }
    if(up && s.signalDbm!=INT32_MIN && s.beaconFrameSerial!=_nativeReportedBeacon &&
       UInt32(s.beaconFrameSerial-_nativeReportedBeacon)>=8){
        AirportWCL::SequoiaLqmUpdate update{};update.rssiValid=1;update.rssi=s.signalDbm;
        post(AirportWCL::LqmUpdate,&update,sizeof(update));_nativeReportedBeacon=s.beaconFrameSerial;
    }
    if(_nativeWasUp && !up){
        AirportWCL::DeauthEvent event{};bcopy(s.bssid,event.bssid,6);event.reason=3;
        post(AirportWCL::Deauth,&event,sizeof(event));
        AirportWCL::LinkStatusEvent link{};bcopy(s.bssid,link.bssid,6);post(AirportWCL::LinkStatus,&link,sizeof(link));
        _nativeInterface->setCurrentApAddress(nullptr);_nativeInterface->setLinkState(kIO80211NetworkLinkDown,0,false,0);
        _nativeBSSBytes=0;_nativeReportedBeacon=0;if(sec)sec->reset();
    }
    _nativeWasUp=up;
}
