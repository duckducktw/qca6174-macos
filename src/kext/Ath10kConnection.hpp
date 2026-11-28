#pragma once
#include "Ath10kHTC.hpp"
#include "Ath10kBSS.hpp"
#include "Ath10kMulticast.hpp"
#include "../shared/Ath10kPMFTransport.hpp"

namespace Ath10k {
// Ethernet MTU excludes headers. Permit at most two in-band VLAN tags, not jumbo payloads.
static constexpr UInt32 kEthernetMTU=1500,kEthernetFrameMax=14+kEthernetMTU+8;
inline bool ethernetFrameAllowed(const UInt8 *p,UInt32 n){
    if(!p || n<14 || n>kEthernetFrameMax)return false;
    UInt32 header=14,tags=0;UInt16 type=UInt16((UInt16(p[12])<<8)|p[13]);
    while(type==0x8100 || type==0x88a8){
        if(++tags>2 || n<header+4)return false;
        type=UInt16((UInt16(p[header+2])<<8)|p[header+3]);header+=4;
    }
    return n-header<=kEthernetMTU;
}
// STA legacy/HT/VHT up to 80MHz/2SS. Wire layouts: Linux v6.12 wmi-tlv.h,
// htt.h and rx_desc.h; open and WPA2-PSK association share this state.
enum class LinkStage : UInt32 { Disabled, Scan, Start, Peer, Auth, Assoc,
    PeerAssoc, Authorize, Up, Barrier, Connected, Failed };
inline const char *linkStageName(LinkStage s) {
    const char *names[]={"disabled","waiting-scan","vdev-start","peer-create",
        "authenticating","associating","peer-assoc","peer-authorize","vdev-up",
        "firmware-barrier","connected-open","failed"};
    return names[static_cast<UInt32>(s)];
}
inline UInt32 connLE32(const UInt8 *p) {return htcReadLE16(p)|(UInt32(htcReadLE16(p+2))<<16);}
inline bool sameMAC(const UInt8 *a,const UInt8 *b) {
    for(UInt32 i=0;i<6;++i)if(a[i]!=b[i])return false;
    return true;
}
enum class DisconnectStage : UInt32 { None, Drain, Down, ClearPair, ClearGroup, DeletePeer, Stop, Barrier };
#ifndef ATH10K_DATA_TX_SLOTS
#define ATH10K_DATA_TX_SLOTS 64
#endif
#ifndef ATH10K_AMSDU_LIMIT
#define ATH10K_AMSDU_LIMIT 3
#endif
static constexpr UInt32 kDataTxSlots=ATH10K_DATA_TX_SLOTS;
static_assert(kDataTxSlots==16 || kDataTxSlots==32 || kDataTxSlots==64,"TX supports 16, 32 or 64 slots");
struct DataTxSlot {
    bool owned=false,ceConsumed=false,complete=false;
    UInt16 id=0;UInt32 ceNext=0,status=0,bytes=0;UInt64 sinceMs=0;bool httObserved=false,waitReported=false;
};
enum class RXSecurityFault : UInt32 {
    HeaderType,Direction,Protected,MoreFragments,FragmentNumber,Transmitter,MixedCipher,Count
};
inline const char *rxSecurityFaultName(RXSecurityFault fault){
    const char *names[]={"header-type","direction","protected-bit","more-fragments",
        "fragment-number","transmitter-bssid","mixed-cipher"};
    return names[static_cast<UInt32>(fault)];
}
// Capture only protocol metadata: never keys, IV contents, EAPOL or IP payload.
struct RXSecurityDiagnostics {
    UInt32 counts[static_cast<UInt32>(RXSecurityFault::Count)]{};
    RXSecurityFault last=RXSecurityFault::HeaderType;bool valid=false,nativeValid=false;
    UInt16 fc=0,sequenceControl=0,peer=0xffff;
    UInt32 attention=0,info=0,firstInfo=0,msduFlags=0,decap=0,length=0,msduCount=0;
    UInt8 originalHeader[26]{},nativeHeader[24]{},expectedBSSID[6]{};
    void record(const UInt8 *d,RXSecurityFault fault,const UInt8 *bssid,
                UInt16 httPeer=0xffff,UInt32 first=0,const UInt8 *native=nullptr){
        ++counts[static_cast<UInt32>(fault)];last=fault;valid=true;nativeValid=native!=nullptr;
        fc=htcReadLE16(d+236);sequenceControl=htcReadLE16(d+258);peer=httPeer;
        attention=connLE32(d+4);info=connLE32(d+12);firstInfo=first;
        msduFlags=connLE32(d+56);decap=(connLE32(d+32)>>8)&3;length=connLE32(d+24)&0x3fff;
        msduCount=0;
        // QoS Control exists only in a QoS original header. For non-QoS,
        // bytes 24/25 belong to crypto metadata and must not be exported.
        for(UInt32 i=0;i<26;++i)originalHeader[i]=(i<24 || (fc&0x008c)==0x0088)?d[236+i]:0;
        for(UInt32 i=0;i<24;++i)nativeHeader[i]=native?native[i]:0;
        for(UInt32 i=0;i<6;++i)expectedBSSID[i]=bssid[i];
    }
};
enum class ChannelSwitchPhase : UInt32 { Idle, Announced, Drain, Down, Restart, PeerPhy, PeerWidth, PeerRates, Up, Barrier, Confirm };
struct ChannelSwitchState {
    ChannelSwitchPhase phase=ChannelSwitchPhase::Idle;
    UInt8 primary=0,center=0,width=0,power=0,count=0,mode=0,armedCount=255;
    bool blocked=false,submitted=false,restartReply=false,failed=false,confirmed=false,widthChanged=false;
    UInt8 country[254]{},countryLength=0;bool countryPresent=false;
    UInt32 generation=0,armedGeneration=0,completed=0,rejected=0;
    UInt64 dueMs=0,sinceMs=0;
    const char *status="idle";
};
struct APConfig {
    UInt8 wmm[16]{},wmmCount=0,slot=2,preamble=2,protection=0,smps=0,nss=1,width=20;
    bool wmmPresent=false;
};
inline const char *channelSwitchPhaseName(ChannelSwitchPhase p){
    const char *names[]={"idle","announced","draining","vdev-down","vdev-restart","peer-phymode","peer-bandwidth","peer-rates","vdev-up","firmware-barrier","confirming-beacon"};
    return names[static_cast<UInt32>(p)];
}
struct ConnectionState {
    MulticastFilter multicast{};
    // Small maintenance state: no ConnectionState/cache temporaries on the kernel stack.
    struct RadioState {
        bool requestedPS=false,appliedPS=false,configuredPS=false,submitted=false,blocked=false,powerBlocked=false;
        bool wakeOnly=false,txWakeRequested=false;UInt32 txWakeRequests=0;UInt64 txWakeHoldUntilMs=0;
        UInt8 step=0,desiredPower=0,appliedPower=0,targetPower=0;bool targetPS=false;
        UInt8 vdevPower=0,targetDTIM=0,appliedDTIM=0;UInt16 targetBeacon=0,appliedBeacon=0;
        UInt32 generation=0,completed=0;UInt64 sinceMs=0;
    } radio{};
    struct APMaintenance {
        APConfig applied{},target{};
        bool valid=false,submitted=false,blocked=false,draining=false,ratesPending=false;
        // Initial PEER_ASSOC already programs rates before WPA2. Do not issue
        // another association merely because the maintenance cache is empty.
        bool peerRatesValid=false;UInt8 peerNSS=1,peerWidth=20;
        UInt8 step=0;UInt32 generation=0,completed=0;UInt64 sinceMs=0;
    } maintenance{};
    UInt8 negotiatedNSS=1,peerOperatingNSS=0,peerOperatingWidth=0,peerSMPS=255;
    // Immutable common association capabilities. OM/SMPS and CSA only derive
    // the applied rate set; they must not erase a stream from this ceiling.
    bool ratesRemembered=false;UInt16 negotiatedHTMask=0,negotiatedVHTRx=0xffff,negotiatedVHTTx=0xffff;
    UInt32 txTID[8]{},txAC[4]{},txAdmissionDropped=0;
    struct FirmwareStats {
        bool noiseValid=false,rateValid=false,vdevValid=false;
        int32_t noiseDbm=INT32_MIN;UInt32 txKbps=0,rxKbps=0,serial=0;
        UInt32 tx[4]{},retries[4]{},failures[4]{},notAcked=0;
        UInt64 observedMs=0,noiseObservedMs=0,rateObservedMs=0;
        UInt32 observedSerial=0,noiseSerial=0,rateSerial=0,observedNoiseSerial=0,observedRateSerial=0;
    } fwStats{};
    struct StatsMaintenance {UInt8 step=0;bool submitted=false;UInt32 generation=0,completed=0;UInt64 sinceMs=0,nextMs=0;} stats{};
    bool hiddenSSID=false;UInt8 negotiatedWidth=20,powerConstraintDb=0;
    UInt32 observedBeacon=0,observedRX=0,beaconTimeoutMs=30000,beaconMisses=0;
    struct ProbeState { bool active=false;UInt8 attempts=0;UInt64 deadlineMs=0; } probe{};
    bool lastTxProbe=false;
    struct SpectrumState {
        UInt8 frame[256]{};UInt16 length=0;bool pending=false,inFlight=false;
        UInt64 sinceMs=0;UInt32 received=0,completed=0,dropped=0;
    } spectrum{};
    bool lastTxSpectrum=false,lastTxPMF=false,lastTxSAE=false,pmfTXPending=false;
    bool beaconWarning=false;UInt64 beaconWarningSinceMs=0;
    ChannelSwitchState csa{};
    bool htEnabled=false,vhtEnabled=false,wmmConfigured=false;
    // SERVICE_READY capabilities, then intersected with AP and build limits.
    UInt32 firmwareHT=1,firmwareVHT=0;
    bool firmwareMFPSupport=false,pmfQoSTransported=false;
    UInt8 rfChains=1,nss=1,width=20,centerChannel=0;
    bool shortGI=false,firmwareReorder=false;
    UInt16 localHT=0,peerHT=0,vhtRxMap=0xffff,vhtTxMap=0xffff;
    UInt32 localVHT=0,peerVHT=0;
    const char *phyReason="not-negotiated";
    struct BASession {
        UInt16 peer=0,lastSeq=0;UInt8 tid=0,window=0;bool sequenceValid=false;
        UInt16 startSeq=0;bool startValid=false;UInt32 generation=0;
        UInt32 frames=0,gaps=0;
    } ba[64]{};
    UInt16 baSSN[16]{};bool baSSNValid[16]{};
    UInt32 baAdded=0,baDeleted=0,baIgnored=0,baOverflow=0,baFrames=0,baGaps=0;
    UInt8 htRates[16]{},htRateCount=0;UInt16 vhtMap=0xffff;
    UInt64 dataCETimeMs=0,dataHTTTimeMs=0,dataCEMaxMs=0,dataHTTMaxMs=0;
    UInt32 dataCECount=0,dataHTTCount=0;
    // Firmware retains payload ownership until HTT completion. An old frame
    // alone is not evidence that a still-progressing transmit queue is wedged.
    UInt32 dataDelayed=0,dataTimeouts=0,dataDrainDropped=0;
    const char *dataWatchdogReason="none";UInt32 dataWatchdogSlot=kDataTxSlots;UInt16 dataWatchdogID=0;UInt64 dataWatchdogAgeMs=0;
    UInt64 dataStallSinceMs=0,dataPauseSinceMs=0;
    bool dataStallArmed=false,dataPauseArmed=false;
    DataTxSlot dataTX[kDataTxSlots]{};
    UInt32 dataPending=0,dataWrite=0,dataRead=0,dataHigh=0,dataStalls=0;
    UInt32 stallManagement=0,stallFirmwarePause=0,stallSlots=0;
    UInt32 dataCompleted=0,dataNoAck=0,dataDiscard=0;
    UInt64 txBytes=0,rxBytes=0,dataLatencyMs=0,dataLatencyMaxMs=0;
    DisconnectStage disconnect=DisconnectStage::None;
    bool stopReply=false,deleteReply=false;
    // CE/credit completion does not prove that firmware created/deleted its
    // objects. Compensate an issued create even if its HTT MAP never arrived.
    bool vdevStartIssued=false,peerCreateIssued=false;
    struct CancelRollback {
        bool active=false,startPending=false,echoPending=false;
        UInt32 echoCookie=0;
    } rollback{};
    // Terminal AP metadata survives radio cleanup until native delivery.
    struct APDisconnect {
        bool valid=false,disassociation=false;
        UInt16 reason=0;UInt8 bssid[6]{};
    } apDisconnect{};
    struct ConnectionDiagnostics {
        UInt32 maps=0,unmaps=0,unknownUnmaps=0,peerBefore=0,peerAfter=0;
        UInt16 peer=0xffff;UInt8 vdev=255,peerMAC[6]{};bool own=false;
        UInt32 mlmeCount=0,mlmeLogged=0,mlmeChannel=0;
        UInt16 mlmeReason=0;UInt8 mlmeSubtype=0;
        UInt32 commands=0,lastCommand=0,lastCommandStage=0,lastCommandSecurity=0;
        UInt32 lastCommandDisconnect=0,lastCommandAP=0,lastCommandRadio=0;
        UInt32 rollbacks=0,lastRollbackStage=0;
        const char *lastDisconnectReason="none",*cancelOrigin="none",*lastCancelOrigin="none";
        UInt32 lastDisconnectStage=0,lastDisconnectSecurity=0,lastDisconnectPeers=0;
    } diagnostics{};
    UInt32 disconnectCount=0;
    LinkStage stage=LinkStage::Disabled;
    UInt8 securityAKM=2;
    bool secure=false,pmfRequired=false,portAuthorized=false,securityAck=false,authorizeRequested=false;
    bool cancelRequested=false,rekeySkipGroup=false,vdevStarted=false,vdevUp=false,eapolPause=false;
    UInt8 rekeyKind=0;UInt32 securityGeneration=0,beaconSerial=0;
    // Actual associated beacon, including TSF, for native freshness queries.
    UInt8 beaconFrame[0x800]{};UInt32 beaconFrameBytes=0,beaconFrameSerial=0;
    int32_t signalDbm=INT32_MIN;
    UInt8 installedPair[16]{},installedGroups[4][32]{};bool pairInstalled=false,groupInstalled[4]{};
    UInt32 securityPhase=0; // 0 EAPOL, 1 PTK, 2 GTK, 3 authorize, 4 barrier, 5 ready
    // Only protocol metadata; no key bytes or replay counters from SEC_IND.
    struct SecurityIndDiagnostics {
        UInt32 received=0,ignored=0,lateClears=0,disconnect=0;
        UInt16 peer=0xffff;UInt8 flags=0;
        bool mapped=false,retired=false,ack=false;
        const char *action="none";
    } securityInd{};
    UInt8 temporalKey[16]{},groupKey[32]{},groupIndex=0,groupCipher=0,groupRSC[6]{};
    UInt16 pairSeq[17]{},groupSeq[17]{};
    UInt8 pairAmsduCount[17]{},groupAmsduCount[17]{};
    bool pairSeqValid[17]{},groupSeqValid[17]{};
    UInt64 pairPN[17]{},groupPN[17]{};
    UInt16 eapolLength[4]{};UInt8 eapolFrames[4][512]{};
    UInt32 eapolRead=0,eapolWrite=0,eapolCount=0;
    bool lastTxEAPOL=false,eapolTxFailed=false;
    RegulatoryPolicy regulatory{};
    WMIBSS ap{};
    UInt8 local[6]{},ssid[32]{},bssid[6]{},ssidLength=0;
    UInt8 wmiSequence=10,httSequence=3,txPowerHalfDbm=40;
    UInt32 operatingChannel=0;
    UInt32 peerIDs[64]{};UInt32 peerCount=0;
    // A key-disable response can follow PEER_UNMAP on CE1. Keep the identities
    // belonging to this AP until teardown's STOP/ECHO barrier completes.
    UInt32 retiredPeerIDs[64]{};
    UInt16 aid=0,peerId=0xffff,txId=0,sequence=0;
    bool submitted=false,startReply=false,peerMapped=false,authReply=false,assocReply=false;
    UInt8 managementAttempts=0;UInt64 managementRetryMs=0;
    UInt32 txPauseMask=0,txPauseEvents=0;bool txWakePending=false;
    bool wmiOwned=false,txOwned=false,txComplete=false,txManagement=false;
    UInt32 wmiNext=0,txNext=0,txStatus=0,associationStatus=0;
    UInt64 sinceMs=0,txSinceMs=0,lastBeaconMs=0;
    UInt32 txPackets=0,txErrors=0,rxPackets=0,rxDropped=0;
    UInt32 rxAmsduSubframes=0,rxChainedBuffers=0,rxChainDrops=0;
    enum RxDrop : UInt32 { Status,Integrity,Length,Peer,Decap,Aggregate,Security,
        Replay,Key,Plaintext,Allocation,Port,EAPOL,Fragment,Duplicate,DropCount };
    UInt32 rxReasons[DropCount]{};RxDrop rxLastReason=Status;
    RXSecurityDiagnostics rxSecurity{};
    const char *error="none";
};
// Shared by BSD admission, direct data submission and deferred queue wakeup.
// A wake must remain pending until all temporary admission blockers clear.
inline bool dataTXAdmissionOpen(const ConnectionState &s){
    return s.stage==LinkStage::Connected && s.portAuthorized && !s.cancelRequested &&
        s.disconnect==DisconnectStage::None && !s.maintenance.blocked && !s.eapolPause &&
        !s.spectrum.pending && !s.spectrum.inFlight && !s.pmfTXPending && !s.probe.active && !s.csa.blocked &&
        !s.radio.blocked && !s.radio.powerBlocked && !s.txOwned && !s.txPauseMask &&
        s.dataPending<kDataTxSlots;
}
// Association survives a controlled-port closure during key replacement.
inline bool connectionAssociated(const ConnectionState &s){
    return s.stage==LinkStage::Connected && s.disconnect==DisconnectStage::None && !s.cancelRequested;
}
inline bool connectionHasError(const ConnectionState &s){
    return s.error && !(s.error[0]=='n' && s.error[1]=='o' && s.error[2]=='n' && s.error[3]=='e' && !s.error[4]);
}
inline void connectionFailure(ConnectionState &s,const char *reason){
    // Keep the first cause: a following UNMAP must not hide an AP's deauth.
    if(!connectionHasError(s))s.error=reason;
}
inline void wipeConnectionSecrets(ConnectionState &s){
    // Terminal association cleanup only; keep anti-reinstall caches during a live rekey.
    s.portAuthorized=false;s.authorizeRequested=s.securityAck=false;
    AthSecurity::wipe(s.temporalKey,sizeof(s.temporalKey));AthSecurity::wipe(s.groupKey,sizeof(s.groupKey));
    AthSecurity::wipe(s.installedPair,sizeof(s.installedPair));AthSecurity::wipe(s.installedGroups,sizeof(s.installedGroups));
    AthSecurity::wipe(s.eapolFrames,sizeof(s.eapolFrames));AthSecurity::wipe(s.eapolLength,sizeof(s.eapolLength));
    AthSecurity::wipe(s.groupRSC,sizeof(s.groupRSC));
    s.pairInstalled=false;for(auto &installed:s.groupInstalled)installed=false;
    s.eapolRead=s.eapolWrite=s.eapolCount=0;s.rekeyKind=0;s.securityPhase=0;++s.securityGeneration;
}
inline int dataTxSlotForID(const ConnectionState &s,UInt16 id){
    for(UInt32 i=0;i<kDataTxSlots;++i)if(s.dataTX[i].owned && s.dataTX[i].id==id)return static_cast<int>(i);
    return -1;
}
inline bool requestDisconnect(ConnectionState &s,UInt64 nowMs) {
    if(s.disconnect!=DisconnectStage::None)return true;
    // Disconnect handles a completed association. Cancel separately drains
    // and tears down a partial association or handshake.
    if(s.stage!=LinkStage::Connected || !s.portAuthorized || s.wmiOwned)return false;
    s.diagnostics.cancelOrigin="app-disconnect";
    s.diagnostics.lastDisconnectReason=connectionHasError(s)?s.error:"requested-disconnect";
    s.diagnostics.lastCancelOrigin=s.diagnostics.cancelOrigin;
    s.diagnostics.lastDisconnectStage=static_cast<UInt32>(s.stage);
    s.diagnostics.lastDisconnectSecurity=s.securityPhase;s.diagnostics.lastDisconnectPeers=s.peerCount;
    s.cancelRequested=false;s.disconnect=DisconnectStage::Drain;s.sinceMs=nowMs;s.submitted=false;
    s.portAuthorized=false;s.stopReply=false;return true;
}
inline void resetDisconnected(ConnectionState &s) {
    const MulticastFilter multicast=s.multicast;
    const bool requestedPS=s.radio.requestedPS;const UInt32 beaconMisses=s.beaconMisses,beaconTimeout=s.beaconTimeoutMs;
    const RegulatoryPolicy regulatory=s.regulatory;
    const UInt32 csaCompleted=s.csa.completed,csaRejected=s.csa.rejected;
    const UInt8 wmi=s.wmiSequence,htt=s.httSequence,power=s.txPowerHalfDbm;
    const UInt16 tx=s.txId,seq=s.sequence;
    const UInt64 txb=s.txBytes,rxb=s.rxBytes;
    const UInt32 count=s.disconnectCount+1,txp=s.txPackets,txe=s.txErrors,rxp=s.rxPackets,rxd=s.rxDropped;
    const bool mfp=s.firmwareMFPSupport,pmfQoS=s.pmfQoSTransported;
    const UInt32 fwHT=s.firmwareHT,fwVHT=s.firmwareVHT;const UInt8 chains=s.rfChains;
    UInt32 rxReasons[ConnectionState::DropCount];
    for(UInt32 i=0;i<ConnectionState::DropCount;++i)rxReasons[i]=s.rxReasons[i];
    const auto lastReason=s.rxLastReason;
    const auto securityDiagnostics=s.rxSecurity;
    const auto securityInd=s.securityInd;
    const auto diagnostics=s.diagnostics;
    const auto apDisconnect=s.apDisconnect;
    UInt8 mac[6];for(unsigned i=0;i<6;++i)mac[i]=s.local[i];
    AthSecurity::wipe(&s,sizeof(s));resetInPlace(s);
    s.regulatory=regulatory;s.csa.completed=csaCompleted;s.csa.rejected=csaRejected;
    s.multicast=multicast;
    s.radio.requestedPS=requestedPS;s.beaconMisses=beaconMisses;s.beaconTimeoutMs=beaconTimeout;
    s.wmiSequence=wmi;s.httSequence=htt;s.txId=tx;s.sequence=seq;s.txPowerHalfDbm=power;
    s.txBytes=txb;s.rxBytes=rxb;
    s.disconnectCount=count;s.txPackets=txp;s.txErrors=txe;s.rxPackets=rxp;s.rxDropped=rxd;
    s.firmwareHT=fwHT;s.firmwareVHT=fwVHT;s.rfChains=chains;s.firmwareMFPSupport=mfp;s.pmfQoSTransported=pmfQoS;
    for(UInt32 i=0;i<ConnectionState::DropCount;++i)s.rxReasons[i]=rxReasons[i];
    s.rxLastReason=lastReason;
    s.rxSecurity=securityDiagnostics;
    s.securityInd=securityInd;
    s.apDisconnect=apDisconnect;
    s.diagnostics=diagnostics;s.diagnostics.cancelOrigin="none";
    for(unsigned i=0;i<6;++i)s.local[i]=mac[i];
}
inline bool supportedLegacyRate(UInt8 value,bool five) {
    const UInt8 r=value&127;
    return (!five && (r==2 || r==4 || r==11 || r==22)) ||
        r==12 || r==18 || r==24 || r==36 || r==48 || r==72 || r==96 || r==108;
}
// QCA6174 local capabilities. Firmware performs PHY/CCMP processing; host
// supports up to 2SS/80MHz and advertises neither STBC nor beamforming.
#ifndef ATH10K_HT_VHT
#define ATH10K_HT_VHT 1
#endif
#ifndef ATH10K_MAX_WIDTH
#define ATH10K_MAX_WIDTH 80
#endif
#ifndef ATH10K_MAX_NSS
#define ATH10K_MAX_NSS 2
#endif
static_assert(ATH10K_MAX_WIDTH==20 || ATH10K_MAX_WIDTH==40 || ATH10K_MAX_WIDTH==80,"width must be 20, 40 or 80");
static_assert(ATH10K_MAX_NSS>=1 && ATH10K_MAX_NSS<=2,"QCA6174 supports at most 2SS");
// Shared local capability policy and IE encoding for association and native queries.
inline UInt16 localHTCapability(UInt32 firmwareHT){
    UInt16 cap=12|(ATH10K_MAX_WIDTH>=40?2:0); // Disabled SMPS, host width ceiling.
    if(firmwareHT&2)cap|=0x20;
    if(ATH10K_MAX_WIDTH>=40 && (firmwareHT&0x800))cap|=0x40;
    return cap;
}
inline UInt32 localVHTCapability(UInt32 firmwareVHT){return (3u<<23)|(firmwareVHT&(1u<<5));}
inline UInt16 localVHTMCSMap(UInt8 streams){
    UInt16 map=0xffff;
    if(!streams || streams>ATH10K_MAX_NSS)return map;
    for(UInt32 n=0;n<streams;++n)
        map=static_cast<UInt16>((map&~(3u<<(2*n)))|((ATH10K_MAX_WIDTH==20?1u:2u)<<(2*n)));
    return map;
}
inline UInt32 writeLocalHTCapabilityIE(UInt8 *out,UInt32 capacity,UInt16 cap,UInt8 streams){
    if(!out || capacity<28 || !streams || streams>ATH10K_MAX_NSS)return 0;
    htcZero(out,28);out[0]=45;out[1]=26;auto h=out+2;htcWriteLE16(h,cap);
    h[2]=3|(6<<2); // 64 KiB A-MPDU, 8us spacing; MPDUs delivered separately.
    for(UInt32 i=0;i<streams;++i)h[3+i]=0xff;
    h[15]=1; // TX MCS set defined, equal to RX.
    return 28;
}
inline UInt32 writeLocalVHTCapabilityIE(UInt8 *out,UInt32 capacity,UInt32 cap,UInt16 rx,UInt16 tx){
    if(!out || capacity<14 || (rx&3)==3 || (tx&3)==3)return 0;
    htcZero(out,14);out[0]=191;out[1]=12;auto v=out+2;
    htcWriteLE32(v,cap);htcWriteLE16(v+4,rx);htcWriteLE16(v+8,tx);
    return 14;
}
inline bool homeChannelAllowed(UInt32 c){return channelCanConnect(c);}
inline UInt16 intersectVHTMap(UInt16 a,UInt16 b){
    UInt16 result=0;for(UInt32 n=0;n<8;++n){UInt16 x=(a>>(2*n))&3,y=(b>>(2*n))&3;
        result=static_cast<UInt16>(result|((x==3 || y==3?3:(x<y?x:y))<<(2*n)));}return result;
}
// Interpret both revised (width=1, second center) and original 160/80+80
// encodings. Return the primary 80 segment; our station never advertises 160.
inline bool vhtPrimary80(UInt32 primary,UInt8 width,UInt8 first,UInt8 second,UInt8 &center){
    center=0;if(width>3)return false;
    if(!width)return second==0;
    int c=first;
    if(width==2){if(second || first<8)return false;c=int(first)+(primary<first?-8:8);}
    if(c<1 || c>255 || !channelDefinitionAllowed(primary,UInt32(c),80,true))return false;
    if(width==2 || (width==1 && second && (int(first)-int(second)==8 || int(second)-int(first)==8))){
        const int full=width==2?first:second;
        const int other=2*full-c;
        if(other<1 || other>255 || !channelDefinitionAllowed(UInt32(other-6),UInt32(other),80,true))return false;
    }else if(second || width==3){
        const int distance=int(first)-int(second);
        if(!second || (distance>=-16 && distance<=16) ||
           !channelDefinitionAllowed(UInt32(second-6),second,80,true))return false;
    }
    center=static_cast<UInt8>(c);return true;
}
template<class Config>
inline void negotiateHighThroughput(Config &s){
    s.htEnabled=s.vhtEnabled=false;s.htRateCount=0;s.vhtMap=0xffff;
    s.vhtRxMap=s.vhtTxMap=0xffff;s.nss=1;s.width=20;s.centerChannel=static_cast<UInt8>(s.operatingChannel);
    s.localHT=s.peerHT=0;s.localVHT=s.peerVHT=0;s.shortGI=false;
    s.phyReason="ht-vht-disabled";
    if(!ATH10K_HT_VHT || !(s.firmwareHT&1))return;
    const auto &a=s.ap;
    s.phyReason="missing-ht-capability-or-operation";
    if(!a.htPresent || !a.htOperation)return;
    s.phyReason="missing-wmm";
    if(!a.wmmPresent)return;
    const UInt16 apHT=htcReadLE16(a.ht);
    const UInt8 chains=s.rfChains>ATH10K_MAX_NSS?ATH10K_MAX_NSS:s.rfChains;
    // SMPS is a temporary TX restriction, not the AP's MCS capability ceiling.
    // desiredAPConfig applies it after association, without erasing common maps.
    UInt8 streams=chains;if(!streams)streams=1;
    if((a.ht[15]&3)==3){const UInt8 txStreams=static_cast<UInt8>(((a.ht[15]>>2)&3)+1);
        if(txStreams<streams)streams=txStreams;}
    s.phyReason="ht-basic-mcs-outside-local-capabilities";
    for(UInt32 i=0;i<16;++i)if((i>=streams && a.htOp[6+i]) ||
        (i<streams && (a.htOp[6+i]&a.ht[3+i])!=a.htOp[6+i]))return;
    for(UInt32 n=0;n<streams;++n)for(UInt32 i=0;i<8;++i)if(a.ht[3+n]&(1u<<i)){
        s.htRates[s.htRateCount++]=static_cast<UInt8>(8*n+i);s.nss=static_cast<UInt8>(n+1);}
    s.phyReason="no-common-ht-mcs";
    if(!s.htRateCount)return;
    s.htEnabled=true;
    const UInt8 offset=a.htOp[1]&3;
    const int secondary=static_cast<int>(s.operatingChannel)+(offset==1?4:-4);
    // Record *which* gate keeps us at 20 MHz: measured throughput (~88 Mbps) already
    // exceeds the HT20-1SS ceiling, so the bookkeeping is what is wrong, not the radio.
    // The reason string surfaces through Ath10kConnectionPHYReason.
    if(!(ATH10K_MAX_WIDTH>=40))
        s.phyReason="ht40-disabled-in-build";
    else if(!(apHT&2))
        s.phyReason="ap-ht-cap-not-40mhz";
    else if(!(a.htOp[1]&4))
        s.phyReason="ap-ht-op-not-40mhz";
    else if(!(offset==1 || offset==3))
        s.phyReason="ap-no-secondary-offset";
    else if(secondary<=0)
        s.phyReason="ht40-secondary-invalid";
    else if(!channelDefinitionAllowed(s.operatingChannel,
                static_cast<UInt32>((static_cast<int>(s.operatingChannel)+secondary)/2),40,
                s.regulatory.stationDFS))
        s.phyReason="ht40-channel-not-allowed";
    if(ATH10K_MAX_WIDTH>=40 && (apHT&2) && (a.htOp[1]&4) && (offset==1 || offset==3) &&
       secondary>0 && channelDefinitionAllowed(s.operatingChannel,
           static_cast<UInt32>((static_cast<int>(s.operatingChannel)+secondary)/2),40,s.regulatory.stationDFS)){
        s.width=40;s.centerChannel=static_cast<UInt8>((s.operatingChannel+secondary)/2);}
    // Disabled SMPS is essential: zero CapInfo incorrectly advertises static SMPS.
    s.localHT=localHTCapability(s.firmwareHT);
    // SGI is used only when both sides support the selected width.
    s.shortGI=(apHT&s.localHT&(s.width>=40?0x40:0x20))!=0;
    s.peerHT=static_cast<UInt16>(apHT & ~(0x0100u|0x0300u|1u));
    s.phyReason="ht-common-capabilities";
    if(s.operatingChannel<=14)return;
    s.phyReason="ap-missing-vht-capability";if(!a.vhtPresent)return;
    s.phyReason="ap-missing-vht-operation";if(!a.vhtOperation)return;
    UInt8 primary80=0;
    s.phyReason="ap-invalid-vht-operation";
    if(!vhtPrimary80(s.operatingChannel,a.vhtOp[0],a.vhtOp[1],a.vhtOp[2],primary80))return;
    const UInt16 rx=htcReadLE16(a.vht+4),tx=htcReadLE16(a.vht+8),basic=htcReadLE16(a.vhtOp+3);
    const UInt8 rxNss0=rx&3,txNss0=tx&3;
    s.phyReason="no-common-vht-stream";if(rxNss0==3 || txNss0==3)return;
    // VHT MCS maps use two bits per spatial stream: 0=MCS 0..7,
    // 1=MCS 0..8, 2=MCS 0..9 and 3=not supported.  Some RM.4.4.1
    // beacons (including the QCA6174 AP seen in the 0.0.66 log) encode the
    // Basic MCS Map as zero even though the AP only has one usable stream.
    // Treat that legacy zero as a one-stream baseline, while still rejecting
    // an explicit basic requirement for a stream the AP advertises as absent.
    UInt8 vhtWidth=s.width,vhtCenter=s.centerChannel;
    if(ATH10K_MAX_WIDTH>=80 && (apHT&2) && a.vhtOp[0]>=1 && a.vhtOp[0]<=3){
        // 160/80+80 AP operation also contains the primary 80MHz segment.
        // Only explicitly allowed contiguous four-channel segments are used.
        const UInt32 c=primary80;bool valid=c>=6;
        bool primary=false;for(int delta=-6;delta<=6;delta+=4){const int ch=static_cast<int>(c)+delta;
            if(ch<=14 || !wmiScanChannelAllowed(static_cast<UInt32>(ch)) || (!s.regulatory.stationDFS && channelNeedsDFS(ch)))valid=false;
            if(ch==static_cast<int>(s.operatingChannel))primary=true;}
        if(valid && primary && channelDefinitionAllowed(s.operatingChannel,c,80,s.regulatory.stationDFS)){vhtWidth=80;vhtCenter=static_cast<UInt8>(c);}
    }
    const UInt16 localMap=localVHTMCSMap(streams);
    s.vhtRxMap=intersectVHTMap(localMap,tx);s.vhtTxMap=intersectVHTMap(localMap,rx);
    const UInt16 commonMap=intersectVHTMap(s.vhtRxMap,s.vhtTxMap);
    s.phyReason="vht-basic-mcs-not-supported";
    if(basic){
        for(UInt32 n=0;n<8;++n){
            const UInt8 required=(basic>>(2*n))&3;
            if(required==3)continue;
            const UInt8 apRx=(rx>>(2*n))&3,apTx=(tx>>(2*n))&3;
            if(n>=streams || apRx==3 || apTx==3)return;
            const UInt8 available=apRx<apTx?apRx:apTx;
            const UInt8 local=(commonMap>>(2*n))&3;
            if(local==3 || required>available || required>local)return;
        }
    }
    s.width=vhtWidth;s.centerChannel=vhtCenter;
    s.vhtMap=commonMap;s.vhtEnabled=true;s.nss=1;
    for(UInt32 n=0;n<streams;++n)if(((commonMap>>(2*n))&3)!=3)s.nss=static_cast<UInt8>(n+1);
    // Bound RX MPDU to 3895 and A-MPDU to 64KiB. No beamforming/STBC claims.
    s.localVHT=localVHTCapability(s.firmwareVHT);
    s.peerVHT=connLE32(a.vht)&~((1u<<4)|(1u<<7)|(7u<<8)|(0xfffu<<11));
    if(s.width==80)s.shortGI=(s.localVHT&connLE32(a.vht)&(1u<<5))!=0;
    s.phyReason="vht-common-capabilities";
}
inline UInt32 peerPhyMode(const ConnectionState &s,UInt8 width){return s.vhtEnabled?(width==80?10:(width==40?9:8)):
    (s.htEnabled?(s.operatingChannel>14?(width==40?6:4):(width==40?7:5)):(s.operatingChannel>14?0:1));}
inline UInt32 connectionPhyMode(const ConnectionState &s){return peerPhyMode(s,s.width);}
inline UInt8 peerBandwidth(const ConnectionState &s){
    UInt8 width=s.ratesRemembered && s.negotiatedWidth<s.width?s.negotiatedWidth:s.width;
    if(s.peerOperatingWidth && s.peerOperatingWidth<width)width=s.peerOperatingWidth;
    return s.htEnabled?width:20;
}
template<class Config>
inline void rememberNegotiatedRates(Config &s){
    s.negotiatedNSS=s.nss;s.negotiatedWidth=20;s.negotiatedHTMask=0;
    if(s.htEnabled && (s.localHT&htcReadLE16(s.ap.ht)&2))
        s.negotiatedWidth=s.vhtEnabled && ATH10K_MAX_WIDTH>=80?80:40;
    for(UInt32 i=0;i<s.htRateCount;++i)if(s.htRates[i]<16)s.negotiatedHTMask|=UInt16(1u<<s.htRates[i]);
    s.negotiatedVHTRx=s.vhtRxMap;s.negotiatedVHTTx=s.vhtTxMap;s.ratesRemembered=true;
}
struct PeerRateSet {UInt8 ht[16]{},count=0,nss=1,width=20;UInt16 rx=0xffff,tx=0xffff;bool shortGI=false;};
inline PeerRateSet peerRateSet(const ConnectionState &s,UInt8 nss,UInt8 width){
    PeerRateSet r;r.nss=nss?nss:1;r.width=width;
    const UInt8 ceiling=s.ratesRemembered?s.negotiatedNSS:ATH10K_MAX_NSS;
    if(r.nss>ceiling)r.nss=ceiling?ceiling:1;
    UInt16 mask=s.negotiatedHTMask;
    if(!s.ratesRemembered)for(UInt32 i=0;i<s.htRateCount;++i)if(s.htRates[i]<16)mask|=UInt16(1u<<s.htRates[i]);
    if(s.htEnabled)for(UInt32 i=0;i<16 && i<8u*r.nss;++i)if(mask&(1u<<i))r.ht[r.count++]=UInt8(i);
    if(s.vhtEnabled){UInt16 local=0xffff;
        for(UInt32 i=0;i<r.nss;++i)local=UInt16((local&~(3u<<(2*i)))|((width==20?1u:2u)<<(2*i)));
        r.rx=intersectVHTMap(local,s.ratesRemembered?s.negotiatedVHTRx:s.vhtRxMap);
        r.tx=intersectVHTMap(local,s.ratesRemembered?s.negotiatedVHTTx:s.vhtTxMap);}
    r.shortGI=s.vhtEnabled && width==80?(s.localVHT&s.peerVHT&(1u<<5))!=0:
        (s.localHT&s.peerHT&(width>=40?0x40:0x20))!=0;
    return r;
}
inline void applyPeerRates(ConnectionState &s,UInt8 nss,UInt8 width){
    const auto r=peerRateSet(s,nss,width);s.nss=r.nss;s.htRateCount=r.count;s.shortGI=r.shortGI;
    for(UInt32 i=0;i<r.count;++i)s.htRates[i]=r.ht[i];
    s.vhtRxMap=r.rx;s.vhtTxMap=r.tx;s.vhtMap=intersectVHTMap(r.rx,r.tx);
}
inline void connTLV(UInt8 *,UInt16,UInt16);
template<class Config>
inline UInt8 stationPowerLimitFor(const Config &s,UInt32 primary,UInt32 center,UInt8 width,
                                 const UInt8 *country=nullptr,UInt32 countryLength=0){
    auto p=s.regulatory;p.powerCeilingHalfDbm=255;
    const UInt8 maximum=regulatoryPower(p,primary,center,width);
    const int low=width==80?int(center)-6:(width==40?int(center)-2:int(primary));
    const UInt32 channels=width==80?4:(width==40?2:1);
    int apMaximum=-1;
    for(UInt32 i=0;i<channels;++i){const int ap=country?apCountryPower(country,countryLength,UInt32(low+4*int(i))):apCountryPower(s.ap,UInt32(low+4*int(i)));
        if(ap>=0 && (apMaximum<0 || ap<apMaximum))apMaximum=ap;}
    const UInt32 reduction=2u*s.powerConstraintDb;
    const UInt32 base=apMaximum>=0?UInt32(apMaximum):maximum;
    UInt8 constrained=static_cast<UInt8>(base>reduction?base-reduction:0);
    if(constrained>maximum)constrained=maximum;
    return constrained<s.regulatory.powerCeilingHalfDbm?constrained:s.regulatory.powerCeilingHalfDbm;
}
template<class Config>
inline UInt8 stationPowerLimit(const Config &s){
    return stationPowerLimitFor(s,s.operatingChannel,s.centerChannel,s.width);
}
inline UInt8 stationOperationalPowerLimit(const ConnectionState &s){
    const UInt8 power=stationPowerLimit(s);
    return s.radio.vdevPower && power>s.radio.vdevPower?s.radio.vdevPower:power;
}
inline bool stationWatchdog(ConnectionState &s,UInt64 nowMs,bool scanActive){
    if(s.stage!=LinkStage::Connected || !s.portAuthorized || s.csa.phase!=ChannelSwitchPhase::Idle || scanActive || s.rekeyKind || s.eapolPause){
        s.lastBeaconMs=nowMs;if(s.beaconWarning)s.beaconWarningSinceMs=nowMs;if(s.probe.active)s.txWakePending=true;s.probe={};return false;}
    if(s.radio.blocked){if(!s.probe.active)s.lastBeaconMs=nowMs;return false;}
    if(!s.lastBeaconMs || s.observedBeacon!=s.beaconSerial || s.observedRX!=s.rxPackets){
        s.lastBeaconMs=nowMs;s.observedBeacon=s.beaconSerial;s.observedRX=s.rxPackets;
        s.beaconWarning=false;s.beaconWarningSinceMs=0;if(s.probe.active)s.txWakePending=true;s.probe={};return false;}
    if(s.probe.active)return false;
    if(s.beaconWarning && !s.beaconWarningSinceMs)s.beaconWarningSinceMs=nowMs;
    const bool warningExpired=s.beaconWarning && nowMs>=s.beaconWarningSinceMs && nowMs-s.beaconWarningSinceMs>=5000;
    if(!warningExpired && (nowMs<s.lastBeaconMs || nowMs-s.lastBeaconMs<s.beaconTimeoutMs))return false;
    ++s.beaconMisses;s.probe.active=true;s.probe.deadlineMs=nowMs;return true;
}
// Directed Probe Request. NO-IR/DFS channels use a passive response window instead.
inline UInt32 buildAPProbe(UInt8 *f,UInt32 capacity,ConnectionState &s){
    if(!f || capacity<128 || s.ssidLength>32 || !s.ap.rateCount || s.ap.rateCount>32)return 0;
    htcZero(f,128);htcWriteLE16(f,0x40);
    for(UInt32 i=0;i<6;++i){f[4+i]=s.bssid[i];f[10+i]=s.local[i];f[16+i]=s.bssid[i];}
    htcWriteLE16(f+22,(s.sequence++&0xfff)<<4);f[24]=0;f[25]=s.ssidLength;
    UInt32 n=26;for(UInt32 i=0;i<s.ssidLength;++i)f[n++]=s.ssid[i];
    const UInt8 count=s.ap.rateCount<8?s.ap.rateCount:8;f[n++]=1;f[n++]=count;
    for(UInt32 i=0;i<count;++i)f[n++]=s.ap.rates[i];
    if(s.ap.rateCount>count){f[n++]=50;f[n++]=s.ap.rateCount-count;for(UInt32 i=count;i<s.ap.rateCount;++i)f[n++]=s.ap.rates[i];}
    return n;
}
inline UInt32 radioCookie(const ConnectionState &s){return 0xa7820000u|(s.radio.generation&0xffff);}
inline UInt32 buildRadioMaintenance(UInt8 *f,UInt32 capacity,UInt8 endpoint,const ConnectionState &s){
    const auto &r=s.radio;if(!f || capacity<256 || !endpoint || endpoint==255 || !r.step || r.step>11)return 0;
    htcZero(f,256);UInt32 n=28,cmd=0x9002;
    if(r.step<=5){connTLV(f+12,109,12);htcWriteLE32(f+20,r.step-1);
        const UInt32 values[]={0,1,0,r.targetBeacon?UInt32(r.targetBeacon)*1024/1000:100,0};
        htcWriteLE32(f+24,values[r.step-1]);
    }else if(r.step==9 || r.step==11){cmd=0x5008;connTLV(f+12,95,12);
        htcWriteLE32(f+20,r.step==9?13:3);htcWriteLE32(f+24,r.step==9?r.targetDTIM:r.targetBeacon);
    }else if(r.step==6 || r.step==10){n=24;cmd=0x9001;connTLV(f+12,108,8);htcWriteLE32(f+20,r.step==6 && r.targetPS?1:0);
    }else if(r.step==7){cmd=0x4003;connTLV(f+12,82,12);htcWriteLE32(f+20,s.operatingChannel<=14?3:4);htcWriteLE32(f+24,r.targetPower);
    }else {n=20;cmd=0x1d001;connTLV(f+12,149,4);htcWriteLE32(f+16,radioCookie(s));}
    f[0]=endpoint;f[1]=1;f[5]=s.wmiSequence;htcWriteLE16(f+2,n-8);htcWriteLE32(f+8,cmd);return n;
}
// Prepared AP/PHY configuration only: no keys, queues or firmware ownership.
inline bool connectionRSNAccepted(const ConnectionState &s,const UInt8 *ie,size_t bytes){
    if(s.securityAKM==8)return s.pmfRequired && AthSecurity::validSAEPersonalRSN(ie,bytes);
    return AthSecurity::supportsPSKAKM(ie,bytes,s.securityAKM);
}
struct CandidateConfiguration {
    WMIBSS ap{};RegulatoryPolicy regulatory{};
    UInt32 operatingChannel=0,firmwareHT=0,firmwareVHT=0,localVHT=0,peerVHT=0;
    UInt8 rfChains=1,nss=1,width=20,centerChannel=0,groupCipher=0;
    UInt8 powerConstraintDb=0,txPowerHalfDbm=0,htRates[16]{},htRateCount=0;
    UInt8 negotiatedNSS=1,negotiatedWidth=20;
    UInt16 localHT=0,peerHT=0,vhtMap=0xffff,vhtRxMap=0xffff,vhtTxMap=0xffff;
    UInt16 negotiatedHTMask=0,negotiatedVHTRx=0xffff,negotiatedVHTTx=0xffff;
    bool htEnabled=false,vhtEnabled=false,shortGI=false,ratesRemembered=false;
    const char *phyReason="not-negotiated";
};
static_assert(sizeof(CandidateConfiguration)<=2048,"candidate must fit bounded kernel stack budget");
// The output is usable only on success; the live connection is read-only.
// BSSID selection/ranking belongs to the caller, but SSID/security must match.
inline const char *prepareBSSCandidate(const ConnectionState &s,const WMIBSS &ap,CandidateConfiguration &candidate) {
    if(s.ssidLength>sizeof(s.ssid) || ap.ssidLength>sizeof(ap.ssid))return "invalid-ssid-length";
    if(!(s.hiddenSSID && ap.hidden)){
        if(ap.ssidLength!=s.ssidLength)return "candidate-ssid-mismatch";
        for(UInt32 j=0;j<s.ssidLength;++j)if(ap.ssid[j]!=s.ssid[j])return "candidate-ssid-mismatch";
    }
    if(s.secure){if(!ap.privacy || !connectionRSNAccepted(s,ap.rsn,ap.rsnLength))return s.securityAKM==8?"sae-ccmp-pmf-required":"wpa2-psk-ccmp-pairwise-required";}
    else if(ap.privacy || ap.rsnPresent || ap.wpaPresent)return "protected-network-not-supported";
    if(!(ap.capabilities&1) || (ap.capabilities&2))return "not-infrastructure-bss";
    if(!ap.beaconInterval || !ap.rateCount || ap.rateCount>sizeof(ap.rates) || !ap.ratesValid)return "bss-missing-valid-rates";
    // RX metadata describes where a beacon was heard, not necessarily
    // the AP's primary channel. Preserve it and resolve the home channel
    // independently from the DS/HT fields validated by recordWMIBSS.
    const UInt32 channel=ap.advertisedChannel?ap.advertisedChannel:ap.rxChannel;
    if(!wmiScanChannelAllowed(channel) || !wmiScanChannelAllowed(ap.rxChannel))return "bss-channel-unsupported";
    if((channel>14)!=(ap.rxChannel>14))return "bss-channel-band-mismatch";
    if(channelNeedsDFS(channel) && !s.regulatory.stationDFS)return "dfs-connection-not-supported";
    // Candidate rejection must precede changes to the selected AP.
    UInt32 commonRates=0;
    for(UInt32 j=0;j<ap.rateCount;++j){
        if(supportedLegacyRate(ap.rates[j],channel>14))++commonRates;
        else if(ap.rates[j]&128)return "unsupported-basic-rate";
    }
    if(!commonRates)return "no-common-rate";
    candidate={};
    candidate.regulatory=s.regulatory;candidate.firmwareHT=s.firmwareHT;
    candidate.firmwareVHT=s.firmwareVHT;candidate.rfChains=s.rfChains;
    candidate.txPowerHalfDbm=s.txPowerHalfDbm;
    candidate.operatingChannel=channel;
    candidate.ap=ap;candidate.ap.rateCount=0;
    candidate.groupCipher=s.secure?AthSecurity::wpa2GroupCipher(ap.rsn,ap.rsnLength):0;
    for(UInt32 j=0;j<ap.rateCount;++j){
        if(!supportedLegacyRate(ap.rates[j],channel>14))continue;
        candidate.ap.rates[candidate.ap.rateCount++]=ap.rates[j];
    }
    negotiateHighThroughput(candidate);
    rememberNegotiatedRates(candidate);
    candidate.powerConstraintDb=ap.powerConstraintDb;
    const UInt8 limit=stationPowerLimit(candidate);
    if(!limit)return "invalid-channel-definition";
    if(!candidate.txPowerHalfDbm)return "invalid-tx-power";
    if(candidate.txPowerHalfDbm>limit)candidate.txPowerHalfDbm=limit;
    return nullptr;
}
inline void applyBSSCandidate(ConnectionState &s,const CandidateConfiguration &candidate) {
    // Commit only after every candidate check has passed.
    s.ap=candidate.ap;
    s.operatingChannel=candidate.operatingChannel;
    s.groupCipher=candidate.groupCipher;
    s.powerConstraintDb=candidate.powerConstraintDb;
    s.txPowerHalfDbm=candidate.txPowerHalfDbm;
    s.htRateCount=candidate.htRateCount;
    s.htEnabled=candidate.htEnabled;
    s.vhtEnabled=candidate.vhtEnabled;
    s.nss=candidate.nss;
    s.width=candidate.width;
    s.centerChannel=candidate.centerChannel;
    s.shortGI=candidate.shortGI;
    s.localHT=candidate.localHT;
    s.peerHT=candidate.peerHT;
    s.localVHT=candidate.localVHT;
    s.peerVHT=candidate.peerVHT;
    s.vhtMap=candidate.vhtMap;
    s.vhtRxMap=candidate.vhtRxMap;
    s.vhtTxMap=candidate.vhtTxMap;
    s.phyReason=candidate.phyReason;
    s.negotiatedNSS=candidate.negotiatedNSS;
    s.negotiatedWidth=candidate.negotiatedWidth;
    s.negotiatedHTMask=candidate.negotiatedHTMask;
    s.negotiatedVHTRx=candidate.negotiatedVHTRx;
    s.negotiatedVHTTx=candidate.negotiatedVHTTx;
    s.ratesRemembered=candidate.ratesRemembered;
    for(UInt32 j=0;j<16;++j)s.htRates[j]=candidate.htRates[j];
    s.apDisconnect={}; // New accepted candidate cannot inherit an old AP cause.
}
inline const char *selectOpenBSS(ConnectionState &s,const WMIBSSCache &cache) {
    for(UInt32 i=0;i<cache.count;++i){
        const auto &ap=cache.entries[i];
        if(!sameMAC(ap.bssid,s.bssid))continue;
        if(!(s.hiddenSSID && ap.hidden)){
            if(ap.ssidLength!=s.ssidLength)continue;
            bool equal=true;for(UInt32 j=0;j<s.ssidLength && j<sizeof(s.ssid);++j)if(ap.ssid[j]!=s.ssid[j])equal=false;
            if(!equal)continue;
        }
        CandidateConfiguration candidate{};
        const char *error=prepareBSSCandidate(s,ap,candidate);
        if(error)return error;
        applyBSSCandidate(s,candidate);
        return nullptr;
    }
    return "configured-bss-not-found";
}
inline void connTLV(UInt8 *p,UInt16 tag,UInt16 bytes){htcWriteLE16(p,bytes);htcWriteLE16(p+2,tag);}
inline void writeWMMCommand(UInt8 *f,const UInt8 *parameters){
    connTLV(f+12,83,8);
    for(UInt32 i=0;i<4;++i){const UInt8 *ac=parameters+i*4;UInt8 *t=f+24+i*28;
        connTLV(t,84,24);htcWriteLE32(t+4,(1u<<(ac[1]&15))-1);htcWriteLE32(t+8,(1u<<(ac[1]>>4))-1);
        htcWriteLE32(t+12,ac[0]&15);htcWriteLE32(t+16,UInt32(htcReadLE16(ac+2))*32);
        htcWriteLE32(t+20,(ac[0]>>4)&1);}
}
inline UInt32 buildConnectionWMI(UInt8 *f,UInt32 capacity,UInt8 endpoint,const ConnectionState &s,
                                LinkStage stage=LinkStage::Disabled,UInt8 peerNSS=0,UInt8 peerWidth=0) {
    if(!f || capacity<256 || !endpoint || endpoint==255)return 0;
    htcZero(f,256);UInt32 bytes=0,cmd=0;auto *p=f+16;
    switch(stage==LinkStage::Disabled?s.stage:stage){
    case LinkStage::Start: {
        const UInt8 limit=regulatoryPower(s.regulatory,s.operatingChannel,s.centerChannel,s.width);
        if(!channelDefinitionAllowed(s.operatingChannel,s.centerChannel,s.width,s.regulatory.stationDFS) || !limit || !s.txPowerHalfDbm || s.txPowerHalfDbm>limit)return 0;
        cmd=0x5003;bytes=120;connTLV(f+12,88,72);
        htcWriteLE32(p+8,s.ap.beaconInterval);htcWriteLE32(p+12,s.ap.timPresent?s.ap.dtimPeriod:1);
        htcWriteLE32(p+20,s.ssidLength);
        for(UInt32 i=0;i<s.ssidLength;++i)p[24+i]=s.ssid[i];
        connTLV(f+88,80,24);
        UInt32 freq=s.operatingChannel<=14?2407+5*s.operatingChannel:5000+5*s.operatingChannel;
        htcWriteLE32(f+92,freq);htcWriteLE32(f+96,s.operatingChannel<=14?2407+5*s.centerChannel:5000+5*s.centerChannel);
        // Linux ath10k_vdev_start_restart enables chan_radar for AP, not STA.
        htcWriteLE32(f+104,connectionPhyMode(s)|(s.htEnabled?1u<<11:0)|(s.vhtEnabled?1u<<12:0));
        writeChannelPower(f+108,f+112,s.txPowerHalfDbm);connTLV(f+116,18,0);break;
    }
    case LinkStage::Peer:
        cmd=0x6001;bytes=32;connTLV(f+12,97,16);
        for(UInt32 i=0;i<6;++i)p[4+i]=s.bssid[i]; // generic peer, same as Linux station path
        break;
    case LinkStage::PeerAssoc: {
        if(s.pmfRequired && !s.pmfQoSTransported)return 0;
        if(s.ap.wmmPresent && !s.wmmConfigured){
            cmd=0x4006;bytes=136;writeWMMCommand(f,s.ap.wmm);
            break;
        }
        const auto r=peerRateSet(s,peerNSS?peerNSS:s.nss,peerWidth?peerWidth:peerBandwidth(s));
        cmd=0x6005;const UInt32 rates=(s.ap.rateCount+3u)&~3u,htRates=(r.count+3u)&~3u;
        bytes=120+rates+htRates;connTLV(f+12,101,76);
        for(UInt32 i=0;i<6;++i)p[i]=s.bssid[i];
        UInt32 flags=(s.secure?(5|(s.ap.wpaPresent?16:0)):1)|(s.ap.wmmPresent?2u:0)|(s.htEnabled?0x1000u:0)|(s.vhtEnabled?0x02000000u:0);
        // Match Linux ath10k_peer_assoc_h_crypto: the TLV peer PMF bit is
        // conditional on the reviewed firmware-file feature, not SERVICE_READY.
        if(s.pmfRequired && s.firmwareMFPSupport)flags|=0x08000000u;
        if(r.width>=40)flags|=0x2000;
        if(r.width==80)flags|=0x04000000;
        // SMPS flags have the same meaning as Linux ath10k_peer_assoc_h_ht.
        if(s.htEnabled){const UInt8 smps=s.peerSMPS!=255?s.peerSMPS:
                ((htcReadLE16(s.ap.ht)&12)==0?1:((htcReadLE16(s.ap.ht)&12)==4?2:0));
            if(smps==1)flags|=0x40000;else if(smps==2)flags|=0x20000;else flags|=0x200000;}
        htcWriteLE32(p+12,1);htcWriteLE32(p+16,s.aid);htcWriteLE32(p+20,flags);
        htcWriteLE32(p+24,s.ap.capabilities);htcWriteLE32(p+28,1);
        htcWriteLE32(p+48,r.nss);htcWriteLE32(p+56,peerPhyMode(s,r.width));
        if(s.htEnabled){
            // peer_ht_caps is the AP's HT CapInfo word.  Leaving it zero
            // makes firmware disable the AP's negotiated HT features even
            // when the association response contained a valid HT IE.
            htcWriteLE32(p+32,s.peerHT);
            // Linux ath10k_peer_assoc_h_vht: never shrink HT aggregate limit
            // for APs advertising a smaller VHT exponent.
            UInt32 exponent=s.ap.ht[2]&3;
            if(s.vhtEnabled){const UInt32 vhtExponent=(connLE32(s.ap.vht)>>23)&7;
                if(vhtExponent>exponent)exponent=vhtExponent;
                // peer_vht_caps describes the AP, not our local association IE.
                htcWriteLE32(p+52,s.peerVHT);}
            htcWriteLE32(p+36,(1u<<(13+exponent))-1);
            const UInt8 density[8]={0,1,1,1,2,4,8,16};htcWriteLE32(p+40,density[(s.ap.ht[2]>>2)&7]);
            htcWriteLE32(p+44,8|(r.width>=40?2:0)|(r.shortGI?4:0)|(r.nss>=2?1:0));htcWriteLE32(p+72,r.count);
        }
        htcWriteLE32(p+68,s.ap.rateCount);
        connTLV(f+92,17,static_cast<UInt16>(rates));
        for(UInt32 i=0;i<s.ap.rateCount;++i)f[96+i]=s.ap.rates[i]&127;
        connTLV(f+96+rates,17,static_cast<UInt16>(htRates));
        for(UInt32 i=0;i<r.count;++i)f[100+rates+i]=r.ht[i];
        UInt8 *v=f+100+rates+htRates;connTLV(v,102,16);
        if(s.vhtEnabled){htcWriteLE32(v+8,r.tx);htcWriteLE32(v+16,r.rx);}
        break;
    }
    case LinkStage::Authorize:
        cmd=0x6004;bytes=36;connTLV(f+12,100,20);
        for(UInt32 i=0;i<6;++i)p[4+i]=s.bssid[i];
        htcWriteLE32(p+12,3);htcWriteLE32(p+16,s.secure?0:1);break;
    case LinkStage::Up:
        cmd=0x5005;bytes=32;connTLV(f+12,92,16);htcWriteLE32(p+4,s.aid);
        for(UInt32 i=0;i<6;++i)p[8+i]=s.bssid[i];
        break;
    case LinkStage::Barrier:
        cmd=0x1d001;bytes=20;connTLV(f+12,149,4);htcWriteLE32(p,0xa7100047);break;
    default:return 0;
    }
    f[0]=endpoint;f[1]=1;f[5]=s.wmiSequence;htcWriteLE16(f+2,static_cast<UInt16>(bytes-8));htcWriteLE32(f+8,cmd);
    return bytes;
}
inline APConfig desiredAPConfig(const ConnectionState &s){
    APConfig c;c.wmmPresent=s.ap.wmmPresent;c.wmmCount=s.ap.wmmCount;
    for(UInt32 i=0;i<16;++i)c.wmm[i]=s.ap.wmm[i];
    c.slot=s.operatingChannel>14 || (s.ap.capabilities&0x400)?2:1;
    c.preamble=s.operatingChannel>14 || ((s.ap.capabilities&0x20) && !(s.ap.erpPresent && (s.ap.erp&4)))?2:1;
    c.protection=s.operatingChannel<=14 && s.ap.erpPresent && (s.ap.erp&2)?1:0;
    const UInt8 smps=(htcReadLE16(s.ap.ht)>>2)&3;
    c.smps=s.htEnabled?(s.peerSMPS!=255?s.peerSMPS:(smps==0?1:(smps==1?2:0))):0;
    c.nss=s.peerOperatingNSS?s.peerOperatingNSS:s.negotiatedNSS;
    if(!c.nss || c.nss>s.negotiatedNSS)c.nss=s.negotiatedNSS?s.negotiatedNSS:1;
    if(c.smps==1)c.nss=1;
    c.width=peerBandwidth(s);
    return c;
}
inline bool sameAPConfig(const APConfig &a,const APConfig &b){
    if(a.slot!=b.slot || a.preamble!=b.preamble || a.protection!=b.protection || a.smps!=b.smps ||
       a.nss!=b.nss || a.width!=b.width || a.wmmPresent!=b.wmmPresent || a.wmmCount!=b.wmmCount)return false;
    for(UInt32 i=0;i<16;++i)if(a.wmm[i]!=b.wmm[i])return false;
    return true;
}
inline void recordAPOperatingMode(ConnectionState &s,UInt8 mode){
    if(!s.vhtEnabled || (mode&0x88))return;
    const UInt8 nss=UInt8(((mode>>4)&7)+1);
    s.peerOperatingNSS=nss<s.negotiatedNSS?nss:s.negotiatedNSS;
    s.peerOperatingWidth=(mode&3)==0?20:((mode&3)==1?40:80);
    if(!s.maintenance.valid || !sameAPConfig(desiredAPConfig(s),s.maintenance.applied))s.maintenance.blocked=true;
}
inline UInt32 apMaintenanceCookie(const ConnectionState &s){return 0xa7840000u|(s.maintenance.generation&0xffff);}
inline UInt32 statsCookie(const ConnectionState &s){return 0xa7850000u|(s.stats.generation&0xffffu);}
inline UInt32 buildStatsRequest(UInt8 *f,UInt32 capacity,UInt8 endpoint,const ConnectionState &s){
    if(!f || capacity<256 || !endpoint || endpoint==255 || (s.stats.step!=1 && s.stats.step!=2))return 0;
    htcZero(f,256);const UInt32 n=s.stats.step==1?36:20;
    if(s.stats.step==1){connTLV(f+12,143,20);htcWriteLE32(f+16,1u|4u|8u);
        for(UInt32 i=0;i<6;++i)f[24+i]=s.bssid[i];}
    else {connTLV(f+12,149,4);htcWriteLE32(f+16,statsCookie(s));}
    f[0]=endpoint;f[1]=1;f[5]=s.wmiSequence;htcWriteLE16(f+2,n-8);htcWriteLE32(f+8,s.stats.step==1?0x16001:0x1d001);return n;
}
inline UInt32 buildAPMaintenance(UInt8 *f,UInt32 capacity,UInt8 endpoint,const ConnectionState &s){
    const auto &m=s.maintenance;const auto &t=m.target;
    if(!f || capacity<256 || !endpoint || endpoint==255 || !m.step || m.step>11)return 0;
    if(m.step==11){const UInt32 n=buildConnectionWMI(f,capacity,endpoint,s,LinkStage::PeerAssoc,t.nss,t.width);
        if(n)htcWriteLE32(f+28,0);
        return n;}
    htcZero(f,256);UInt32 bytes=28,cmd=0x5008;
    if(m.step==1){if(!t.wmmPresent)return 0;cmd=0x4006;bytes=136;writeWMMCommand(f,t.wmm);}
    else if(m.step==8){cmd=0x1d001;bytes=20;connTLV(f+12,149,4);htcWriteLE32(f+16,apMaintenanceCookie(s));}
    else if(m.step==6 || m.step==7 || m.step==9 || m.step==10){
        cmd=0x6004;bytes=36;connTLV(f+12,100,20);
        for(UInt32 i=0;i<6;++i)f[20+i]=s.bssid[i];
        htcWriteLE32(f+28,m.step==6?1:(m.step==7?5:(m.step==9?0xd:4)));
        htcWriteLE32(f+32,m.step==6?t.smps:(m.step==7?t.nss:(m.step==9?peerPhyMode(s,t.width):(t.width==80?2:(t.width==40?1:0)))));
    }else {connTLV(f+12,95,12);
        const UInt32 params[]={0,0,7,8,26,44};
        const UInt32 values[]={0,0,t.slot,t.preamble,t.protection,1u|(t.protection?0x20u:0x10u)};
        htcWriteLE32(f+20,params[m.step]);htcWriteLE32(f+24,values[m.step]);}
    f[0]=endpoint;f[1]=1;f[5]=s.wmiSequence;htcWriteLE16(f+2,bytes-8);htcWriteLE32(f+8,cmd);return bytes;
}
// Linux v6.12 TLV teardown: DOWN, key disable (cipher NONE), peer
// delete/unmap, STOP/stopped, then ECHO. CE consumption alone is not completion.
inline UInt32 buildDisconnectWMI(UInt8 *f,UInt32 capacity,UInt8 endpoint,const ConnectionState &s) {
    if(!f || capacity<256 || !endpoint || endpoint==255)return 0;
    htcZero(f,256);UInt32 bytes=20,cmd=0;auto *p=f+16;
    switch(s.disconnect){
    case DisconnectStage::Down:cmd=0x5007;connTLV(f+12,94,4);break;
    case DisconnectStage::ClearPair:
    case DisconnectStage::ClearGroup: {
        const bool group=s.disconnect==DisconnectStage::ClearGroup;
        if(!s.secure || s.groupIndex>3)return 0;
        cmd=0x5009;bytes=128;connTLV(f+12,96,92);
        for(unsigned i=0;i<6;++i)p[4+i]=s.bssid[i];
        htcWriteLE32(p+12,group?s.groupIndex:0);htcWriteLE32(p+16,group?1:0);
        // Linux retains key_len with key_data=NULL when disabling a CCMP key.
        htcWriteLE32(p+80,16);connTLV(f+108,17,16);break;
    }
    case DisconnectStage::DeletePeer:
        cmd=0x6002;bytes=28;connTLV(f+12,98,12);
        for(unsigned i=0;i<6;++i)p[4+i]=s.bssid[i];
        break;
    case DisconnectStage::Stop:cmd=0x5006;connTLV(f+12,93,4);break;
    case DisconnectStage::Barrier:
        cmd=0x1d001;connTLV(f+12,149,4);htcWriteLE32(p,0xa7590000u|((s.disconnectCount+1)&0xffff));break;
    default:return 0;
    }
    f[0]=endpoint;f[1]=1;f[5]=s.wmiSequence;htcWriteLE16(f+2,static_cast<UInt16>(bytes-8));htcWriteLE32(f+8,cmd);
    return bytes;
}
// INSTALL_KEY follows Linux ath10k's 92-byte fixed command. Pairwise uses
// 16-byte CCMP; group uses 16-byte CCMP or 32-byte TKIP (16+8+8 MIC keys).
inline UInt32 buildSecurityWMI(UInt8 *f,UInt32 capacity,UInt8 endpoint,const ConnectionState &s){
    if(!f || capacity<256 || !endpoint || !s.secure)return 0;
    htcZero(f,256);UInt32 bytes=0,cmd=0;UInt8 *p=f+16;
    if(s.securityPhase==1 || s.securityPhase==2){
        bool group=s.securityPhase==2;cmd=0x5009;bytes=128;connTLV(f+12,96,92);
        if(group && s.groupCipher!=AthSecurity::cipherCCMP && s.groupCipher!=AthSecurity::cipherTKIP)return 0;
        for(UInt32 i=0;i<6;++i)p[4+i]=s.bssid[i];
        htcWriteLE32(p+12,group?s.groupIndex:0);htcWriteLE32(p+16,group?1:0);
        const UInt32 keyLength=group?(s.groupCipher==AthSecurity::cipherTKIP?32:16):16;
        htcWriteLE32(p+20,group?s.groupCipher:AthSecurity::cipherCCMP);htcWriteLE32(p+80,keyLength);
        if(group && s.groupCipher==AthSecurity::cipherTKIP){htcWriteLE32(p+84,8);htcWriteLE32(p+88,8);}
        if(group)for(UInt32 i=0;i<6;++i)p[24+i]=s.groupRSC[i];
        connTLV(f+108,17,static_cast<UInt16>(keyLength));
        for(UInt32 i=0;i<keyLength;++i)f[112+i]=group?s.groupKey[i]:s.temporalKey[i];
        bytes=112+keyLength;
    }else if(s.securityPhase==3){
        cmd=0x6004;bytes=36;connTLV(f+12,100,20);
        for(UInt32 i=0;i<6;++i)p[4+i]=s.bssid[i];
        htcWriteLE32(p+12,3);htcWriteLE32(p+16,1);
    }else if(s.securityPhase==4){cmd=0x1d001;bytes=20;connTLV(f+12,149,4);htcWriteLE32(p,0xa7100057);}
    if(!bytes)return 0;
    f[0]=endpoint;f[1]=1;f[5]=s.wmiSequence;htcWriteLE16(f+2,static_cast<UInt16>(bytes-8));htcWriteLE32(f+8,cmd);
    return bytes;
}
inline UInt32 buildStationManagement(UInt8 *f,UInt32 capacity,const ConnectionState &s,
    const UInt8 *saeRSN=nullptr,size_t saeRSNBytes=0) {
    const bool spectrum=(s.ap.capabilities&0x100)!=0;
    if(s.securityAKM==8 && s.stage==LinkStage::Auth)return 0; // Never encode open auth for SAE.
    if(!f || capacity<(s.htEnabled || spectrum?256u:128u) || s.ssidLength>32 || s.ap.rateCount>32 || !s.nss || s.nss>2 || (s.stage!=LinkStage::Auth && s.stage!=LinkStage::Assoc))return 0;
    htcZero(f,s.htEnabled || spectrum?256:128);f[0]=s.stage==LinkStage::Auth?0xb0:0;
    for(UInt32 i=0;i<6;++i){f[4+i]=s.bssid[i];f[10+i]=s.local[i];f[16+i]=s.bssid[i];}
    htcWriteLE16(f+22,static_cast<UInt16>((s.sequence&4095)<<4));
    if(s.stage==LinkStage::Auth){htcWriteLE16(f+26,1);return 30;}
    htcWriteLE16(f+24,static_cast<UInt16>(1|(s.ap.capabilities&0x420)|(s.secure?0x10:0)|(spectrum?0x100:0)));htcWriteLE16(f+26,1);
    UInt32 n=28;f[n++]=0;f[n++]=s.ssidLength;
    for(UInt32 i=0;i<s.ssidLength;++i)f[n++]=s.ssid[i];
    if(spectrum){
        f[n++]=33;f[n++]=2;f[n++]=0;f[n++]=s.txPowerHalfDbm/2;
        const UInt32 start=n;f[n++]=36;f[n++]=0;
        for(const auto &ch:kWMIScanPlan){
            if((ch.channel<=14)!=(s.operatingChannel<=14) || !regulatoryPower(s.regulatory,ch.channel,ch.channel,20))continue;
            if(n+2>capacity)return 0;
            f[n++]=static_cast<UInt8>(ch.channel);f[n++]=1;
        }
        f[start+1]=static_cast<UInt8>(n-start-2);
    }
    size_t stationRSNLength=0;const UInt8 *stationRSN=nullptr;
    if(s.securityAKM==8){
        if(!s.secure || !s.pmfRequired || s.groupCipher!=AthSecurity::cipherCCMP ||
           !AthSecurity::validSAEStationRSN(saeRSN,saeRSNBytes))return 0;
        stationRSN=saeRSN;stationRSNLength=saeRSNBytes;
    }else {
        if(saeRSN || saeRSNBytes)return 0;
        if(s.secure)stationRSN=AthSecurity::stationRSNForGroup(s.groupCipher,stationRSNLength,s.securityAKM);
    }
    if(s.secure&&!stationRSN)return 0;
    const UInt32 tail=2+s.ap.rateCount+(s.ap.rateCount>8?2:0)+(s.secure?stationRSNLength:0)+
        (s.ap.wmmPresent?(s.ap.wmmVendor?9:3):0)+(s.htEnabled?28:0)+(s.vhtEnabled?17:0);
    if(tail>capacity-n)return 0;
    UInt32 first=s.ap.rateCount<8?s.ap.rateCount:8;f[n++]=1;f[n++]=static_cast<UInt8>(first);
    for(UInt32 i=0;i<first;++i)f[n++]=s.ap.rates[i];
    if(s.ap.rateCount>8){f[n++]=50;f[n++]=s.ap.rateCount-8;
        for(UInt32 i=8;i<s.ap.rateCount;++i)f[n++]=s.ap.rates[i];}
    if(s.secure){for(size_t i=0;i<stationRSNLength;++i)f[n++]=stationRSN[i];}
    if(s.ap.wmmPresent){
        if(s.ap.wmmVendor){const UInt8 wmm[]={221,7,0,0x50,0xf2,2,0,1,0};for(UInt8 b:wmm)f[n++]=b;}
        else {f[n++]=46;f[n++]=1;f[n++]=0;}}
    if(s.htEnabled){
        const UInt32 htBytes=writeLocalHTCapabilityIE(f+n,capacity-n,s.localHT,s.negotiatedNSS);
        if(!htBytes)return 0;n+=htBytes;
        if(s.vhtEnabled){
            const UInt32 vhtBytes=writeLocalVHTCapabilityIE(f+n,capacity-n,s.localVHT,s.vhtRxMap,s.vhtTxMap);
            if(!vhtBytes)return 0;n+=vhtBytes;
            // Operating Mode Notification: selected width and receive NSS.
            f[n++]=199;f[n++]=1;f[n++]=static_cast<UInt8>((s.negotiatedWidth==80?2:(s.negotiatedWidth==40?1:0))|((s.negotiatedNSS-1)<<4));}
    }
    return n;
}
// Same-band CSA preserves the peer and keys. Width changes update PHY before BW,
// followed by peer rates with new_assoc=0, as in Linux ath10k_sta_rc_update_wk.
inline void receiveAPOperatingMode(ConnectionState &s,UInt8 mode);
inline UInt16 operatingClassWidth(UInt8 op,UInt8 primary){
    auto range=[&](UInt8 first,UInt8 last,UInt8 step){return primary>=first && primary<=last && (primary-first)%step==0;};
    switch(op){
    case 81:return range(1,13,1)?20:0;
    case 83:return range(1,9,1)?40:0;case 84:return range(5,13,1)?40:0;
    case 115:return range(36,48,4)?20:0;case 116:return range(36,44,8)?40:0;case 117:return range(40,48,8)?40:0;
    case 118:return range(52,64,4)?20:0;case 119:return range(52,60,8)?40:0;case 120:return range(56,64,8)?40:0;
    case 121:return range(100,144,4)?20:0;case 122:return range(100,140,8)?40:0;case 123:return range(104,144,8)?40:0;
    case 124:return range(149,161,4)?20:0;case 125:return range(149,177,4)?20:0;
    case 126:return range(149,173,8)?40:0;case 127:return range(153,177,8)?40:0;
    case 128:case 129:case 130:
        if(range(36,64,4) || range(100,144,4) || range(149,177,4))return op==128?80:(op==129?160:161);
        return 0;
    default:return 0;
    }
}
inline bool queueSpectrumReport(ConnectionState &s,const UInt8 *f,UInt32 bytes){
    if(s.stage!=LinkStage::Connected || !s.portAuthorized || !(s.ap.capabilities&0x100) ||
       bytes<29 || f[24]!=0 || (f[25]!=0 && f[25]!=2))return false;
    UInt32 count=0;const bool tpc=f[25]==2;
    // Validate the whole request before committing a response; no partial replies.
    for(UInt32 i=27;i<bytes;){
        if(bytes-i<2 || f[i+1]>bytes-i-2)return false;
        const UInt8 id=f[i],len=f[i+1];
        if(id==(tpc?34:38)){
            if(tpc?len!=0:(len<3 || (f[i+4]<=2 && len!=14)))return false;
            if(++count>(tpc?1u:45u))return false;
        }
        i+=2+len;
    }
    if(!count)return false;
    auto &r=s.spectrum;++r.received;
    if(r.pending || r.inFlight){++r.dropped;return false;}
    htcZero(r.frame,sizeof(r.frame));r.frame[0]=0xd0;
    for(UInt32 i=0;i<6;++i){r.frame[4+i]=s.bssid[i];r.frame[10+i]=s.local[i];r.frame[16+i]=s.bssid[i];}
    r.frame[24]=0;r.frame[25]=tpc?3:1;r.frame[26]=f[26];UInt32 n=27;
    if(tpc){
        r.frame[n++]=35;r.frame[n++]=2;r.frame[n++]=s.radio.appliedPower/2;
        r.frame[n++]=0; // No calibrated receive sensitivity is available: no invented link margin.
    }else for(UInt32 i=27;i<bytes;i+=2+f[i+1])if(f[i]==38){
        r.frame[n++]=39;r.frame[n++]=3;r.frame[n++]=f[i+2];
        // Match mac80211's negative report. Unsupported measurement types are Incapable.
        r.frame[n++]=f[i+4]<=2?4:2;r.frame[n++]=f[i+4];
    }
    r.length=UInt16(n);r.pending=true;r.sinceMs=0;return true;
}
inline bool channelSwitchIEs(ConnectionState &s,const UInt8 *ies,UInt32 bytes,bool action=false,UInt32 rxChannel=0,bool probeResponse=false){
    bool seen=false,secondarySeen=false,wideSeen=false,dsSeen=false,htSeen=false;
    UInt8 primary=0,count=0,mode=0,secondary=0,wide=0,center=0,advertised=0,opClass=0,constraint=0;
    bool constraintSeen=false,timSeen=false;UInt8 dtimCount=0,dtimPeriod=0;
    UInt8 htOffset=0,vhtWidth=0,vhtCenter=0,vhtSecond=0,wideSecond=0;bool vhtOperationSeen=false;
    const UInt8 *country=nullptr,*targetCountry=nullptr,*htOperation=nullptr,*htCapabilities=nullptr;
    UInt8 targetCountryLength=0;bool wrapperSeen=false;
    UInt8 countryLength=0,wmm[16]{},wmmCount=0,erp=0;
    bool wmmSeen=false,edcaSeen=false,erpSeen=false,operatingModeSeen=false;UInt8 operatingMode=0;
    auto parse=[&](const UInt8 *p,UInt32 n,bool nested)->bool {
        for(UInt32 i=0;i<n;){
            if(n-i<2 || p[i+1]>n-i-2)return false;
            const UInt8 id=p[i],len=p[i+1];const UInt8 *v=p+i+2;i+=2+len;
            if(id==37 || id==60){
                if(nested || len!=(id==37?3:4) || v[0]>1)return false;
                const UInt8 ch=v[id==37?1:2],ct=v[id==37?2:3];
                if(seen && (primary!=ch || count!=ct || mode!=v[0]))return false;
                if(id==60){if(opClass && opClass!=v[1])return false;opClass=v[1];if(!opClass)return false;}
                seen=true;primary=ch;count=ct;mode=v[0];
            }else if(id==5){
                if(nested || timSeen || len<4 || !v[1] || v[0]>=v[1])return false;
                timSeen=true;dtimCount=v[0];dtimPeriod=v[1];
            }else if(id==32){
                if(nested || len!=1 || constraintSeen)return false;
                constraintSeen=true;constraint=v[0];
            }else if(id==7){
                if(!validAPCountry(v,len))return false;
                if(nested){if(targetCountry)return false;targetCountry=v;targetCountryLength=len;}
                else {if(country)return false;country=v;countryLength=len;}
            }else if(id==42){
                if(nested || erpSeen || len!=1)return false;
                erpSeen=true;erp=v[0];
            }else if(id==45){
                if(nested || htCapabilities || len!=26)return false;
                htCapabilities=v;
            }else if(id==12){
                UInt8 ac[16]{},counter=0;
                if(nested || edcaSeen || !parseAPEDCA(v,len,ac,counter))return false;
                edcaSeen=true;if(!wmmSeen){wmmCount=counter;for(UInt32 j=0;j<16;++j)wmm[j]=ac[j];}
            }else if(id==199){
                if(nested || operatingModeSeen || len!=1)return false;
                operatingModeSeen=true;operatingMode=v[0];
            }else if(id==221 && len>=6 && v[0]==0 && v[1]==0x50 && v[2]==0xf2 && v[3]==2 && v[4]==1 && v[5]==1){
                if(nested || wmmSeen || !parseAPWMM(v,len,wmm,wmmCount))return false;
                wmmSeen=true;
            }else if(id==62){
                if(len!=1 || (v[0]!=0 && v[0]!=1 && v[0]!=3) || (secondarySeen && secondary!=v[0]))return false;
                secondarySeen=true;secondary=v[0];
            }else if(id==194){
                if(len!=3 || v[0]>3 || (wideSeen && (wide!=v[0] || center!=v[1] || wideSecond!=v[2])))return false;
                wideSeen=true;wide=v[0];center=v[1];wideSecond=v[2];
            }else if(id==196 && nested)return false;
            else if(id==3){if(nested || len!=1 || dsSeen || (htSeen && advertised!=v[0]))return false;dsSeen=true;advertised=v[0];}
            else if(id==61){if(nested || len!=22 || htSeen || (dsSeen && advertised!=v[0]))return false;htSeen=true;htOperation=v;advertised=v[0];htOffset=(v[1]&4)?(v[1]&3):0;if(htOffset==2)return false;}
            else if(id==192){if(nested || len!=5 || vhtOperationSeen)return false;vhtOperationSeen=true;vhtWidth=v[0];vhtCenter=v[1];vhtSecond=v[2];}
        }
        return true;
    };
    // One bounded wrapper level; no target pointer traversal or heap allocation.
    for(UInt32 i=0;i<bytes;){
        if(bytes-i<2 || ies[i+1]>bytes-i-2)return false;
        if(ies[i]==196){if(wrapperSeen || !parse(ies+i+2,ies[i+1],true))return false;wrapperSeen=true;}
        i+=2+ies[i+1];
    }
    if(!parse(ies,bytes,false))return false;
    if(targetCountry && !seen)return false;
    if(htCapabilities && ((htcReadLE16(htCapabilities)>>2)&3)==2)return false;
    if(wideSeen && !wide && wideSecond)return false;
    if(vhtOperationSeen){UInt8 usable=0;
        if(!vhtPrimary80(advertised,vhtWidth,vhtCenter,vhtSecond,usable) ||
           (vhtWidth && (!htSeen || !htOffset)))return false;
        if(vhtWidth){vhtWidth=1;vhtCenter=usable;vhtSecond=0;}}
    const int wideDifference=int(center)-int(wideSecond);
    const UInt16 fullWide=wide==2?160:(wide==3?161:(wide==1?(wideSecond?((wideDifference==8 || wideDifference==-8)?160:161):80):0));
    if(wideSeen && wide){UInt8 usable=0;if(!seen || !vhtPrimary80(primary,wide,center,wideSecond,usable))return false;wide=1;center=usable;}
    if(probeResponse)return true; // Liveness only; absent IEs must not relax the current AP power limit.
    if(!action && rxChannel==s.operatingChannel && timSeen){s.ap.timPresent=true;s.ap.dtimCount=dtimCount;s.ap.dtimPeriod=dtimPeriod;}
    // AP bandwidth can change without moving the primary channel or issuing CSA.
    if(!seen && !action && htSeen && advertised==s.operatingChannel && rxChannel==s.operatingChannel &&
       s.stage==LinkStage::Connected && s.portAuthorized && s.csa.phase==ChannelSwitchPhase::Idle){
        UInt8 announced=(htOffset==1 || htOffset==3)?40:20;
        if(vhtOperationSeen && vhtWidth==1)announced=80;
        if(announced>s.negotiatedWidth)announced=s.negotiatedWidth;
        if(announced!=s.width){seen=true;primary=static_cast<UInt8>(s.operatingChannel);mode=1;count=0;
            secondarySeen=true;secondary=announced>=40?htOffset:0;wideSeen=true;wide=announced==80?1:0;center=announced==80?vhtCenter:0;}
    }
    if(!action && s.stage==LinkStage::Connected && rxChannel==s.operatingChannel)
        {if(constraintSeen)s.powerConstraintDb=constraint;
        // A wrapper describes the destination. Missing Country during switch
        // confirmation cannot clear the destination constraint just committed.
        if(country){
            s.ap.countryPresent=true;s.ap.countryLength=countryLength;
            for(UInt32 i=0;i<countryLength;++i)s.ap.country[i]=country[i];}
        s.ap.erpPresent=erpSeen;s.ap.erp=erp;
        if(htOperation)for(UInt32 i=0;i<22;++i)s.ap.htOp[i]=htOperation[i];
        if(vhtOperationSeen){s.ap.vhtOp[0]=vhtWidth;s.ap.vhtOp[1]=vhtCenter;s.ap.vhtOp[2]=vhtSecond;}
        if(htCapabilities){
            if((htcReadLE16(htCapabilities)^htcReadLE16(s.ap.ht))&12)s.peerSMPS=255;
            s.ap.ht[0]=htCapabilities[0];s.ap.ht[1]=htCapabilities[1];}
        if((wmmSeen || (edcaSeen && !s.ap.wmmVendor)) && s.ap.wmmPresent){s.ap.wmmCount=wmmCount;for(UInt32 i=0;i<16;++i)s.ap.wmm[i]=wmm[i];}
        if(!s.maintenance.valid || !sameAPConfig(desiredAPConfig(s),s.maintenance.applied))s.maintenance.blocked=true;
        s.radio.powerBlocked=stationOperationalPowerLimit(s)<s.radio.appliedPower;}
    const UInt8 apWidth=vhtOperationSeen && vhtWidth==1?80:((htOffset==1 || htOffset==3)?40:20);
    UInt8 beaconWidth=apWidth>s.negotiatedWidth?s.negotiatedWidth:apWidth;
    const UInt8 beaconCenter=beaconWidth==80?vhtCenter:(beaconWidth==40?UInt8(advertised+(htOffset==1?2:-2)):advertised);
    if(!action && (dsSeen || htSeen) && advertised==s.csa.primary && rxChannel==s.csa.primary &&
       s.csa.phase>=ChannelSwitchPhase::Up && (htSeen?(beaconWidth==s.csa.width && beaconCenter==s.csa.center):(!s.csa.widthChanged && s.csa.width==20)))s.csa.confirmed=true;
    if(!seen){if(operatingModeSeen && !action && rxChannel==s.operatingChannel)receiveAPOperatingMode(s,operatingMode);return true;}
    if(s.stage!=LinkStage::Connected || !s.portAuthorized || s.disconnect!=DisconnectStage::None)return true;
    auto reject=[&](){++s.csa.rejected;s.csa.status="unsupported-or-forbidden-channel-switch";
        s.csa.blocked=true;s.error=s.csa.status;s.cancelRequested=true;return true;};
    UInt8 width=s.width,targetCenter=primary;
    if(wideSeen){width=wide?80:(secondarySeen && secondary?40:20);targetCenter=center;
        if(!wide){targetCenter=width==40?static_cast<UInt8>(primary+(secondary==1?2:-2)):primary;}}
    else if(secondarySeen){width=secondary?40:20;targetCenter=width==40?static_cast<UInt8>(primary+(secondary==1?2:-2)):primary;}
    else if(width==80){const UInt8 centers[]={42,58,106,122,138,155};targetCenter=0;
        for(UInt8 ch:centers)if(primary==ch-6 || primary==ch-2 || primary==ch+2 || primary==ch+6)targetCenter=ch;}
    else if(width==40){
        if(s.operatingChannel<=13)targetCenter=static_cast<UInt8>(primary+(s.centerChannel>s.operatingChannel?2:-2));
        else {const UInt8 pairs[]={36,44,52,60,100,108,116,124,132,140,149,157};targetCenter=0;
            for(UInt8 low:pairs)if(primary==low || primary==low+4)targetCenter=low+2;}
    }
    if(opClass){
        const UInt16 classWidth=operatingClassWidth(opClass,primary);
        if(!classWidth || (opClass<=84)!=(primary<=13))return reject();
        if(wideSeen && (fullWide?fullWide:(secondarySeen && secondary?40:20))!=classWidth)return reject();
        if(classWidth==20){if((wideSeen && fullWide) || (secondarySeen && secondary))return reject();width=20;targetCenter=primary;}
        else if(classWidth==40){
            const bool above=opClass==83 || opClass==116 || opClass==119 || opClass==122 || opClass==126;
            if((wideSeen && fullWide) || (secondarySeen && secondary!=(above?1:3)))return reject();
            width=40;targetCenter=UInt8(primary+(above?2:-2));
        }else if(!wideSeen){
            if(classWidth==160){
                // Global class 129 defines contiguous 160; validate both 80 segments.
                const UInt8 full=primary<100?50:(primary<149?114:163);
                if(!vhtPrimary80(primary,2,full,0,targetCenter))return reject();
                width=80;
            }else if(classWidth==80){
                const UInt8 centers[]={42,58,106,122,138,155};targetCenter=0;for(UInt8 c:centers)
                    if(channelDefinitionAllowed(primary,c,80,true))targetCenter=c;
                if(!targetCenter)return reject();
                width=80;
            }else {
                // Class 130 alone cannot identify the second 80 segment. As
                // mac80211, retain only explicit HT geometry, or use HT20.
                width=secondarySeen && secondary?40:20;
                targetCenter=width==40?UInt8(primary+(secondary==1?2:-2)):primary;
            }
        }
        if(width==80 && secondarySeen && secondary &&
           secondary!=(((int(primary)+6-int(targetCenter))%8)==0?1:3))return reject();
        if(width>s.negotiatedWidth){
            if(s.negotiatedWidth==40 && width==80){targetCenter=UInt8(primary+(((int(primary)+6-int(targetCenter))%8)==0?2:-2));width=40;}
            else {width=20;targetCenter=primary;}
        }
    }
    UInt8 power=regulatoryPower(s.regulatory,primary,targetCenter,width);
    if((primary<=14)!=(s.operatingChannel<=14) || width>s.negotiatedWidth || (width>=40 && !s.htEnabled) || (width==80 && !s.vhtEnabled) ||
       !channelDefinitionAllowed(primary,targetCenter,width,s.regulatory.stationDFS) || !power)return reject();
    if(s.rekeyKind)return reject();
    const UInt8 targetLimit=stationPowerLimitFor(s,primary,targetCenter,width,targetCountry,targetCountryLength);
    if(!targetLimit)return reject();
    if(targetLimit<power)power=targetLimit;
    // A beacon may carry both CSA and a new receive NSS. Apply that peer
    // restriction as well; the CSA geometry remains the announced target.
    if(operatingModeSeen && !action)recordAPOperatingMode(s,operatingMode);
    if(s.txPowerHalfDbm<power)power=s.txPowerHalfDbm;
    if(primary==s.operatingChannel && targetCenter==s.centerChannel && width==s.width && s.csa.phase==ChannelSwitchPhase::Idle)return true;
    if(s.csa.phase>ChannelSwitchPhase::Announced){
        if(primary!=s.csa.primary || targetCenter!=s.csa.center || width!=s.csa.width)return reject();
        if(targetCountry){if(!s.csa.countryPresent || targetCountryLength!=s.csa.countryLength)return reject();
            for(UInt32 i=0;i<targetCountryLength;++i)if(targetCountry[i]!=s.csa.country[i])return reject();}
        return true;
    }
    if(s.csa.phase==ChannelSwitchPhase::Idle){
        ++s.csa.generation;s.csa.phase=ChannelSwitchPhase::Announced;
        s.csa.primary=primary;s.csa.center=targetCenter;s.csa.width=width;s.csa.power=power;
        s.csa.widthChanged=width!=s.width;
        s.csa.count=count;s.csa.mode=mode;s.csa.armedCount=255;s.csa.dueMs=0;
        s.csa.submitted=s.csa.restartReply=s.csa.failed=s.csa.confirmed=false;s.csa.status="announced";
        s.csa.countryPresent=false;s.csa.countryLength=0;
    }else {
        if(primary!=s.csa.primary || targetCenter!=s.csa.center || width!=s.csa.width)return reject();
        if(count<s.csa.count)s.csa.count=count;
        if(mode)s.csa.mode=1; // Replayed beacons must never postpone or unpause.
    }
    if(targetCountry){
        if(s.csa.countryPresent){if(targetCountryLength!=s.csa.countryLength)return reject();
            for(UInt32 i=0;i<targetCountryLength;++i)if(targetCountry[i]!=s.csa.country[i])return reject();}
        else {s.csa.countryPresent=true;s.csa.countryLength=targetCountryLength;
            for(UInt32 i=0;i<targetCountryLength;++i)s.csa.country[i]=targetCountry[i];}}
    if(power<s.csa.power)s.csa.power=power;
    if(s.csa.mode || !s.csa.count)s.csa.blocked=true;
    return true;
}
inline UInt32 channelSwitchCookie(const ConnectionState &s){return 0xa75c0000u|(s.csa.generation&0xffffu);}
inline void receiveAPOperatingMode(ConnectionState &s,UInt8 mode){
    if(s.stage!=LinkStage::Connected || !s.portAuthorized)return;
    // OM limits the peer's receiver; the BSS channel definition is unchanged.
    recordAPOperatingMode(s,mode);
}
inline UInt32 buildChannelSwitchWMI(UInt8 *f,UInt32 capacity,UInt8 endpoint,ConnectionState &s){
    if(s.csa.phase==ChannelSwitchPhase::PeerRates){
        const auto stage=s.stage;s.stage=LinkStage::PeerAssoc;
        const UInt32 n=buildConnectionWMI(f,capacity,endpoint,s);s.stage=stage;
        if(n)htcWriteLE32(f+28,0); // peer_new_assoc=false; preserve security/replay state.
        return n;
    }
    if(s.csa.phase==ChannelSwitchPhase::Restart){
        const auto stage=s.stage;const UInt32 primary=s.operatingChannel;
        const UInt8 center=s.centerChannel,width=s.width,power=s.txPowerHalfDbm;
        s.stage=LinkStage::Start;s.operatingChannel=s.csa.primary;s.centerChannel=s.csa.center;
        s.width=s.csa.width;s.txPowerHalfDbm=s.csa.power;
        const UInt32 n=buildConnectionWMI(f,capacity,endpoint,s);
        s.stage=stage;s.operatingChannel=primary;s.centerChannel=center;s.width=width;s.txPowerHalfDbm=power;
        if(n)htcWriteLE32(f+8,0x5004);
        return n;
    }
    if(!f || capacity<256 || !endpoint || endpoint==255)return 0;
    htcZero(f,256);UInt32 n=20,cmd=0;
    if(s.csa.phase==ChannelSwitchPhase::Down){cmd=0x5007;connTLV(f+12,94,4);}
    else if(s.csa.phase==ChannelSwitchPhase::PeerPhy || s.csa.phase==ChannelSwitchPhase::PeerWidth){
        cmd=0x6004;n=36;connTLV(f+12,100,20);for(UInt32 i=0;i<6;++i)f[20+i]=s.bssid[i];
        htcWriteLE32(f+28,s.csa.phase==ChannelSwitchPhase::PeerPhy?0xd:4);
        const UInt8 width=peerBandwidth(s);
        htcWriteLE32(f+32,s.csa.phase==ChannelSwitchPhase::PeerPhy?peerPhyMode(s,width):(width==80?2:(width==40?1:0)));
    }
    else if(s.csa.phase==ChannelSwitchPhase::Up){cmd=0x5005;n=32;connTLV(f+12,92,16);
        htcWriteLE32(f+20,s.aid);for(UInt32 i=0;i<6;++i)f[24+i]=s.bssid[i];}
    else if(s.csa.phase==ChannelSwitchPhase::Barrier){cmd=0x1d001;connTLV(f+12,149,4);htcWriteLE32(f+16,channelSwitchCookie(s));}
    else return 0;
    f[0]=endpoint;f[1]=1;f[5]=s.wmiSequence;htcWriteLE16(f+2,n-8);htcWriteLE32(f+8,cmd);return n;
}
enum class ManagementAuthentication : UInt8 {Unverified,Pairwise,Group};
// Authentication is an internal dispatch assertion, never derived from WMI
// status or Protected alone. Native callers must first commit a PMF result.
inline void receiveStationManagementAuthenticated(ConnectionState &s,const UInt8 *f,UInt32 bytes,UInt32 status,UInt32 rxChannel,ManagementAuthentication authentication) {
    if(!f || s.disconnect!=DisconnectStage::None || s.stage<LinkStage::Auth || s.stage==LinkStage::Failed || status&0x39u || bytes<24)return;
    UInt16 fc=htcReadLE16(f);
    if(authentication!=ManagementAuthentication::Unverified && authentication!=ManagementAuthentication::Pairwise && authentication!=ManagementAuthentication::Group)return;
    const UInt16 forbidden=authentication==ManagementAuthentication::Pairwise?0x070fu:0x470fu;
    if((fc&forbidden) || (htcReadLE16(f+22)&15) || !sameMAC(f+10,s.bssid) || !sameMAC(f+16,s.bssid))return;
    if(authentication==ManagementAuthentication::Pairwise && (!(fc&0x4000) || !sameMAC(f+4,s.local)))return;
    if(authentication==ManagementAuthentication::Group && (!(f[4]&1) || !AthSecurity::robustManagementFrame(f,bytes)))return;
    // Raw WMI input cannot bypass the retained PMF verifier after PTK setup.
    if(authentication==ManagementAuthentication::Unverified && s.pmfRequired && s.pairInstalled && AthSecurity::robustManagementFrame(f,bytes))return;
    bool broadcast=true;for(UInt32 i=0;i<6;++i)if(f[4+i]!=255)broadcast=false;
    if(!sameMAC(f+4,s.local) && !broadcast && authentication!=ManagementAuthentication::Group)return;
    const UInt32 subtype=fc&0xf0u;
    if((subtype==0xc0 || subtype==0xa0) && bytes>=26){
        ++s.diagnostics.mlmeCount;s.diagnostics.mlmeSubtype=static_cast<UInt8>(subtype);
        s.diagnostics.mlmeReason=htcReadLE16(f+24);s.diagnostics.mlmeChannel=rxChannel;
        if(!s.cancelRequested && !s.apDisconnect.valid){
            s.apDisconnect.valid=true;s.apDisconnect.disassociation=subtype==0xa0;
            s.apDisconnect.reason=htcReadLE16(f+24);
            for(UInt32 i=0;i<6;++i)s.apDisconnect.bssid[i]=s.bssid[i];
        }
        connectionFailure(s,"ap-disconnected");s.portAuthorized=false;s.cancelRequested=true;return;
    }
    if((subtype==0x80 || (subtype==0x50 && s.probe.active)) && bytes>=36){
        if(!channelSwitchIEs(s,f+36,bytes-36,false,rxChannel,subtype==0x50))return;
        if(rxChannel==s.operatingChannel){++s.beaconSerial;
            if(subtype==0x80){
                if(s.stage==LinkStage::Connected && bytes<=sizeof(s.beaconFrame)){
                    for(UInt32 i=0;i<bytes;++i)s.beaconFrame[i]=f[i];
                    s.beaconFrameBytes=bytes;++s.beaconFrameSerial;
                }
                if(htcReadLE16(f+32))s.ap.beaconInterval=htcReadLE16(f+32);
                s.ap.capabilities=htcReadLE16(f+34);
                if(!s.maintenance.valid || !sameAPConfig(desiredAPConfig(s),s.maintenance.applied))s.maintenance.blocked=true;}}
    }
    if(subtype==0xd0 && bytes>=26 && f[24]==0 && f[25]==4){
        channelSwitchIEs(s,f+26,bytes-26,true);return;
    }
    if(subtype==0xd0 && bytes>=26 && f[24]==0 && (f[25]==0 || f[25]==2)){
        queueSpectrumReport(s,f,bytes);return;
    }
    if(subtype==0xd0 && s.stage==LinkStage::Connected && bytes==27){
        if(f[24]==7 && f[25]==1 && s.htEnabled && !(f[26]&0xfc) && f[26]!=2){
            s.peerSMPS=!(f[26]&1)?0:((f[26]&2)?2:1);s.maintenance.blocked=true;return;}
        if(f[24]==21 && f[25]==2){receiveAPOperatingMode(s,f[26]);return;}
    }
    // Extended Channel Switch Announcement action (Public category, action 4).
    if(subtype==0xd0 && bytes>=30 && f[24]==4 && f[25]==4){
        if(bytes>280)return;
        UInt8 ecsa[256]={60,4,f[26],f[27],f[28],f[29]};
        for(UInt32 i=30;i<bytes;++i)ecsa[6+i-30]=f[i];
        channelSwitchIEs(s,ecsa,6+bytes-30,true);return;
    }
    if(broadcast)return;
    if(subtype==0xd0 && s.htEnabled && bytes>=26 && f[24]==3){
        // Firmware owns ADDBA/DELBA exchanges. Observe SSN for the fallback
        // host reorder path; never send a competing response from the host.
        if(f[25]==0 && bytes>=33){const UInt16 params=htcReadLE16(f+27);
            const UInt8 tid=(params>>2)&15;const UInt16 window=params>>6;
            if((params&2) && window && window<=64){s.baSSN[tid]=htcReadLE16(f+31)>>4;s.baSSNValid[tid]=true;}}
        if(f[25]==2 && bytes>=30){const UInt8 tid=htcReadLE16(f+26)>>12;s.baSSNValid[tid]=false;}
        return;
    }
    if(subtype==0xb0 && s.stage==LinkStage::Auth && s.submitted && bytes>=30){
        if(htcReadLE16(f+24)!=0 || htcReadLE16(f+26)!=2)return;
        s.associationStatus=htcReadLE16(f+28);
        if(s.associationStatus){s.error="authentication-rejected";s.stage=LinkStage::Failed;}
        else s.authReply=true;
    }
    if(subtype==0x10 && s.stage==LinkStage::Assoc && s.submitted && bytes>=30){
        s.associationStatus=htcReadLE16(f+26);
        UInt16 aid=htcReadLE16(f+28)&0x3fff;
        if(s.associationStatus || aid==0 || aid>2007){s.error="association-rejected";s.stage=LinkStage::Failed;return;}
        // Validate all response IEs before accepting the AID.
        for(UInt32 p=30;p<bytes;){if(bytes-p<2 || f[p+1]>bytes-p-2)return;p+=2+f[p+1];}
        bool ratesSeen=false,extendedSeen=false,rsnSeen=false,responseEDCA=false,responseWMM=false,responseOM=false;
        UInt8 operatingMode=0;
        UInt8 responseRates[32]{},responseAC[16]{},responseCount=0;UInt32 rateCount=0;
        for(UInt32 p=30;p<bytes;p+=2+f[p+1]){
            const UInt32 id=f[p],len=f[p+1];const UInt8 *v=f+p+2;
            if(id==1 || id==50){
                if((id==1 && (ratesSeen || !len || len>8)) || (id==50 && (extendedSeen || !len)) || len>32-rateCount)return;
                if(id==1)ratesSeen=true;else extendedSeen=true;
                for(UInt32 i=0;i<len;++i){
                    if(!supportedLegacyRate(v[i],s.operatingChannel>14)){
                        if(v[i]&128){s.error="association-unsupported-basic-rate";s.stage=LinkStage::Failed;return;}continue;}
                    responseRates[rateCount++]=v[i];}
            }
            if(id==48){if(rsnSeen || (s.secure && (!connectionRSNAccepted(s,f+p,len+2) || AthSecurity::wpa2GroupCipher(f+p,len+2)!=s.groupCipher)))return;rsnSeen=true;}
            if(id==199){if(responseOM || len!=1)return;responseOM=true;operatingMode=v[0];}
            if(id==12){UInt8 ac[16]{},counter=0;
                if(responseEDCA || !parseAPEDCA(v,len,ac,counter))return;
                responseEDCA=true;if(!responseWMM){responseCount=counter;for(UInt32 i=0;i<16;++i)responseAC[i]=ac[i];}}
            if(id==221 && len>=6 && v[0]==0 && v[1]==0x50 && v[2]==0xf2 && v[3]==2 && v[4]==1 && v[5]==1){
                if(responseWMM || !parseAPWMM(v,len,responseAC,responseCount))return;
                responseWMM=true;}
        }
        if(ratesSeen || extendedSeen){if(!rateCount)return;s.ap.rateCount=static_cast<UInt8>(rateCount);
            for(UInt32 i=0;i<rateCount;++i)s.ap.rates[i]=responseRates[i];}
        if(s.htEnabled){
            const bool wantedVHT=s.vhtEnabled;const UInt16 offeredVHT=s.vhtMap;
            const UInt16 offeredRx=s.vhtRxMap,offeredTx=s.vhtTxMap,offeredHT=s.localHT;
            const UInt32 offeredLocalVHT=s.localVHT;
            const UInt8 offeredWidth=s.width,offeredCenter=s.centerChannel,offeredNSS=s.negotiatedNSS;
            UInt16 offeredHTMask=0;
            for(UInt32 i=0;i<s.htRateCount;++i)if(s.htRates[i]<16)offeredHTMask|=UInt16(1u<<s.htRates[i]);
            const char *offeredReason=s.phyReason;
            bool ht=false,vht=false,htOp=false,vhtOp=false;
            for(UInt32 p=30;p<bytes;p+=2+f[p+1]){
                const UInt32 id=f[p],len=f[p+1];const UInt8 *v=f+p+2;
                if(id==45){if(ht || len!=26 || !(v[3]&s.ap.ht[3]))return;ht=true;}
                if(id==191){if(vht || len!=12 || (htcReadLE16(v+4)&3)==3 || (htcReadLE16(v+8)&3)==3)return;vht=true;}
                if(id==61){if(htOp || len!=22 || v[0]!=s.operatingChannel)return;htOp=true;}
                if(id==192){if(vhtOp || len!=5 || v[0]>3)return;vhtOp=true;}
            }
            if(!ht || !(responseWMM || responseEDCA) || (s.vhtEnabled && !vht)){
                s.error="association-ht-vht-not-confirmed";s.stage=LinkStage::Failed;return;
            }
            // Response can change AP EDCA/rate capabilities. Commit only after validation.
            for(UInt32 p=30;p<bytes;p+=2+f[p+1]){
                const UInt32 id=f[p];const UInt8 *v=f+p+2;
                if(id==45)for(UInt32 i=0;i<26;++i)s.ap.ht[i]=v[i];
                if(id==191)for(UInt32 i=0;i<12;++i)s.ap.vht[i]=v[i];
                if(id==61)for(UInt32 i=0;i<22;++i)s.ap.htOp[i]=v[i];
                if(id==192)for(UInt32 i=0;i<5;++i)s.ap.vhtOp[i]=v[i];
            }
            negotiateHighThroughput(s);
            if(!wantedVHT){s.vhtEnabled=false;s.vhtMap=0xffff;s.phyReason=offeredReason;}
            else if(s.vhtEnabled){s.vhtRxMap=intersectVHTMap(s.vhtRxMap,offeredRx);
                s.vhtTxMap=intersectVHTMap(s.vhtTxMap,offeredTx);
                s.vhtMap=intersectVHTMap(intersectVHTMap(s.vhtRxMap,s.vhtTxMap),offeredVHT);
                s.vhtRxMap=intersectVHTMap(s.vhtRxMap,s.vhtMap);s.vhtTxMap=intersectVHTMap(s.vhtTxMap,s.vhtMap);}
            UInt8 count=0;s.nss=1;
            for(UInt32 i=0;i<s.htRateCount;++i)if(s.htRates[i]<16 && (offeredHTMask&(1u<<s.htRates[i])))s.htRates[count++]=s.htRates[i];
            s.htRateCount=count;
            if(s.vhtEnabled){for(UInt32 i=0;i<offeredNSS;++i)if(((s.vhtMap>>(2*i))&3)!=3)s.nss=UInt8(i+1);}
            else for(UInt32 i=0;i<count;++i)if(s.htRates[i]/8+1>s.nss)s.nss=UInt8(s.htRates[i]/8+1);
            // VDEV_START already selected this channel definition. A changed
            // response requires a new association, never silently retune here.
            if(s.width!=offeredWidth || s.centerChannel!=offeredCenter || s.nss>offeredNSS){
                s.error="association-channel-capability-changed";s.stage=LinkStage::Failed;return;}
            s.localHT=offeredHT;s.localVHT=offeredLocalVHT;
            if(!s.htEnabled || !s.htRateCount || (wantedVHT && (!s.vhtEnabled || (s.vhtMap&3)==3))){s.error="association-capability-changed";s.stage=LinkStage::Failed;return;}
        }
        rememberNegotiatedRates(s);
        if(responseOM)recordAPOperatingMode(s,operatingMode);
        const auto initial=desiredAPConfig(s);applyPeerRates(s,initial.nss,initial.width);
        if(s.ap.wmmPresent && (responseWMM || responseEDCA)){
            s.ap.wmmCount=responseCount;s.ap.wmmVendor=responseWMM;
            for(UInt32 i=0;i<16;++i)s.ap.wmm[i]=responseAC[i];}
        s.aid=aid;s.assocReply=true;
    }
}
inline void receiveStationManagement(ConnectionState &s,const UInt8 *f,UInt32 bytes,UInt32 status,UInt32 rxChannel=0){
    receiveStationManagementAuthenticated(s,f,bytes,status,rxChannel,ManagementAuthentication::Unverified);
}

inline UInt32 connectionReadLE32(const UInt8 *p) {
    return static_cast<UInt32>(htcReadLE16(p)) |
           (static_cast<UInt32>(htcReadLE16(p+2))<<16);
}
// One packet in flight. Completion IDs are never inferred from a CE read pointer.
inline bool mappedPeer(const ConnectionState &s,UInt16 id){
    return id<2048 && (s.peerIDs[id/32]&(1u<<(id%32)));
}
inline bool retiredPeer(const ConnectionState &s,UInt16 id){
    return id<2048 && (s.retiredPeerIDs[id/32]&(1u<<(id%32)));
}
inline UInt8 expectedSecurityIndication(const ConnectionState &s){
    if(s.securityPhase==1)return 0x86; // CCMP pairwise/unicast.
    if(s.securityPhase==2){
        if(s.groupCipher==AthSecurity::cipherTKIP)return 4; // HTT_SECURITY_TKIP, group.
        if(s.groupCipher==AthSecurity::cipherCCMP)return 6; // HTT_SECURITY_AES_CCMP, group.
    }
    return 0xff;
}
inline const char *validateConnectionHTT(const ConnectionState &s,const UInt8 *f,UInt32 end) {
    if(!f || end<9)return "htt-connection-short";
    switch(f[8]){
    case 3: if(end!=20 || htcReadLE16(f+10)>=2048)return "peer-map-length";break;
    case 4: if(end!=12 || htcReadLE16(f+10)>=2048)return "peer-unmap-length";break;
    case 5: case 6:
        // HTT TLV RX_ADDBA/RX_DELBA: window/reserved + peer:12,TID:4.
        if(end!=12)return "block-ack-length";
        if(f[8]==5 && (!f[9] || f[9]>64))return "block-ack-window";
        break;
    case 0xb:
        if(end!=36)return "security-indication-length";
        if(htcReadLE16(f+10)>=2048 || (f[9]&0x7f)>=8)return "security-indication-fields";
        // Linux consumes NONE indications even after the peer has disappeared.
        // Whether this acknowledges our key-disable command is decided at commit;
        // an unrelated/late NONE must neither stop the runtime nor install a key.
        if(!(f[9]&0x7f))break;
        if(s.disconnect!=DisconnectStage::None || !s.secure || s.securityPhase<1 || s.securityPhase>2 ||
           !s.submitted || !mappedPeer(s,htcReadLE16(f+10)) ||
           f[9]!=expectedSecurityIndication(s))return "security-indication-unexpected";
        break;
    case 7: {
        if(end<16 || !f[10] || f[10]>kDataTxSlots || 12u+2u*f[10]>end)return "data-completion-length";
        if((f[9]&7)>4)return "data-completion-status";
        // Validate the entire ID list before mutating ownership or HTC credits.
        for(UInt32 i=0;i<f[10];++i){
            const UInt16 id=htcReadLE16(f+12+2*i);
            for(UInt32 j=0;j<i;++j)if(htcReadLE16(f+12+2*j)==id)return "data-completion-duplicate";
            const int slot=dataTxSlotForID(s,id);
            if(slot>=0){if(s.dataTX[slot].complete)return "data-completion-id";}
            else if(!s.txOwned || s.txComplete || id!=s.txId || f[10]!=1)return "data-completion-id";
        }
        break;
    }
    case 0xe: // HTT_MGMT_TX_COMPLETION: common prefix includes 32-bit ID/status.
        // RM.4.4.1 emits 12 payload bytes; newer layouts append PPDU/RSSI.
        if(end<20 || (end%4))return "mgmt-completion-length";
        if(!s.txOwned || !s.txManagement || s.txComplete || connectionReadLE32(f+12)!=s.txId)
            return "mgmt-completion-id";
        if(connectionReadLE32(f+16)>2)return "mgmt-completion-status";
        break;
    case 0xd:return "tx-inspection-not-supported";
    default:break;
    }
    return nullptr;
}
inline void clearPeerBA(ConnectionState &s,UInt16 peer){
    for(auto &ba:s.ba)if(ba.window && ba.peer==peer){ba.window=0;ba.sequenceValid=false;s.baSSNValid[ba.tid]=false;}
    if(peer==s.peerId)for(auto &valid:s.baSSNValid)valid=false;
}
inline void commitConnectionHTT(ConnectionState &s,const UInt8 *f) {
    if(f[8]==0xb){
        auto &d=s.securityInd;++d.received;d.peer=htcReadLE16(f+10);d.flags=f[9];
        d.disconnect=static_cast<UInt32>(s.disconnect);
        d.mapped=mappedPeer(s,d.peer);d.retired=retiredPeer(s,d.peer);d.ack=false;
        if(f[9]&0x7f){
            // The validator matched the expected cipher, cast, peer, and install phase.
            s.securityAck=d.ack=true;d.action="install-ack";return;
        }
        const bool clearing=s.disconnect==DisconnectStage::ClearPair || s.disconnect==DisconnectStage::ClearGroup;
        if(clearing && s.secure && s.submitted && (d.mapped || d.retired) &&
           f[9]==(s.disconnect==DisconnectStage::ClearPair?0x80:0)){
            s.securityAck=d.ack=true;d.action=d.mapped?"clear-ack":"late-clear-ack";
            if(!d.mapped)++d.lateClears;
        }else{
            ++d.ignored;d.action="none-ignored";
            // A live authorized peer losing its cipher closes the controlled
            // port immediately, then uses ordinary association teardown.
            if(s.secure && s.portAuthorized && d.mapped && s.disconnect==DisconnectStage::None){
                s.portAuthorized=false;connectionFailure(s,"firmware-key-cleared");s.cancelRequested=true;
                d.action="live-key-cleared";
            }
        }
        return;
    }
    if(f[8]==5 || f[8]==6){
        const UInt16 info=htcReadLE16(f+10),peer=info>>4;const UInt8 tid=info&15;
        if(!mappedPeer(s,peer)){++s.baIgnored;return;}
        ConnectionState::BASession *match=nullptr,*empty=nullptr;
        for(auto &ba:s.ba){if(ba.window && ba.peer==peer && ba.tid==tid)match=&ba;
            if(!ba.window && !empty)empty=&ba;}
        if(f[8]==6){++s.baDeleted;s.baSSNValid[tid]=false;if(match){match->window=0;match->sequenceValid=false;}return;}
        ++s.baAdded;if(!match)match=empty;
        if(!match){++s.baOverflow;return;}
        match->peer=peer;match->tid=tid;match->window=f[9];match->sequenceValid=false;++match->generation;
        match->startValid=s.baSSNValid[tid];match->startSeq=s.baSSN[tid];
        s.baSSNValid[tid]=false; // One observed request belongs to one BA generation.
        match->frames=match->gaps=0;return;
    }
    if(f[8]==3){
        // Never attribute a late clear to an ID that firmware reused, including
        // a new mapping for a MAC other than the configured AP.
        const UInt16 id=htcReadLE16(f+10);s.retiredPeerIDs[id/32]&=~(1u<<(id%32));
        auto &d=s.diagnostics;++d.maps;d.peer=id;d.vdev=f[9];d.peerBefore=s.peerCount;
        d.own=f[9]==0 && sameMAC(f+12,s.bssid);
        for(UInt32 i=0;i<6;++i)d.peerMAC[i]=f[12+i];
    }
    if(f[8]==3 && ((s.stage>=LinkStage::Peer && s.stage<LinkStage::Failed) ||
                  (s.peerCreateIssued && s.disconnect!=DisconnectStage::None))){
        UInt16 id=htcReadLE16(f+10);UInt32 bit=1u<<(id%32);
        bool own=f[9]==0 && sameMAC(f+12,s.bssid);
        if(own && !mappedPeer(s,id)){s.peerIDs[id/32]|=bit;++s.peerCount;}
        if(!own && mappedPeer(s,id)){clearPeerBA(s,id);s.peerIDs[id/32]&=~bit;--s.peerCount;
            if(!s.peerCount){s.peerMapped=false;
                if(s.disconnect==DisconnectStage::None){s.stage=LinkStage::Failed;connectionFailure(s,"peer-remapped");}}}
        if(own){s.peerId=id;s.peerMapped=true;}
    }
    if(f[8]==4){auto &d=s.diagnostics;++d.unmaps;d.peer=htcReadLE16(f+10);
        d.peerBefore=s.peerCount;d.own=mappedPeer(s,d.peer);d.vdev=d.own?0:255;
        for(UInt32 i=0;i<6;++i)d.peerMAC[i]=d.own?s.bssid[i]:0;
        if(!d.own)++d.unknownUnmaps;
    }
    if(f[8]==4 && mappedPeer(s,htcReadLE16(f+10))){
        UInt16 id=htcReadLE16(f+10);clearPeerBA(s,id);s.peerIDs[id/32]&=~(1u<<(id%32));--s.peerCount;
        s.retiredPeerIDs[id/32]|=1u<<(id%32);
        if(!s.peerCount){s.peerMapped=false;
            if(s.disconnect==DisconnectStage::None){connectionFailure(s,"peer-unmapped");s.stage=LinkStage::Failed;}}
    }
    if(f[8]==3 || f[8]==4)s.diagnostics.peerAfter=s.peerCount;
    if(f[8]==7)for(UInt32 i=0;i<f[10];++i){
        const int slot=dataTxSlotForID(s,htcReadLE16(f+12+2*i));
        if(slot>=0){s.dataTX[slot].complete=true;s.dataTX[slot].status=f[9]&7;}
        else{s.txComplete=true;s.txStatus=f[9]&7;}
    }
    if(f[8]==0xe){s.txComplete=true;s.txStatus=static_cast<UInt8>(connectionReadLE32(f+16));}
}
inline UInt8 txAccessCategory(UInt8 tid){const UInt8 ac[]={0,1,1,0,2,2,3,3};return ac[tid&7];}
// PCP overrides IP priority. IPv4 DSCP and IPv6 traffic class exclude ECN;
// EF voice is UP6, other classes retain precedence. No port/payload inspection.
inline UInt8 ethernetPriority(const UInt8 *p,UInt32 length){
    if(!p || length<14)return 0;
    UInt32 offset=14;UInt16 type=static_cast<UInt16>((UInt16(p[12])<<8)|p[13]);
    UInt8 priority=0;bool tagged=false;
    for(UInt32 tags=0;tags<2 && (type==0x8100 || type==0x88a8);++tags){
        if(length-offset<4)return 0;
        if(!tagged){priority=p[offset]>>5;tagged=true;}
        type=static_cast<UInt16>((UInt16(p[offset+2])<<8)|p[offset+3]);offset+=4;
    }
    if(tagged)return priority;
    if(type==0x888e)return 7;
    UInt8 dscp=0;
    if(type==0x0800 && length-offset>=20 && (p[offset]>>4)==4 && (p[offset]&15)>=5 &&
       UInt32(p[offset]&15)*4<=length-offset)dscp=p[offset+1]>>2;
    else if(type==0x86dd && length-offset>=40 && (p[offset]>>4)==6)
        dscp=static_cast<UInt8>(((p[offset]&15)<<2)|(p[offset+1]>>6));
    return dscp==46?6:static_cast<UInt8>(dscp>>3);
}
inline UInt8 txPriorityTID(const ConnectionState &s,const UInt8 *packet,UInt32 length){
    if(!s.ap.wmmPresent || !s.portAuthorized)return 16;
    UInt8 tid=ethernetPriority(packet,length);
    // There is no TSPEC admission in this STA profile. Follow the same
    // VO -> VI -> BE -> BK downgrade chain as mac80211/net80211.
    for(UInt32 attempts=0;attempts<4;++attempts){const UInt8 ac=txAccessCategory(tid);
        if(!(s.ap.wmm[4*ac]&16))return tid;
        if(ac==3)tid=5;else if(ac==2)tid=0;else if(ac==0)tid=1;
        else return packet && length>=14 && packet[12]==0x88 && packet[13]==0x8e?16:255;
    }
    return 255;
}
// Our HTT management path follows Linux v6.12 hardware CCMP with host IV.
// Software ciphertext passthrough has not been validated for this firmware.
inline bool packetProtectionAllowed(const UInt8 *packet,UInt32 length,const ConnectionState &s,bool management,AthSecurity::ManagementTX protection){
    if(!packet || length<14 || length>kEthernetFrameMax)return false;
    if(!management)return protection==AthSecurity::ManagementTX::Plain && ethernetFrameAllowed(packet,length);
    if(protection==AthSecurity::ManagementTX::Plain)
        return length>=24 && !(htcReadLE16(packet)&0xc70fu) && !(htcReadLE16(packet+22)&15) &&
            !(s.pmfRequired && s.pairInstalled && AthSecurity::robustManagementFrame(packet,length));
    if(protection!=AthSecurity::ManagementTX::HardwareCCMP || !s.pmfRequired || !s.secure || !s.pairInstalled ||
       s.stage!=LinkStage::Connected || s.disconnect!=DisconnectStage::None)return false;
    return AthSecurity::PMFHardwareCCMP::valid(packet,length,s.local,s.bssid);
}
inline UInt32 buildHTTPacket(UInt8 *f,UInt32 capacity,const UInt8 *packet,UInt32 length,
                            UInt32 address,UInt32 frags,UInt8 ep,const ConnectionState &s,bool management,
                            AthSecurity::ManagementTX protection=AthSecurity::ManagementTX::Plain) {
    if(!f || capacity<256 || !ep || ep==255 || !address || !frags || !packetProtectionAllowed(packet,length,s,management,protection))return 0;
    htcZero(f,256);f[0]=ep;f[5]=s.httSequence;
    UInt32 bytes=0;
    if(!frags)return 0;
    UInt32 prefetch=length<50?length:50;bytes=24+((prefetch+3u)&~3u);
    f[8]=1;f[9]=management?(protection==AthSecurity::ManagementTX::HardwareCCMP?0x61:0x65):((s.secure && (s.portAuthorized || s.rekeyKind))?0x42:0x46); // HTT >=3: TX_FRM for both management and Ethernet.
    const UInt8 tid=management?17:txPriorityTID(s,packet,length);if(tid==255)return 0;
    htcWriteLE16(f+10,static_cast<UInt16>(0x800u|(UInt16(tid)<<6))); // POSTPONED, TID bits 6..10
    htcWriteLE16(f+12,static_cast<UInt16>(length));htcWriteLE16(f+14,s.txId);
    htcWriteLE32(f+16,management?address:frags);htcWriteLE32(f+20,0xffff);
    for(UInt32 i=0;i<prefetch;++i)f[24+i]=packet[i];
    htcWriteLE16(f+2,static_cast<UInt16>(bytes-8));return bytes;
}
inline UInt16 connBE16(const UInt8 *p){return static_cast<UInt16>((UInt16(p[0])<<8)|p[1]);}
// Convert one RFC1042 A-MSDU subframe to Ethernet II.  The returned offset is
// relative to the beginning of the decapped payload; zero means no following
// subframe.  Each subframe is padded to four bytes except the final one.
inline UInt32 decodeAMSDUSubframe(const UInt8 *p,UInt32 length,UInt32 base,
                                  UInt8 *out,UInt32 capacity,UInt32 *nextOffset){
    if(nextOffset)*nextOffset=0;
    if(!p || !out || base>length || length-base<14)return 0;
    const UInt8 *h=p+base;const UInt32 subLength=connBE16(h+12);
    if(subLength<8 || subLength>kEthernetMTU+16 || 14u+subLength>length-base)return 0;
    // Fragment-and-forge: an RFC1042 LLC prefix must never be accepted as DA.
    const UInt8 rfc1042[6]={0xaa,0xaa,3,0,0,0};
    if(sameMAC(h,rfc1042) || (h[6]&1))return 0;
    const UInt8 *payload=h+14;const bool snap=payload[0]==0xaa && payload[1]==0xaa &&
        payload[2]==3 && payload[3]==0 && payload[4]==0 && (payload[5]==0 || payload[5]==0xf8);
    UInt32 result=0;
    if(snap){
        result=14u+subLength-8u;if(result>capacity)return 0;
        for(UInt32 i=0;i<6;++i){out[i]=h[i];out[6+i]=h[6+i];}
        out[12]=payload[6];out[13]=payload[7];
        for(UInt32 i=8;i<subLength;++i)out[6+6+2+i-8]=payload[i];
    }else if(subLength>=2 && connBE16(payload)>0x05dcu){
        // A few firmware revisions hand the A-MSDU payload to the host after
        // DIX decapsulation.  Preserve that form when it is unambiguous.
        result=14u+subLength-2u;if(result>capacity)return 0;
        for(UInt32 i=0;i<6;++i){out[i]=h[i];out[6+i]=h[6+i];}
        out[12]=payload[0];out[13]=payload[1];
        for(UInt32 i=2;i<subLength;++i)out[14+i-2]=payload[i];
    }else return 0;
    if(!ethernetFrameAllowed(out,result))return 0;
    const UInt32 unpadded=base+14u+subLength;
    if(unpadded==length)return result;
    const UInt32 padded=(14u+subLength+3u)&~3u;
    const UInt32 following=base+padded;
    if(following+14u<=length){
        const UInt32 candidate=connBE16(p+following+12);
        if(candidate<8 || candidate>length-following-14u)return 0;
        if(nextOffset)*nextOffset=following;
    }else {
        // Last subframe is not padded. Trailing bytes are malformed rather
        // than evidence that the previous subframe was complete.
        return 0;
    }
    return result;
}
// Convert only complete data from the configured AP. Security validation is
// applied once per staged MPDU before delivery. Native WiFi
// carries LLC/SNAP; output is Ethernet II. A-MSDU subframes are exposed one at
// a time through amsduOffset/nextAmsduOffset so the caller can deliver every
// MSDU in a chained aggregate. No checksum offload is advertised.
inline UInt32 decodeEthernetRX(const ConnectionState &s,const UInt8 *d,UInt32 bytes,
                               UInt8 mpduStatus,UInt16 peerId,UInt8 *out,UInt32 capacity,
                               UInt16 indicatedLength=0,UInt32 amsduOffset=0,
                               UInt32 *nextAmsduOffset=nullptr) {
    if(nextAmsduOffset)*nextAmsduOffset=0;
    if(s.stage!=LinkStage::Connected || !d || !out || bytes<300 || mpduStatus!=1)return 0;
    const UInt32 attention=connLE32(d+4);
    // BUFFER_FRAGMENT is valid when ring2_more_count is non-zero.  The
    // individual fragment buffers are joined by serviceHTTRxOne before this
    // decoder is called; only actual integrity/length errors remain fatal.
    const UInt32 bad=(1u<<3)|(1u<<4)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<13)|
        (1u<<16)|(1u<<17)|(1u<<24)|(1u<<27)|(1u<<28)|(1u<<29)|(1u<<30);
    if(!(attention&0x80000000u) || (attention&bad))return 0;
    // rx_msdu_start_common at 24: info0 +0, flow_id_crc +4, info1 +8.
    UInt32 length=indicatedLength?indicatedLength:(connLE32(d+24)&0x3fff),mode=(connLE32(d+32)>>8)&3;
    const UInt8 *p=d+300;
    if(length>bytes-300 || length<14)return 0;
    // Firmware may use a separate peer for group traffic. Permit this only
    // for explicit-paddr RX and a multicast destination, never for unicast.
    if(!mappedPeer(s,peerId) && (s.peerCount || peerId!=s.peerId) && (!indicatedLength ||
       ((mode==2 || mode==3)?!(p[0]&1):(mode!=1 || length<24 || !(p[4]&1)))))return 0;
    if(mode==3){
        const UInt32 result=decodeAMSDUSubframe(p,length,amsduOffset,out,capacity,nextAmsduOffset);
        if(result && !(out[0]&1) && !sameMAC(out,s.local))return 0;
        return result;
    }
    if(mode==2){
        const bool aggregate=amsduOffset || connBE16(p+12)<=0x05dcu;
        if(aggregate){
            const UInt32 result=decodeAMSDUSubframe(p,length,amsduOffset,out,capacity,nextAmsduOffset);
            if(result)return result;
            return 0;
        }
        if(length>kEthernetFrameMax || length>capacity || (p[6]&1))return 0;
        if(!(p[0]&1) && !sameMAC(p,s.local))return 0;
        for(UInt32 i=0;i<length;++i)out[i]=p[i];
        return ethernetFrameAllowed(out,length)?length:0;
    }
    if(mode!=1 || length<32)return 0;
    const UInt16 fc=htcReadLE16(p);const bool qos=(fc&0x80)!=0;
    // RM.4.4.1 normally keeps QoS control in rx_hdr_status, but older
    // QCA6174 builds strip those two bytes before the native payload.  The
    // LLC signature disambiguates both layouts and preserves compatibility.
    const UInt32 headerLength=qos && !(p[24]==0xaa && p[25]==0xaa && p[26]==3)?26:24;
    if((fc&(s.secure?0x037cu:0x437cu))!=0x0208 || length<headerLength+8 ||
       (htcReadLE16(p+22)&15) || !sameMAC(p+10,s.bssid))return 0;
    if(!(p[4]&1) && !sameMAC(p+4,s.local))return 0;
    const bool aggregate=qos && headerLength==26 && (p[24]&0x80);
    if(aggregate){
        const UInt32 result=decodeAMSDUSubframe(p,length,headerLength+amsduOffset,out,capacity,nextAmsduOffset);
        if(result){
            if(nextAmsduOffset && *nextAmsduOffset)*nextAmsduOffset-=headerLength;
            return result;
        }
        return 0;
    }
    const UInt8 *llc=p+headerLength;
    if(llc[0]!=0xaa || llc[1]!=0xaa || llc[2]!=3 || llc[3] || llc[4] || (llc[5]!=0 && llc[5]!=0xf8))return 0;
    UInt32 result=length-headerLength-8+14;if(result>kEthernetFrameMax || result>capacity)return 0;
    for(UInt32 i=0;i<6;++i){out[i]=p[4+i];out[6+i]=p[16+i];}
    out[12]=llc[6];out[13]=llc[7];
    for(UInt32 i=headerLength+8;i<length;++i)out[i-(headerLength+8)+14]=p[i];
    return ethernetFrameAllowed(out,result)?result:0;
}
inline void recordRxBlockAck(ConnectionState &s,const UInt8 *d){
    if(!d)return;
    const UInt32 mpdu=connLE32(d+12),mpduInfo1=connLE32(d+20);
    const UInt16 peer=static_cast<UInt16>(mpdu&0x7ff),seq=static_cast<UInt16>((mpdu>>16)&0xfff);
    const UInt8 tid=static_cast<UInt8>((mpduInfo1>>28)&15);
    for(auto &ba:s.ba)if(ba.window && ba.peer==peer && ba.tid==tid){
        if(ba.sequenceValid){const UInt16 delta=static_cast<UInt16>((seq-ba.lastSeq)&0xfff);
            if(delta>1 && delta<2048){ba.gaps+=delta-1;s.baGaps+=delta-1;}}
        ba.lastSeq=seq;ba.sequenceValid=true;++ba.frames;++s.baFrames;return;
    }
}
// QCA6174 v1 descriptor: MPDU start at 12, original RX header at 236.
// Fail closed on plaintext data, unsupported ciphers/fragmentation,
// wrong key index and replay. Firmware verifies CCMP/TKIP MIC; host enforces PN/TSC.
inline bool acceptSecureRX(ConnectionState &s,const UInt8 *d,const UInt8 *frame,UInt32 bytes,UInt16 peer=0xffff){
    if(!s.secure)return true;
    auto reject=[&](ConnectionState::RxDrop reason){s.rxLastReason=reason;return false;};
    if(bytes<14)return reject(ConnectionState::Length);
    const bool eapol=frame[12]==0x88 && frame[13]==0x8e;
    UInt32 info=connLE32(d+12);bool encrypted=info&(1u<<13);
    if(!encrypted)return eapol?true:reject(ConnectionState::Plaintext);
    if(s.securityPhase<3 && !s.rekeyKind)return reject(ConnectionState::Key);
    const UInt8 *h=d+236;UInt16 fc=htcReadLE16(h);
    auto headerReject=[&](RXSecurityFault fault){
        s.rxSecurity.record(d,fault,s.bssid,peer,info);return reject(ConnectionState::Security);};
    // Exactly the same predicate as the former 0x470f/0x4208 check; splitting
    // it provides evidence without trusting a modified/absent original header.
    if((fc&0x000f)!=0x0008)return headerReject(RXSecurityFault::HeaderType);
    if((fc&0x0300)!=0x0200)return headerReject(RXSecurityFault::Direction);
    if(!(fc&0x4000))return headerReject(RXSecurityFault::Protected);
    if(fc&0x0400)return headerReject(RXSecurityFault::MoreFragments);
    if(htcReadLE16(h+22)&15)return headerReject(RXSecurityFault::FragmentNumber);
    if(!sameMAC(h+10,s.bssid))return headerReject(RXSecurityFault::Transmitter);
    bool qos=(fc&0x80)!=0;UInt32 tid=qos?(h[24]&15):16;
    const UInt8 *iv=h+(qos?((fc&0x8000)?32:28):24);
    // Key selection follows the MPDU receiver address, not an A-MSDU DA.
    const bool group=(h[4]&1)!=0;
    const bool tkipGroup=group && s.groupCipher==AthSecurity::cipherTKIP;
    if((group && !tkipGroup && s.groupCipher!=AthSecurity::cipherCCMP) ||
       (tkipGroup?(info>>28)!=4:(info>>28)!=6))return reject(ConnectionState::Key);
    if(!(iv[3]&0x20) || (iv[3]>>6)!=(group?s.groupIndex:0))return reject(ConnectionState::Key);
    UInt64 pn=UInt64(connLE32(d+16))|(UInt64(htcReadLE16(d+20))<<32);
    if(tkipGroup){
        // TKIP IV bytes are TSC1, WEPSeed, TSC0, ExtIV, TSC2..TSC5.
        if(iv[1]!=static_cast<UInt8>((iv[0]|0x20)&0x7f))return reject(ConnectionState::Security);
        pn=UInt64(iv[2])|(UInt64(iv[0])<<8)|(UInt64(iv[4])<<16)|(UInt64(iv[5])<<24)|
           (UInt64(iv[6])<<32)|(UInt64(iv[7])<<40);
    }
    UInt64 &last=group?s.groupPN[tid]:s.pairPN[tid];
    UInt64 rsc=0;if(group)for(UInt32 i=0;i<6;++i)rsc|=UInt64(s.groupRSC[i])<<(8*i);
    if(!pn || pn<=last || (group && pn<=rsc))return reject(ConnectionState::Replay);
    if(!eapol && !s.portAuthorized)return reject(ConnectionState::Port);
    // Called exactly once after an entire MPDU has been validated/reordered.
    // Its subsequent MSDUs inherit this authorization inside RXBatch only.
    last=pn;return true;
}
} // namespace Ath10k
