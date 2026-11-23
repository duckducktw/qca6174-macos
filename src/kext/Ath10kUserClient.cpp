#include "Ath10kPCIDevice.hpp"
#include "Ath10kUserClient.hpp"
#include <IOKit/IOLib.h>
#include <kern/task.h>
#define super IOUserClient
OSDefineMetaClassAndStructors(Ath10kUserClient,IOUserClient)
// Local login sessions may control the adapter; root remains available for diagnostics.
static bool athControlAllowed(task_t task){
 return task && (IOUserClient::clientHasPrivilege(task,kIOClientPrivilegeLocalUser)==kIOReturnSuccess ||
                 IOUserClient::clientHasPrivilege(task,kIOClientPrivilegeAdministrator)==kIOReturnSuccess);
}
bool Ath10kUserClient::initWithTask(task_t task,void *security,UInt32 type,OSDictionary *properties){
 if(!athControlAllowed(task) || !super::initWithTask(task,security,type,properties))return false;
 openingTask=task;task_reference(openingTask);return true;
}
void Ath10kUserClient::free(){
 if(openingTask){task_deallocate(openingTask);openingTask=nullptr;}
 super::free();
}
bool Ath10kUserClient::start(IOService *p){
 device=OSDynamicCast(Ath10kPCIDevice,p);
 return device && super::start(p);
}
void Ath10kUserClient::stop(IOService *p){
 if(device){device->closeControl(this);device=nullptr;}
 super::stop(p);
}
IOReturn Ath10kUserClient::clientClose(){
 if(device)device->closeControl(this);
 terminate();return kIOReturnSuccess;
}
IOReturn Ath10kUserClient::externalMethod(uint32_t selector,IOExternalMethodArguments *a,IOExternalMethodDispatch *,OSObject *,void *){
 if(!device || isInactive())return kIOReturnNotAttached;
 if(selector || !a || a->structureInputSize!=sizeof(AthControl::Request) ||
    a->structureOutputSize!=sizeof(AthControl::Response) || !a->structureInput || !a->structureOutput ||
    a->scalarInputCount || a->scalarOutputCount || a->structureInputDescriptor || a->structureOutputDescriptor)return kIOReturnBadArgument;
 // Authentication at open is not enough for a port transferred to another process.
 if(current_task()!=openingTask || !athControlAllowed(openingTask))return kIOReturnNotPrivileged;
 return device->controlRequest(this,static_cast<const AthControl::Request *>(a->structureInput),static_cast<AthControl::Response *>(a->structureOutput));
}
IOReturn Ath10kPCIDevice::newUserClient(task_t task,void *security,UInt32 type,OSDictionary *properties,IOUserClient **out){
 // airportd only knows IO80211Controller nodes (its XPC launch event is an
 // "iokit.matching for IO80211Controller" one), so if Apple80211 wants a user
 // client it has to come through here. Record every attempt, whatever the type:
 // a silent counter after airportd ran means the "-3903 Wireless is not
 // available" decision happens without ever knocking on this door.
 {UInt64 seen=0;if(OSNumber*n=OSDynamicCast(OSNumber,getProperty("Ath10kCtrlNewUserClient")))seen=n->unsigned64BitValue();
  setProperty("Ath10kCtrlNewUserClient",seen+1,32);setProperty("Ath10kCtrlNewUserClientType",(UInt64)type,32);
  IOLog("Ath10kMac: controller newUserClient type=%u (#%llu)\n",(unsigned)type,(unsigned long long)(seen+1));}
 if(type!=AthControl::clientType)return IOEthernetController::newUserClient(task,security,type,properties,out);
 if(!athControlAllowed(task))return kIOReturnNotPrivileged;
 auto *client=new Ath10kUserClient;
 if(!client)return kIOReturnNoMemory;
 if(!client->initWithTask(task,security,type,properties)){client->release();return kIOReturnError;}
 client->setProperty("IOUserClientDefaultLocking",kOSBooleanTrue);
 if(!client->attach(this)){client->release();return kIOReturnError;}
 if(!client->start(this)){client->detach(this);client->release();return kIOReturnError;}
 *out=client;return kIOReturnSuccess;
}
bool Ath10kPCIDevice::requestDeviceRestart(){
 if(_systemSuspended || _reprobeRequested || !_pci || (powerIOBlocked() && !powerRestartAllowed()))return false;
 if(_wmiLive.active)endWMIRuntime("app-restart-requested");
 if(_dmaQuarantine)return false;
 _reprobeRequested=true;
 _pci->setProperty("Ath10kRecoveryPhase","restart-requested");
 _pci->setProperty("Ath10kRecoveryDetail","App requested guarded driver termination and PCI rematch");
 // Only prepare under the hardware gate. IOService termination runs after
 // runAction returns, so stop() can never release a currently executing gate.
 return true;
}
void Ath10kPCIDevice::fillControlInfo(AthControl::Response &o){
 Ath10k::htcWriteLE32(o.data,0x2ff|(_regulatory.stationDFS?256u:0u));Ath10k::htcWriteLE32(o.data+4,_wmiScan.generation);
 Ath10k::htcWriteLE32(o.data+8,_connection.operatingChannel);
 Ath10k::htcWriteLE32(o.data+12,static_cast<UInt32>(_connection.signalDbm));
 Ath10k::htcWriteLE32(o.data+16,_connection.width);Ath10k::htcWriteLE32(o.data+20,_connection.nss);
 Ath10k::htcWriteLE32(o.data+24,_connection.securityGeneration);
 Ath10k::htcWriteLE32(o.data+28,(_systemSuspended?1u:0u)|(_dmaQuarantine?2u:0u)|(_power.needsRestart?4u:0u));
 o.data[32]=_connection.ssidLength;bcopy(_connection.ssid,o.data+33,_connection.ssidLength);
 strlcpy(reinterpret_cast<char *>(o.data+68),"0.0.98",16);o.length=112;
 const auto &r=_connection.radio;
 Ath10k::htcWriteLE32(o.data+84,(r.requestedPS?1u:0u)|(r.appliedPS?2u:0u)|(r.step?4u:0u));
 Ath10k::htcWriteLE32(o.data+88,_connection.beaconMisses);Ath10k::htcWriteLE32(o.data+92,_connection.txPowerHalfDbm);
 Ath10k::htcWriteLE32(o.data+96,_connection.powerConstraintDb);Ath10k::htcWriteLE32(o.data+100,r.completed);
 Ath10k::htcWriteLE32(o.data+104,_connection.beaconTimeoutMs);
 Ath10k::htcWriteLE32(o.data+108,(_wmiScan.completed?1u:0u)|(_wmiScan.failed?2u:0u)|(_wmiScan.cancelRequested?4u:0u)|(_wmiScan.started?8u:0u));
}
IOReturn Ath10kPCIDevice::controlRequest(IOUserClient *client,const AthControl::Request *r,AthControl::Response *o){
 if(!_wmiWorkLoop || isInactive())return kIOReturnNotReady;
 // Keep this object alive if termination detaches its clients immediately.
 retain();
 IOReturn result=_wmiWorkLoop->runAction(controlAction,this,client,const_cast<AthControl::Request *>(r),o);
 if(result==kIOReturnSuccess && r->operation==AthControl::Restart){
  if(!terminate(kIOServiceRequired)){_reprobeRequested=false;result=kIOReturnNotReady;}
 }
 release();return result;
}
void Ath10kPCIDevice::closeControl(IOUserClient *client){
 if(_wmiWorkLoop)_wmiWorkLoop->runAction(controlAction,this,client,nullptr,nullptr);
}
IOReturn Ath10kPCIDevice::controlAction(OSObject *owner,void *who,void *input,void *output,void *){
 auto *self=OSDynamicCast(Ath10kPCIDevice,owner);if(!self)return kIOReturnBadArgument;
 auto &s=self->_connection;using namespace AthControl;
 if(!input){if(self->_controlOwner==who){self->_controlOwner=nullptr;
   if(!self->_wmiScan.completed)self->_wmiScan.cancelRequested=true;
   uint64_t t=0,n=0;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&n);
   if(Ath10k::requestDisconnect(s,n/1000000u)){s.diagnostics.cancelOrigin=s.diagnostics.lastCancelOrigin="client-close";self->networkLink(false);self->publishConnection();}
   else if(self->_wmiLive.active && s.stage!=Ath10k::LinkStage::Disabled){s.diagnostics.cancelOrigin="client-close";s.cancelRequested=true;s.portAuthorized=false;self->networkLink(false);self->publishConnection();}}return kIOReturnSuccess;}
 const auto &r=*static_cast<const Request *>(input);auto &o=*static_cast<Response *>(output);
 if(r.version!=AthControl::version || r.length>512 || r.operation>RadioOptions)return kIOReturnBadArgument;
 bzero(&o,sizeof(o));o.version=AthControl::version;o.stage=static_cast<UInt32>(s.stage);o.phase=s.securityPhase;
 o.flags=128|512|((self->_wmiScan.attempted && !self->_wmiScan.completed)?256:0)|(s.disconnect!=Ath10k::DisconnectStage::None?64:0)|(self->_wmiLive.active?1:0)|(self->_wmiScan.completed?2:0)|(self->_networkLinkUp?4:0)|(s.txOwned?8:0)|(s.secure?16:0)|(s.eapolTxFailed?32:0);
 o.count=self->_wmiBSS.count;bcopy(s.local,o.local,6);bcopy(s.bssid,o.bssid,6);
 if(s.stage==Ath10k::LinkStage::Disabled)bcopy(self->_wmiInit.mac,o.local,6);
 strlcpy(o.error,s.error,sizeof(o.error));
#ifdef ATH10K_NATIVE
 if(s.stage==Ath10k::LinkStage::Disabled && self->_nativeMACValid)bcopy(self->_nativeMACCurrent,o.local,6);
#endif
 if(r.operation==Status)return kIOReturnSuccess;
#ifdef ATH10K_NATIVE
 if(self->_nativeMACActive || self->_nativeMACWaiting)return kIOReturnBusy;
#endif
 if(r.operation==BSS){
  if(r.index>=self->_wmiBSS.count)return kIOReturnNotFound;
  const auto &b=self->_wmiBSS.entries[r.index];o.data[0]=b.ssidLength;o.data[1]=b.privacy;
  bcopy(b.bssid,o.data+2,6);bcopy(b.ssid,o.data+8,b.ssidLength);
  o.data[40]=b.advertisedChannel?b.advertisedChannel:static_cast<UInt8>(b.rxChannel);
  o.data[41]=AthSecurity::supportsWPA2(b.rsn,b.rsnLength);
  Ath10k::htcWriteLE16(o.data+42,b.rsnLength);bcopy(b.rsn,o.data+44,b.rsnLength);o.length=44+b.rsnLength;Ath10k::htcWriteLE32(o.data+o.length,b.snr);o.length+=4;
  Ath10k::htcWriteLE32(o.data+o.length,b.hidden?2u:0u);o.length+=4;return kIOReturnSuccess;
 }
 if(r.operation==Info){self->fillControlInfo(o);return kIOReturnSuccess;}
#ifdef ATH10K_NATIVE
 // Native WCL exclusively owns mutations, including hardware restart.
 return kIOReturnNotPermitted;
#endif
 if(r.operation==Restart){
  if(self->_controlOwner && self->_controlOwner!=who)return kIOReturnNotPermitted;
  return self->requestDeviceRestart()?kIOReturnSuccess:kIOReturnNotReady;
 }
#ifdef ATH10K_NATIVE
 // Native WCL owns association and keys; diagnostic clients are read-only.
 return kIOReturnNotPermitted;
#endif
 if(!self->_wmiLive.active)return kIOReturnNotReady;
 if(s.disconnect!=Ath10k::DisconnectStage::None)return kIOReturnBusy;
 if(r.operation==RadioOptions){
  if(self->_controlOwner && self->_controlOwner!=who)return kIOReturnNotPermitted;
  if(r.index>1 || r.length)return kIOReturnBadArgument;
  s.radio.requestedPS=r.index!=0;return kIOReturnSuccess;
 }
 if(r.operation==Scan){
  if(self->_controlOwner && self->_controlOwner!=who)return kIOReturnNotPermitted;
  if(r.index>1)return kIOReturnBadArgument;
  if(r.index==1){if(!self->_wmiScan.completed)self->_wmiScan.cancelRequested=true;return kIOReturnSuccess;}
  if(self->_wmiEvents.rfKill)return kIOReturnNotReady;
  if(s.stats.step || s.maintenance.step || s.radio.step || s.csa.phase!=Ath10k::ChannelSwitchPhase::Idle || !self->_wmiScan.completed || s.wmiOwned || s.txOwned || self->_wmi.credits.outstanding ||
     (s.stage!=Ath10k::LinkStage::Disabled && (s.stage!=Ath10k::LinkStage::Connected || !s.portAuthorized)))return kIOReturnBusy;
  // Do not build default cache/state temporaries on the kernel stack.
  // The reported double-fault frame matches the 28496-byte cache plus
  // call overhead. A shared prologue can reserve that temporary even for
  // read-only Status/Info calls that never execute the Scan branch.
  const UInt32 generation=self->_wmiScan.generation+1;
  Ath10k::resetInPlace(self->_wmiScan);self->_wmiScan.generation=generation;
  self->_wmiScan.id=0xa000|((0x36+generation)&0xfff);Ath10k::resetInPlace(self->_wmiBSS);
  return kIOReturnSuccess;
 }
 if(r.operation==Connect){
  if(self->_wmiEvents.rfKill)return kIOReturnNotReady;
  if(self->_controlOwner || s.stage!=Ath10k::LinkStage::Disabled)return kIOReturnExclusiveAccess;
  if(!self->_wmiScan.completed || self->_wmiScan.failed || self->_wmiScan.cancelRequested || !self->_networkEnabled)return kIOReturnNotReady;
  if(r.index>1 || r.length<9 || !r.data[0] || r.data[0]>32 || r.length!=8u+r.data[0] || r.data[1]>1 || (r.data[2]&1))return kIOReturnBadArgument;
  s.regulatory=self->_regulatory;s.hiddenSSID=r.index==1;
  s.secure=r.data[1];s.ssidLength=r.data[0];bcopy(r.data+2,s.bssid,6);bcopy(r.data+8,s.ssid,s.ssidLength);bcopy(self->_wmiInit.mac,s.local,6);
#ifdef ATH10K_NATIVE
  if(self->_nativeMACValid)bcopy(self->_nativeMACCurrent,s.local,6);
#endif
  s.firmwareHT=self->_wmi.htCapability;s.firmwareVHT=self->_wmi.vhtCapability;
        s.firmwareMFPSupport=self->payload().kSupportsMFP;s.pmfQoSTransported=self->_wmiRadio[Ath10k::kWMIPMFQoSIndex].completed;
  s.rfChains=static_cast<UInt8>(self->_wmiInit.requestedChainMask==1?1:2);
  const char *error=Ath10k::selectOpenBSS(s,self->_wmiBSS);
  if(error){s.error=error;return kIOReturnUnsupported;}
  self->_controlOwner=static_cast<IOUserClient *>(who);s.error="none";s.stage=Ath10k::LinkStage::Scan;
  self->publishConnection();return kIOReturnSuccess;
 }
 if(self->_controlOwner!=who)return kIOReturnNotPermitted;
 if(r.operation==SecurityHold){
  if(r.index>1)return kIOReturnBadArgument;
  s.eapolPause=r.index!=0;if(!s.eapolPause)s.txWakePending=true;return kIOReturnSuccess;
 }
 if(r.operation==Cancel){
  if(!self->_wmiScan.completed)self->_wmiScan.cancelRequested=true;
  s.diagnostics.cancelOrigin="app-cancel";
  IOLog("Ath10kMac: APP CANCEL stage=%u security=%u peers=%u first-error=%s\n",static_cast<UInt32>(s.stage),s.securityPhase,s.peerCount,s.error);
  s.cancelRequested=true;s.portAuthorized=false;self->networkLink(false);self->publishConnection();return kIOReturnSuccess;
 }
 if(r.operation==Disconnect){
  uint64_t t=0,n=0;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&n);
  if(!Ath10k::requestDisconnect(s,n/1000000u))return kIOReturnBusy;
  IOLog("Ath10kMac: APP DISCONNECT stage=%u security=%u peers=%u first-error=%s\n",static_cast<UInt32>(s.stage),s.securityPhase,s.peerCount,s.error);
  if(!self->_wmiScan.completed)self->_wmiScan.cancelRequested=true;
  self->networkLink(false);self->publishConnection();return kIOReturnSuccess;
 }
 if(r.operation==Abort){self->endWMIRuntime("app-disconnected-reload-required");return kIOReturnSuccess;}
 if(!s.secure || s.stage!=Ath10k::LinkStage::Connected)return kIOReturnNotReady;
 if(r.operation==ReceiveEAPOL){
  if(!s.eapolCount)return kIOReturnNotReady;
  o.length=s.eapolLength[s.eapolRead];bcopy(s.eapolFrames[s.eapolRead],o.data,o.length);
  AthSecurity::wipe(s.eapolFrames[s.eapolRead],512);s.eapolRead=(s.eapolRead+1)%4;--s.eapolCount;return kIOReturnSuccess;
 }
 if(r.operation==SendEAPOL){
  // Do not acknowledge the new key before its firmware ACK/ECHO barrier.
  if(s.rekeyKind || (s.securityPhase && s.securityPhase!=3 && s.securityPhase!=5))return kIOReturnNotReady;
  if(s.maintenance.step || s.radio.blocked || s.radio.powerBlocked || s.radio.appliedPS || s.csa.blocked || s.txOwned || s.dataPending || s.txPauseMask)return kIOReturnBusy;
  if(r.length<18 || r.data[12]!=0x88 || r.data[13]!=0x8e || !Ath10k::sameMAC(r.data,s.bssid) ||
     !Ath10k::sameMAC(r.data+6,s.local) || AthSecurity::be16(r.data+16)!=r.length-18)return kIOReturnBadArgument;
  uint64_t now=0,ns=0;clock_get_uptime(&now);absolutetime_to_nanoseconds(now,&ns);s.eapolTxFailed=false;
  const char *e=self->submitNetworkPacket(r.data,r.length,false,ns/1000000u);
  if(e){self->endWMIRuntime(e);return kIOReturnError;}
  const UInt16 keyInfo=r.data[15]==3 && r.length>=21?AthSecurity::be16(r.data+19):0;
  IOLog("Ath10kMac: EAPOL TX submit id=%u type=%u key-info=0x%04x bytes=%u security=%u\n",s.txId,r.data[15],keyInfo,r.length,s.securityPhase);
  return kIOReturnSuccess;
 }
 if(r.operation==RekeyGroup || r.operation==RekeyPair){
  if(s.stats.step || s.maintenance.step || s.radio.step || s.csa.phase!=Ath10k::ChannelSwitchPhase::Idle || r.length!=56 || r.data[48]!=s.groupCipher || r.data[49]>3 || !s.portAuthorized || s.securityPhase!=5 || s.wmiOwned || s.rekeyKind)return kIOReturnBusy;
  const UInt32 groupKeyLength=s.groupCipher==AthSecurity::cipherTKIP?32:16;
  if(groupKeyLength==16)for(unsigned i=16;i<32;++i)if(r.data[16+i])return kIOReturnBadArgument;
  const bool pair=r.operation==RekeyPair;
  if(pair && AthSecurity::equal(r.data,s.installedPair,16))return kIOReturnBadArgument;
  bool sameGroup=s.groupInstalled[r.data[49]] && AthSecurity::equal(r.data+16,s.installedGroups[r.data[49]],groupKeyLength);
  for(unsigned i=0;i<4;++i)if(s.groupInstalled[i] && AthSecurity::equal(r.data+16,s.installedGroups[i],groupKeyLength) && i!=r.data[49])return kIOReturnBadArgument;
  if(sameGroup && (!pair || r.data[49]!=s.groupIndex))return kIOReturnBadArgument;
  bcopy(r.data,s.temporalKey,16);bcopy(r.data+16,s.groupKey,32);s.groupIndex=r.data[49];bcopy(r.data+50,s.groupRSC,6);
  if(!self->_wmiScan.completed)self->_wmiScan.cancelRequested=true;
  s.rekeyKind=pair?2:1;s.rekeySkipGroup=pair && sameGroup;s.securityPhase=pair?1:2;
  s.portAuthorized=false;s.submitted=false;s.authorizeRequested=false;s.securityAck=false;
  uint64_t t=0,n=0;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&n);s.sinceMs=n/1000000u;
  return kIOReturnSuccess;
 }
 if(r.operation==InstallKeys){
  if(r.length!=56 || r.data[48]!=s.groupCipher || r.data[49]>3 || s.securityPhase || s.portAuthorized)return kIOReturnBadArgument;
  if(s.groupCipher==AthSecurity::cipherCCMP)for(unsigned i=16;i<32;++i)if(r.data[16+i])return kIOReturnBadArgument;
  bcopy(r.data,s.temporalKey,16);bcopy(r.data+16,s.groupKey,32);s.groupIndex=r.data[49];bcopy(r.data+50,s.groupRSC,6);
  UInt64 pn=0;for(unsigned i=0;i<6;++i)pn|=UInt64(s.groupRSC[i])<<(8*i);
  for(auto &v:s.groupPN)v=pn;
  s.securityPhase=1;s.submitted=false;return kIOReturnSuccess;
 }
 if(r.operation==Authorize){
  if(s.securityPhase!=3 || !s.lastTxEAPOL || s.txOwned || !s.txComplete || s.txStatus || s.eapolTxFailed)return kIOReturnNotReady;
  s.authorizeRequested=true;return kIOReturnSuccess;
 }
 return kIOReturnUnsupported;
}
