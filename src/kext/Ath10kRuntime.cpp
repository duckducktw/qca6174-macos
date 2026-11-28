#include "Ath10kPCIDevice.hpp"
#include <libkern/OSAtomic.h>
#include <sys/kpi_mbuf.h>

UInt64 Ath10k::uptimeNS()
{
    uint64_t t=0,ns=0;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&ns);return ns;
}

bool Ath10kPCIDevice::prepareRuntimePower()
{
    // Hook before the MSI provider is enabled; polling fallback holds wake.
    if(!_runtimeWakeLock)_runtimeWakeLock=IOSimpleLockAlloc();
    if(!_runtimeWakeLock)return false;
    _runtimePower.awake=true;_runtimePower.asserted=_runtimeWakeAsserted;
    _runtimePowerCleanup=false;
    _regs.setAccessHook(runtimeMMIOBegin,runtimeMMIOEnd,this);
    return true;
}
bool Ath10kPCIDevice::runtimeMMIOBegin(void *context,UInt32 offset,UInt64 &token)
{
    auto *self=static_cast<Ath10kPCIDevice *>(context);const auto &l=Ath10k::kQca6174RegisterLayout;
    if(offset==Ath10k::socWakeOffset(l) || offset==Ath10k::rtcStateOffset(l))return true;
    if(!self || !self->_runtimeWakeLock)return false;
    const IOInterruptState state=IOSimpleLockLockDisableInterrupt(self->_runtimeWakeLock);
    token=static_cast<UInt64>(state);
    const bool ok=(!self->powerIOBlocked() || self->_runtimePowerCleanup) && self->_runtimePower.wake(
        [self](UInt32 o,UInt32 &v){return self->_regs.read32Direct(o,v);},
        [self](UInt32 o,UInt32 v){return self->_regs.write32Direct(o,v);},[](UInt32 us){IODelay(us);});
    self->_runtimeWakeAsserted=self->_runtimePower.asserted;
    if(!ok)IOSimpleLockUnlockEnableInterrupt(self->_runtimeWakeLock,state);
    // Retain the lock across the actual MMIO. A check followed by unlocking
    // would race the work-loop's sleep against an ISR's register access.
    return ok;
}
void Ath10kPCIDevice::runtimeMMIOEnd(void *context,UInt32 offset,UInt64 token)
{
    const auto &l=Ath10k::kQca6174RegisterLayout;
    if(offset==Ath10k::socWakeOffset(l) || offset==Ath10k::rtcStateOffset(l))return;
    auto *self=static_cast<Ath10kPCIDevice *>(context);
    IOSimpleLockUnlockEnableInterrupt(self->_runtimeWakeLock,static_cast<IOInterruptState>(token));
}
bool Ath10kPCIDevice::runtimeASPM(bool enabled)
{
    if(!_pci || !_hasPCIe || !_pcieCapOffset || !_aspmDisableAttempted || _pcieLinkControlBefore==0xffff)return !enabled;
    const UInt8 offset=static_cast<UInt8>(_pcieCapOffset+0x10);
    const UInt16 current=_pci->configRead16(offset);if(current==0xffff)return false;
    // Only restore the platform's original L0s/L1 bits. Never enable modes
    // that firmware/BIOS disabled, and preserve all other Link Control bits.
    const UInt16 bits=enabled?(_pcieLinkControlBefore&3):0;
    _pci->configWrite16(offset,static_cast<UInt16>((current&~3u)|bits));
    const UInt16 after=_pci->configRead16(offset);
    // A platform that drops ASPM writes (QEMU/VFIO: "write-ignored-continued" at
    // bring-up) also fails this readback. Treat it as "not controllable", not as a
    // hardware fault: failing here made every runtime stop set _dmaQuarantine, and
    // quarantine is exactly what blocks both restart paths, so one teardown used to
    // brick Wi-Fi until reboot.
    const bool applied=(after!=0xffff && (after&3)==bits);
    if(!applied)setProperty("Ath10kRuntimeASPMReadback","write-ignored");
    _runtimeASPM=enabled && applied && bits;
    return true;
}
bool Ath10kPCIDevice::finishRuntimePower()
{
    if(!_runtimeWakeLock)return true;
    const IOInterruptState state=IOSimpleLockLockDisableInterrupt(_runtimeWakeLock);
    _runtimePowerCleanup=true;_runtimePower.enabled=false;
    const bool awake=_runtimePower.wake([this](UInt32 o,UInt32 &v){return _regs.read32Direct(o,v);},
        [this](UInt32 o,UInt32 v){return _regs.write32Direct(o,v);},[](UInt32 us){IODelay(us);});
    _runtimeWakeAsserted=_runtimePower.asserted;
    IOSimpleLockUnlockEnableInterrupt(_runtimeWakeLock,state);
    const bool aspm=runtimeASPM(false);
    return awake && aspm;
}
const char *Ath10kPCIDevice::idleRuntimePower(bool pending,bool &asleep)
{
    asleep=false;if(!_runtimeWakeLock)return nullptr;
    const auto &s=_connection;
    const bool enabled=_wmiLive.active && _runtimeIRQArmed && s.radio.requestedPS && s.radio.appliedPS && !powerIOBlocked();
    if(enabled!=_runtimePower.enabled){
        if(!runtimeASPM(enabled))return "runtime-aspm-readback";
        const IOInterruptState state=IOSimpleLockLockDisableInterrupt(_runtimeWakeLock);
        _runtimePower.enabled=enabled;_runtimePower.activeMs=Ath10k::uptimeNS()/1000000u;
        IOSimpleLockUnlockEnableInterrupt(_runtimeWakeLock,state);
    }
    const bool permitted=enabled && !pending && s.stage==Ath10k::LinkStage::Connected && s.portAuthorized &&
        !s.cancelRequested && s.disconnect==Ath10k::DisconnectStage::None && !s.wmiOwned && !s.txOwned && !s.dataPending &&
        !s.rekeyKind && !s.eapolPause && !s.txPauseMask && !s.radio.step && !s.radio.blocked && !s.radio.powerBlocked &&
        !s.stats.step && !s.maintenance.step && !s.maintenance.blocked && !s.probe.active && s.csa.phase==Ath10k::ChannelSwitchPhase::Idle &&
        _wmiScan.completed && !_wmiScan.txOwned && !_wmi.credits.outstanding && !_rxSetup.pendingMPDUs && !_rxSetup.rxDoneConsecutiveWaits && !_dmaQuarantine;
    const IOInterruptState state=IOSimpleLockLockDisableInterrupt(_runtimeWakeLock);
    const bool idle=_runtimePower.idle(Ath10k::uptimeNS()/1000000u,permitted,s.rxPackets,s.txPackets,_wmiLive.rxCount);
    const bool ok=!idle || _runtimePower.sleep([this](UInt32 o,UInt32 &v){return _regs.read32Direct(o,v);},
        [this](UInt32 o,UInt32 v){return _regs.write32Direct(o,v);});
    _runtimeWakeAsserted=_runtimePower.asserted;asleep=ok && !_runtimePower.awake;
    IOSimpleLockUnlockEnableInterrupt(_runtimeWakeLock,state);
    return ok?nullptr:"runtime-target-sleep";
}
void Ath10kPCIDevice::publishRuntimePower()
{
    Ath10k::RuntimePowerState snapshot;
    if(_runtimeWakeLock){const IOInterruptState state=IOSimpleLockLockDisableInterrupt(_runtimeWakeLock);
        snapshot=_runtimePower;IOSimpleLockUnlockEnableInterrupt(_runtimeWakeLock,state);}
    setProperty("Ath10kPCIeIdleEnabled",snapshot.enabled);
    setProperty("Ath10kPCIeTargetAwake",snapshot.awake);
    setProperty("Ath10kPCIeTargetWakes",snapshot.wakes,32);
    setProperty("Ath10kPCIeTargetSleeps",snapshot.sleeps,32);
    setProperty("Ath10kPCIeWakeFailures",snapshot.failures,32);
    setProperty("Ath10kPCIeRuntimeASPM",_runtimeASPM);
}

// MSI/INTx top half: mask the QCA PCIe notification, then schedule the shared
// work loop. No DMA sync, mbuf allocation, parsing or queue service here.
// Linux v6.12 pci.c: ath10k_pci_interrupt_handler / ath10k_pci_napi_poll.
// The filter can run on another CPU while the gated worker re-enables MSI.
// Serialize target register transactions, including the posted-write readback.
// Disable local interrupts while holding this short spin lock; otherwise a
// same-CPU filter could deadlock against the interrupted worker. No event-source
// enable/disable, allocation, DMA sync or work-loop action runs under this lock.
bool Ath10kPCIDevice::maskRuntimeWorkerIRQ()
{
    if(!_runtimeIRQLock)return false;
    const IOInterruptState state=IOSimpleLockLockDisableInterrupt(_runtimeIRQLock);
    const bool ok=powerIOBlocked() || maskRuntimeWorkerIRQLocked();
    IOSimpleLockUnlockEnableInterrupt(_runtimeIRQLock,state);
    return ok;
}
bool Ath10kPCIDevice::unmaskRuntimeWorkerIRQ()
{
    if(!_runtimeIRQLock)return false;
    const IOInterruptState state=IOSimpleLockLockDisableInterrupt(_runtimeIRQLock);
    const bool ok=powerIOBlocked() || (_runtimeIRQArmed && unmaskRuntimeWorkerIRQLocked());
    IOSimpleLockUnlockEnableInterrupt(_runtimeIRQLock,state);
    return ok;
}
bool Ath10kPCIDevice::maskRuntimeWorkerIRQLocked()
{
    const auto &l=Ath10k::kQca6174RegisterLayout;
    UInt32 core=0,flush=0;
    return _regs.write32(Ath10k::socCoreOffset(l,Ath10k::kPCIeIntrEnableAddress),0) &&
        _regs.write32(Ath10k::socCoreOffset(l,Ath10k::kPCIeIntrClearAddress),
            Ath10k::kPCIeIntrFirmwareMask|Ath10k::kPCIeIntrCEMaskAll) &&
        _regs.read32(Ath10k::socCoreOffset(l,Ath10k::kCoreCtrlAddress),core) &&
        _regs.write32(Ath10k::socCoreOffset(l,Ath10k::kCoreCtrlAddress),core&~Ath10k::kCoreCtrlPCIeReg31Mask) &&
        _regs.read32(Ath10k::socCoreOffset(l,Ath10k::kPCIeIntrEnableAddress),flush) && flush==0;
}
bool Ath10kPCIDevice::unmaskRuntimeWorkerIRQLocked()
{
    const auto &l=Ath10k::kQca6174RegisterLayout;
    UInt32 core=0,flush=0;
    const UInt32 mask=Ath10k::kPCIeIntrFirmwareMask|Ath10k::kPCIeIntrCEMaskAll;
    _runtimeIRQUnmaskStep=1;
    if(!_regs.write32(Ath10k::socCoreOffset(l,Ath10k::kPCIeIntrEnableAddress),mask))return false;
    _runtimeIRQUnmaskStep=2;
    if(!_regs.read32(Ath10k::socCoreOffset(l,Ath10k::kCoreCtrlAddress),core))return false;
    _runtimeIRQUnmaskStep=3;
    if(!_regs.write32(Ath10k::socCoreOffset(l,Ath10k::kCoreCtrlAddress),core|Ath10k::kCoreCtrlPCIeReg31Mask))return false;
    _runtimeIRQUnmaskStep=4;
    if(!_regs.read32(Ath10k::socCoreOffset(l,Ath10k::kPCIeIntrEnableAddress),flush))return false;
    _runtimeIRQUnmaskEnableValue=flush;_runtimeIRQUnmaskStep=5;
    if(flush!=mask)return false;
    _runtimeIRQUnmaskStep=0;return true;
}
bool Ath10kPCIDevice::runtimeIRQFilter(OSObject *owner,IOFilterInterruptEventSource *)
{
    auto *self=OSDynamicCast(Ath10kPCIDevice,owner);
    if(!self || !self->_runtimeIRQLock)return false;
    const IOInterruptState state=IOSimpleLockLockDisableInterrupt(self->_runtimeIRQLock);
    if(!self->_runtimeIRQArmed || self->powerIOBlocked()){
        IOSimpleLockUnlockEnableInterrupt(self->_runtimeIRQLock,state);return false;
    }
    if(!self->_irqMessaged){
        UInt32 cause=0;
        if(!self->_regs.read32(Ath10k::socCoreOffset(Ath10k::kQca6174RegisterLayout,Ath10k::kPCIeIntrCauseAddress),cause) || cause==0xffffffffu){
            self->_runtimeIRQMaskFailed=true;
            IOSimpleLockUnlockEnableInterrupt(self->_runtimeIRQLock,state);return true;
        }
        if(!(cause&(Ath10k::kPCIeIntrFirmwareMask|Ath10k::kPCIeIntrCEMaskAll))){
            IOSimpleLockUnlockEnableInterrupt(self->_runtimeIRQLock,state);return false;
        }
    }
    OSIncrementAtomic(reinterpret_cast<volatile SInt32 *>(&self->_irqObservedCount));
    if(!self->maskRuntimeWorkerIRQLocked())self->_runtimeIRQMaskFailed=true;
    IOSimpleLockUnlockEnableInterrupt(self->_runtimeIRQLock,state);
    return true;
}
const char *Ath10kPCIDevice::startRuntimeWorkerIRQ()
{
    if(powerIOBlocked())return "runtime-power-blocked";
    if(!ATH10K_RUNTIME_IRQ)return nullptr;
    if(!_pci || !_irqPrepared || _irqIndex<0 || !_wmiWorkLoop)return "runtime-irq-prerequisite";
    if(_runtimeIRQ)return nullptr;
    if(!_runtimeIRQLock)_runtimeIRQLock=IOSimpleLockAlloc();
    if(!_runtimeIRQLock)return "runtime-irq-lock-allocation";
    // Bootstrap registered a raw handler; hand it over only while both the
    // provider and the QCA target are masked. Its registration remains valid
    // until the replacement source has been constructed.
    if(!maskRuntimeWorkerIRQ())return "runtime-irq-mask";
    if(_irqRegistered){
        if(_pci->unregisterInterrupt(_irqIndex)!=kIOReturnSuccess)return "runtime-irq-handoff";
        _irqRegistered=false;
    }
    _runtimeIRQ=IOFilterInterruptEventSource::filterInterruptEventSource(
        this,runtimeIRQAction,runtimeIRQFilter,_pci,_irqIndex);
    if(!_runtimeIRQ)return "runtime-irq-allocation";
    _runtimeIRQ->disable();
    if(_wmiWorkLoop->addEventSource(_runtimeIRQ)!=kIOReturnSuccess)return "runtime-irq-add";
    _runtimeIRQAdded=true;
    for(UInt32 ce=1;ce<=3;++ce){
        const UInt32 base=Ath10k::ceBaseAddress(ce);UInt32 value=0;
        // qcax host_is at +0x30 is write-one-to-clear, before servicing.
        if(!_regs.write32(base+Ath10k::kCEHostIS,Ath10k::kCEHostInterruptMask) ||
           !_regs.write32(base+Ath10k::kCEHostIE,Ath10k::kCEHostCopyCompleteMask) ||
           !_regs.read32(base+Ath10k::kCEHostIE,value) || value!=Ath10k::kCEHostCopyCompleteMask)
            return "runtime-irq-ce-enable";
    }
    // CE4 deliberately stays polling-only, matching Linux's host config.
    // Firmware TX completions arrive on CE1 and wake the same worker.
    const IOInterruptState state=IOSimpleLockLockDisableInterrupt(_runtimeIRQLock);
    _runtimeIRQMaskFailed=false;_runtimeIRQArmed=true;
    IOSimpleLockUnlockEnableInterrupt(_runtimeIRQLock,state);
    _runtimeIRQ->enable();
    if(!_irqMessaged){
        const UInt16 command=_pci->configRead16(kIOPCIConfigCommand);
        if(command==0xffffu)return "runtime-intx-pci-command";
        _pci->configWrite16(kIOPCIConfigCommand,command&~UInt16(0x400));
        if(_pci->configRead16(kIOPCIConfigCommand)&0x400)return "runtime-intx-pci-enable";
    }
    if(!unmaskRuntimeWorkerIRQ())return "runtime-irq-unmask";
    _runtimeInterruptsEnabled=true;_runtimeStatus=_irqMessaged?"wmi-msi-worker":"wmi-intx-worker";
    return nullptr;
}
void Ath10kPCIDevice::stopRuntimeWorkerIRQ()
{
    if(_runtimeIRQLock){
        const IOInterruptState state=IOSimpleLockLockDisableInterrupt(_runtimeIRQLock);
        _runtimeIRQArmed=false;
        IOSimpleLockUnlockEnableInterrupt(_runtimeIRQLock,state);
    }else _runtimeIRQArmed=false;
    if(_runtimeIRQ)_runtimeIRQ->disable();
    _runtimeIRQDeferred=false;
    // The caller's existing stopRuntimeCEIRQ() masks target causes, verifies
    // bus master/reset state and retains mappings on ambiguous failure.
}
void Ath10kPCIDevice::runtimeIRQAction(OSObject *owner,IOInterruptEventSource *source,int)
{
    auto *self=OSDynamicCast(Ath10kPCIDevice,owner);
    if(!self || source!=self->_runtimeIRQ || !self->_wmiLive.active || !self->_runtimeIRQArmed || self->powerIOBlocked())return;
    ++self->_runtimeIRQWorkers;
    if(self->_runtimeIRQMaskFailed){self->endWMIRuntime("runtime-irq-mask");return;}
    for(UInt32 ce=1;ce<=3;++ce)
        if(!self->_regs.write32(Ath10k::ceBaseAddress(ce)+Ath10k::kCEHostIS,Ath10k::kCEHostInterruptMask)){
            self->endWMIRuntime("runtime-irq-ack");return;
        }
    const char *error=self->serviceRuntime();
    if(error){self->endWMIRuntime(error);return;}
    UInt32 summary=0;
    if(!self->_regs.read32(Ath10k::kQca6174RegisterLayout.ceWrapperBaseAddress,summary)){
        self->endWMIRuntime("runtime-irq-summary");return;
    }
    // Recheck after processing to close the MSI lost-wakeup window. Defer
    // remaining work to the next bounded timer action (yield to output).
    self->_runtimeIRQDeferred=self->_rxSetup.pendingMPDUs || ((summary>>8)&0x0e);
    if(self->_runtimeIRQDeferred){
        ++self->_runtimeIRQReschedules;
        if(self->_wmiTimer->setTimeoutMS(1)!=kIOReturnSuccess){self->endWMIRuntime("runtime-timer-failed");return;}
    }else if(!self->unmaskRuntimeWorkerIRQ()){self->endWMIRuntime("runtime-irq-unmask");return;}
    bool asleep=false;error=self->idleRuntimePower(self->_runtimeIRQDeferred,asleep);
    if(error)self->endWMIRuntime(error);
}

// Reserve worker uses a different work loop. The short lock protects only
// mbuf ownership and counts; allocate/free/copy/input never hold it.
bool Ath10kPCIDevice::prepareRXReserve()
{
    if(!ATH10K_RX_RESERVE)return true;
    _rxReserveLock=IOSimpleLockAlloc();
    if(!_rxReserveLock)return false;
    _rxReserveWorkLoop=IOWorkLoop::workLoop();
    if(!_rxReserveWorkLoop)return false;
    _rxReserveTimer=IOTimerEventSource::timerEventSource(
        kIOTimerEventSourceOptionsPriorityWorkLoop,this,rxReserveTimerFired);
    if(!_rxReserveTimer || _rxReserveWorkLoop->addEventSource(_rxReserveTimer)!=kIOReturnSuccess)return false;
    _rxReserveAdded=true;
    // Before starting data service, prime a bounded reserve.
    for(UInt32 i=0;i<Ath10k::kRXReserveCapacity/Ath10k::kRXReserveRefillBudget;++i)refillRXReserve();
    return _rxReserveTimer->setTimeoutMS(10)==kIOReturnSuccess;
}
void Ath10kPCIDevice::refillRXReserve()
{
    if(!_rxReserveLock)return;
    for(UInt32 i=0;i<Ath10k::kRXReserveRefillBudget;++i){
        IOSimpleLockLock(_rxReserveLock);const bool full=_rxReserveCount==Ath10k::kRXReserveCapacity;
        IOSimpleLockUnlock(_rxReserveLock);if(full)break;
        mbuf_t packet=allocatePacket(Ath10k::kEthernetFrameMax);
        if(!packet || mbuf_next(packet) || mbuf_len(packet)<Ath10k::kEthernetFrameMax){
            if(packet)freePacket(packet);
            IOSimpleLockLock(_rxReserveLock);++_perf.reserveFailures;IOSimpleLockUnlock(_rxReserveLock);break;
        }
        IOSimpleLockLock(_rxReserveLock);
        _rxReserve[_rxReserveCount++]=packet;++_perf.reserveRefills;
        IOSimpleLockUnlock(_rxReserveLock);
    }
}
void Ath10kPCIDevice::rxReserveTimerFired(OSObject *owner,IOTimerEventSource *timer)
{
    auto *self=OSDynamicCast(Ath10kPCIDevice,owner);
    if(!self || timer!=self->_rxReserveTimer)return;
    self->refillRXReserve();
    IOSimpleLockLock(self->_rxReserveLock);const bool low=self->_rxReserveCount<Ath10k::kRXReserveLowWater;
    IOSimpleLockUnlock(self->_rxReserveLock);
    // Allocation failure is recoverable: RX can allocate directly meanwhile.
    if(timer->setTimeoutMS(low?1:10)!=kIOReturnSuccess){
        IOSimpleLockLock(self->_rxReserveLock);++self->_perf.reserveFailures;IOSimpleLockUnlock(self->_rxReserveLock);
    }
}
mbuf_t Ath10kPCIDevice::takeRXPacket(UInt32 length)
{
    mbuf_t packet=nullptr;
    if(_rxReserveLock && length<=Ath10k::kEthernetFrameMax){
        IOSimpleLockLock(_rxReserveLock);
        if(_rxReserveCount){packet=_rxReserve[--_rxReserveCount];_rxReserve[_rxReserveCount]=nullptr;++_perf.reserveHits;}
        else ++_perf.reserveMisses;
        IOSimpleLockUnlock(_rxReserveLock);
    }
    if(!packet)return allocatePacket(length);
    // Reserve entries are deliberately single mbufs, so both lengths can
    // shrink without leaving trailing data in an mbuf chain.
    mbuf_setlen(packet,length);mbuf_pkthdr_setlen(packet,length);return packet;
}
IOReturn Ath10kPCIDevice::stopRXReserve(OSObject *owner,void *,void *,void *,void *)
{
    auto *self=OSDynamicCast(Ath10kPCIDevice,owner);
    if(self && self->_rxReserveTimer){self->_rxReserveTimer->cancelTimeout();self->_rxReserveTimer->disable();}
    return kIOReturnSuccess;
}
void Ath10kPCIDevice::pauseRXReserve()
{
    if(_rxReserveWorkLoop)_rxReserveWorkLoop->runAction(stopRXReserve,this);
}
void Ath10kPCIDevice::releaseRXReserve()
{
    pauseRXReserve();
    if(_rxReserveTimer){
        _rxReserveTimer->cancelTimeout();_rxReserveTimer->disable();
        if(_rxReserveAdded)_rxReserveWorkLoop->removeEventSource(_rxReserveTimer);
        _rxReserveTimer->release();_rxReserveTimer=nullptr;_rxReserveAdded=false;
    }
    if(_rxReserveWorkLoop){_rxReserveWorkLoop->release();_rxReserveWorkLoop=nullptr;}
    // Runtime is already stopped and the reserve worker has been drained.
    for(UInt32 i=0;i<_rxReserveCount;++i){freePacket(_rxReserve[i]);_rxReserve[i]=nullptr;}
    _rxReserveCount=0;
    if(_rxReserveLock){IOSimpleLockFree(_rxReserveLock);_rxReserveLock=nullptr;}
}
