#include "Ath10kPCIDevice.hpp"
#include "Ath10kPayload.hpp"
#include "Ath10kDiagnostics.hpp"
#include <IOKit/IOLib.h>
#include <mach/kmod.h>
extern "C" kmod_info_t kmod_info;
#include <string.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSData.h>

#ifdef ATH10K_NATIVE
#define super IO80211Controller
OSDefineMetaClassAndStructors(Ath10kPCIDevice, IO80211Controller)
#else
#define super IOEthernetController
OSDefineMetaClassAndStructors(Ath10kPCIDevice, IOEthernetController)
#endif

// Standard PCI configuration-space offsets/capability IDs.
static constexpr UInt8 kPCIConfigProgIf        = 0x09;
static constexpr UInt8 kPCIConfigSubclass      = 0x0a;
static constexpr UInt8 kPCIConfigClassCode     = 0x0b;
static constexpr UInt8 kPCIConfigCapabilities  = 0x34;
static constexpr UInt8 kPCIConfigInterruptLine = 0x3c;
static constexpr UInt8 kPCIConfigInterruptPin  = 0x3d;

static constexpr UInt16 kPCIStatusCapabilitiesList = 0x0010;
static constexpr UInt8 kPCICapPowerManagement = 0x01;
static constexpr UInt8 kPCICapMSI             = 0x05;
static constexpr UInt8 kPCICapPCIExpress      = 0x10;
static constexpr UInt8 kPCICapMSIX            = 0x11;

// Standard PCIe / MSI capability-relative fields.
static constexpr UInt8  kPCIeLinkControlOffset = 0x10;
static constexpr UInt16 kPCIeLinkControlASPMMask = 0x0003; // L0s | L1
static constexpr UInt8  kMSIMessageControlOffset = 0x02;
static constexpr UInt16 kMSIControlEnable = 0x0001;
static constexpr UInt16 kMSIControl64Bit = 0x0080;
static constexpr UInt16 kMSIControlPerVectorMask = 0x0100;

static void ath10kSetRecoveryStatus(IOService *provider, const char *phase, const char *detail)
{
    if (!provider)
        return;
    provider->setProperty("Ath10kRecoveryPhase", phase);
    if (detail)
        provider->setProperty("Ath10kRecoveryDetail", detail);
}

static bool ath10kBytesEqual(const void *lhs, const void *rhs, UInt32 length)
{
    if (!lhs || !rhs)
        return false;
    const auto *a = reinterpret_cast<const UInt8 *>(lhs);
    const auto *b = reinterpret_cast<const UInt8 *>(rhs);
    for (UInt32 i = 0; i < length; ++i) {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

// Match against the provider's cached identity. A rematched instance can be
// probed before its power parent permits configuration/MMIO transactions.
static bool ath10kRegistryPCIID(IOService *provider,const char *key,UInt16 &value)
{
    auto property=provider->copyProperty(key);
    bool valid=false;UInt64 candidate=0;
    if(auto data=OSDynamicCast(OSData,property)){
        if(data->getLength()==4 && data->getBytesNoCopy()){
            const auto bytes=static_cast<const UInt8 *>(data->getBytesNoCopy());
            candidate=UInt32(bytes[0])|(UInt32(bytes[1])<<8)|(UInt32(bytes[2])<<16)|(UInt32(bytes[3])<<24);
            valid=candidate<=0xffffu;
        }
    }else if(auto number=OSDynamicCast(OSNumber,property)){
        candidate=number->unsigned64BitValue();valid=candidate<=0xffffu;
    }
    if(property)property->release();
    if(valid)value=static_cast<UInt16>(candidate);
    return valid;
}

#ifdef ATH10K_NATIVE
static bool ath10kNativePCIIdentity(UInt16 vendor,UInt16 device,UInt16 subsystemVendor,UInt16 subsystem)
{
    return vendor==0x168c && ((device==0x42 && subsystemVendor==0x1a3b && subsystem==0x2b51) ||
        (device==0x3e && subsystemVendor==0x11ad && subsystem==0x0807));
}
#endif

IOService *Ath10kPCIDevice::probe(IOService *provider, SInt32 *score)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci)
        return nullptr;

    UInt16 vendor=0,device=0;
    if(!ath10kRegistryPCIID(pci,"vendor-id",vendor) ||
       !ath10kRegistryPCIID(pci,"device-id",device))return nullptr;

    ATH10K_TRACE("Ath10kMac: probe vendor=%04x device=%04x\n", vendor, device);

    if (!Ath10k::isSupportedDevice(vendor, device))
        return nullptr;

#ifdef ATH10K_NATIVE
    // Reject foreign subsystem/OS before opening PCI or starting firmware DMA.
    UInt16 subsystemVendor=0,subsystem=0;
    if(version_major!=24 || version_minor<2 ||
       !ath10kRegistryPCIID(pci,"subsystem-vendor-id",subsystemVendor) ||
       !ath10kRegistryPCIID(pci,"subsystem-id",subsystem) ||
       !ath10kNativePCIIdentity(vendor,device,subsystemVendor,subsystem))return nullptr;
    // OpenCore diagnostic personality: verify matching without entering the
    // controller, CoreCapture, PCI-open or firmware/DMA startup paths.
    if (auto *probeOnly = OSDynamicCast(OSBoolean, getProperty("Ath10kProbeOnly"))) {
        if (probeOnly->isTrue()) {
            provider->setProperty("Ath10kNativeStartupPhase", "opencore-probe-only");
            ATH10K_TRACE("Ath10kMac: OpenCore probe-only matched; controller startup skipped\n");
            return nullptr;
        }
    }
#endif
    if (score)
        *score += 1000;

    return this;
}

bool Ath10kPCIDevice::start(IOService *provider)
{
    if(auto *country=OSDynamicCast(OSString,getProperty("Ath10kCountryCode"))){
        const char *value=country->getCStringNoCopy();
        _regulatory.country=Ath10k::regulatoryCountry(value);
    }
    if(auto *indoor=OSDynamicCast(OSBoolean,getProperty("Ath10kIndoor")))_regulatory.indoor=indoor->isTrue();
    if(auto *power=OSDynamicCast(OSNumber,getProperty("Ath10kTxPowerDbm"))){
        const UInt32 value=power->unsigned32BitValue();
        _regulatory.ceilingHalfDbm=static_cast<UInt8>(value>=1 && value<=10?2*value:0);
    }
    if(auto *dfs=OSDynamicCast(OSBoolean,getProperty("Ath10kStationDFS")))_regulatory.stationDFS=dfs->isTrue();
    if(auto *timeout=OSDynamicCast(OSNumber,getProperty("Ath10kBeaconTimeoutMs"))){
        const UInt32 value=timeout->unsigned32BitValue();_connection.beaconTimeoutMs=value>=10000 && value<=120000?value:30000;
    }
    IOLog("Ath10kMac: start v%s (deferred device initialization)\n", kmod_info.version);

#ifdef ATH10K_NATIVE
    if (version_major!=24 || version_minor<2) return false;
    UInt32 diagnosticStage = 0;
    if (auto *stage = OSDynamicCast(OSNumber, getProperty("Ath10kDiagnosticStage")))
        diagnosticStage = stage->unsigned32BitValue();
    if (diagnosticStage > 3) return false;
    nativeStartupPhase("workqueue-create");
    if (!_networkWorkLoop && !createWorkLoop()) return false;
    if (diagnosticStage == 1) {
        provider->setProperty("Ath10kNativeStartupPhase", "opencore-workqueue-passed");
        ATH10K_TRACE("Ath10kMac: diagnostic workqueue passed; controller startup skipped\n");
        return false;
    }
    nativeStartupPhase("corecapture-init");
    if (!prepareNativeLogs()) {releaseNativeLogs();return false;}
    if (diagnosticStage == 2) {
        releaseNativeLogs();
        provider->setProperty("Ath10kNativeStartupPhase", "opencore-corecapture-passed");
        ATH10K_TRACE("Ath10kMac: diagnostic CoreCapture passed; controller startup skipped\n");
        return false;
    }
    nativeStartupPhase("controller-legacy-thread-check");
    const bool legacyThreadReady=getWorkLoop() && getWorkLoop()->getThread();
    const bool queuesSeparated=getWorkLoop()!=getWorkQueue();
    provider->setProperty("Ath10kLegacyWorkLoopHasThread",legacyThreadReady);
    provider->setProperty("Ath10kWorkQueuesSeparated",queuesSeparated);
    if (!legacyThreadReady || !queuesSeparated) {
        IOLog("Ath10kMac: refusing controller start: legacy thread=%d separate queues=%d\n",
              legacyThreadReady,queuesSeparated);
        return false;
    }
    nativeStartupPhase("controller-start");
#endif
    if (!super::start(provider)) {
        IOLog("Ath10kMac: super::start failed\n");
        return false;
    }

#ifdef ATH10K_NATIVE
    nativeStartupPhase("controller-started");
    if (diagnosticStage == 3) {
        provider->setProperty("Ath10kNativeStartupPhase", "opencore-controller-stopping");
        super::stop(provider);
        releaseNativeLogs();
        provider->setProperty("Ath10kNativeStartupPhase", "opencore-controller-passed");
        ATH10K_TRACE("Ath10kMac: diagnostic controller start/stop passed; PCI startup skipped\n");
        return false;
    }
#endif
    _pci = OSDynamicCast(IOPCIDevice, provider);
    if (!_pci) {
        ATH10K_TRACE("Ath10kMac: provider is not IOPCIDevice\n");
        return false;
    }
    ath10kSetRecoveryStatus(_pci, "driver-starting", "Ath10kMac matched the PCI device");

    if (!_pci->open(this)) {
        IOLog("Ath10kMac: failed to open PCI provider\n");
        _pci = nullptr;
        return false;
    }

    // A rematch can occur while the parent is off. Allocate all schedulers
    // before PM registration and wait for an actual On callback before PCI I/O.
    _power.deviceOn=false;_powerIOBlock=1;
    _probePowerLock=IOLockAlloc();
    _deferredProbeLock=IOLockAlloc();
    if(_deferredProbeLock)_deferredProbeCall=thread_call_allocate_with_options(
        deferredProbeCall,this,THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
    if(_deferredProbeLock)_wakeRecoveryCall=thread_call_allocate_with_options(
        wakeRecoveryCall,this,THREAD_CALL_PRIORITY_KERNEL,THREAD_CALL_OPTIONS_ONCE);
    if(!_probePowerLock || !_deferredProbeLock || !_deferredProbeCall || !_wakeRecoveryCall ||
       !getWorkLoop() || !preparePowerNotifications()){
        stop(provider);return false;
    }
    ath10kSetRecoveryStatus(_pci,"awaiting-initial-power","Waiting for the parent's initial power transition");
    registerService();
    PMinit();_powerManaged=true;
    static IOPMPowerState states[2]={
        {1,0,0,0,0,0,0,0,0,0,0,0},
        {1,kIOPMDeviceUsable,0,kIOPMPowerOn,0,0,0,0,0,0,0,0}
    };
    // Do not set kIOPMInitialDeviceState: the initial callback is our permission.
    if(registerPowerDriver(this,states,2)!=kIOReturnSuccess){stop(provider);return false;}
    _pci->joinPMtree(this);
    if(changePowerStateTo(1)!=kIOReturnSuccess){stop(provider);return false;}
    return true;
}

bool Ath10kPCIDevice::preparePoweredPCI()
{
    if(!identifyDevice())return false;
    _originalCommand=_pci->configRead16(kIOPCIConfigCommand);_pciConfigured=true;
    _pci->setMemoryEnable(true);_pci->setBusMasterEnable(true);
    dumpPCIConfig();dumpPCICapabilities();
    if(!mapBAR0())return false;
    _regs.attach(_bar0,_bar0Map->getLength());
    validateRegisterLayer();dumpHardwareCandidates();
    return true;
}
void Ath10kPCIDevice::queueDeferredProbe()
{
    // Callers own the initialization barrier. Cancellation never takes that
    // barrier while holding this short scheduler lock.
    if(!_deferredProbeLock || powerIOBlocked())return;
    IOLockLock(_deferredProbeLock);
    if(!_deferredProbeStopping && !_deferredProbeStarted && !_deferredProbePending &&
       _deferredProbeCall && !powerIOBlocked()){
        _deferredProbePending=true;thread_call_enter(_deferredProbeCall);
    }
    IOLockUnlock(_deferredProbeLock);
}

void Ath10kPCIDevice::deferredProbeCall(thread_call_param_t param0, thread_call_param_t)
{
    auto *self = static_cast<Ath10kPCIDevice *>(param0);
    if (self) {
        self->runDeferredProbe();
        IOLockLock(self->_deferredProbeLock);
        self->_deferredProbePending = false;
        // On can arrive after an aborted probe releases the producer barrier
        // but before this tail publishes completion. Do not lose that wake.
        if(!self->_deferredProbeStopping && !self->_deferredProbeStarted && !self->powerIOBlocked()){
            self->_deferredProbePending=true;thread_call_enter(self->_deferredProbeCall);
        }
        IOLockWakeup(self->_deferredProbeLock, &self->_deferredProbePending, false);
        IOLockUnlock(self->_deferredProbeLock);
    }
}

void Ath10kPCIDevice::runDeferredProbe()
{
    // A priority sleep callback blocks new I/O before waiting here. If sleep
    // won the race, do not start firmware or expose a newly initialized port.
    IOLockLock(_probePowerLock);
    if (powerIOBlocked() || _deferredProbeStarted) {
        IOLockUnlock(_probePowerLock);
        ath10kSetRecoveryStatus(_pci, "probe-power-blocked", "Initialization deferred until a clean power recovery");
        return;
    }
    IOLockLock(_deferredProbeLock);_deferredProbeStarted=true;IOLockUnlock(_deferredProbeLock);
    if(!preparePoweredPCI()){
        ath10kSetRecoveryStatus(_pci,"powered-pci-failed","Powered PCI identity/configuration/BAR preparation failed");
        IOLockUnlock(_probePowerLock);return;
    }
    ATH10K_TRACE("Ath10kMac: deferred target probe begin\n");
    ath10kSetRecoveryStatus(_pci, "probe-running", "Firmware and runtime initialization are in progress");
    const char *probeFailure = "target-probe-did-not-run";

    // 0.0.91 probes for a device-specific IOMapper, but a null result is valid
    // on systems without an iommu-parent. IODMACommand::kMapped is still used
    // with the default/null mapper path, and all potentially double-buffered
    // memory is explicitly synchronized around DMA ownership changes.
    if (prepareDeviceDMAMapper()) {
        if (allocateCERings()) {
            const bool targetReady = probeTarget();
            if (!targetReady)
                probeFailure = "target-probe-failed";
            if (!_dmaQuarantine && !_runtimeActive)
                runDMATest();
            else
                IOLog("Ath10kMac: DMA regression test skipped while runtime or quarantine is active\n");
        } else {
            probeFailure = "CE-ring-allocation-failed";
            IOLog("Ath10kMac: CE ring allocation failed; reset/CE transfer test skipped\n");
        }
    } else {
        probeFailure = "DMA-mapper-preparation-failed";
        IOLog("Ath10kMac: DMA mapping policy preparation failed; invasive Phase 2 test skipped\n");
    }

    if(_wmiLive.active && !prepareNetwork())
        _wmiWorkLoop->runAction(controlWMIRuntime,this,const_cast<char *>("network-attach-failed"));
    // No worker runs until probeTarget has transferred its wake/DMA ownership.
    publishDiagnosticProperties();
    IOLockUnlock(_probePowerLock);
    // The parent already admitted On. Runtime arming still checks the atomic
    // block if a new root/device Off arrived while firmware was initializing.
    if(_wmiLive.active && _wmiWorkLoop)_wmiWorkLoop->runAction(controlWMIRuntime,this);
    if (_wmiLive.active && !_dmaQuarantine) {
        ath10kSetRecoveryStatus(_pci, "ready", "Firmware runtime is active; waiting for app scan/connect");
    } else {
        const bool hasRuntimeFailure = _wmiLive.status &&
            strcmp(_wmiLive.status, "not-run") != 0;
        const char *detail = _dmaQuarantine ? "DMA safety quarantine blocked initialization" :
            (hasRuntimeFailure ? _wmiLive.status : probeFailure);
        ath10kSetRecoveryStatus(_pci, "probe-failed", detail);
    }
    IOLog("Ath10kMac: READY %s pci-rev=%02x BAR0=%p len=0x%llx\n",
          chipName(), _revisionId, _bar0,
          (unsigned long long)_bar0Map->getLength());
    if (_selectedHw) {
        ATH10K_TRACE("Ath10kMac: target profile=%s firmware=%s (post-reset chip-id)\n",
              _selectedHw->targetRevision, _selectedHw->firmwareDirectory);
    } else {
        ATH10K_TRACE("Ath10kMac: target profile unresolved after cold-reset diagnostic\n");
    }
    ATH10K_TRACE("Ath10kMac: next=explicit open STA configuration; Ethernet stays down until association\n");
}

void Ath10kPCIDevice::cancelDeferredProbe()
{
    if(!_deferredProbeLock)return;
    IOLockLock(_deferredProbeLock);_deferredProbeStopping=true;
    if(_deferredProbeCall && thread_call_cancel(_deferredProbeCall))_deferredProbePending=false;
    while(_deferredProbePending)IOLockSleep(_deferredProbeLock,&_deferredProbePending,THREAD_UNINT);
    auto call=_deferredProbeCall;_deferredProbeCall=nullptr;
    IOLockUnlock(_deferredProbeLock);
    if(call)thread_call_free(call);
    // PM callbacks can still reach the scheduler until terminal power closure.
    // Keep its lock until final controller free, not just thread-call drain.
}

bool Ath10kPCIDevice::stopDMAForRelease()
{
    if (!_connection.wmiOwned && !_connection.txOwned && !_ceDiagTransferUncertain && !_bmiTransferUncertain &&
        !_rxSetup.txOwned && !_rxSetup.ringOwned && !_htcControlExchangeUncertain && !_wmi.rxOwned && !_wmiInit.txOwned && !_wmiInit.hostMemoryOwned && !_wmiEcho.txOwned && !_wmiVdevCreate.txOwned && !_wmiRadio[0].txOwned && !_wmiRadio[1].txOwned && !_wmiRadio[2].txOwned && !_wmiRadio[3].txOwned && !_wmiRadio[4].txOwned && !_wmiRadio[Ath10k::kWMIPMFQoSIndex].txOwned && !_wmiScanChannels.txOwned && !_wmiScan.txOwned && !_boot.rxOwned && !_dmaQuarantine)
        return true;
    // A failed boot/reset must not restore the original bus-master bit later.
    _dmaQuarantine = true;
    if (!_pci)
        return false;
    _pci->setBusMasterEnable(false);
    OSSynchronizeIO();
    IODelay(1000);
    const UInt16 command = _pci->configRead16(kIOPCIConfigCommand);
    if (command == 0xffffu || (command & 0x0004u)) {
        IOLog("Ath10kMac: DMA release BLOCKED: cannot verify bus mastering disabled; mappings retained until reboot\n");
        return false;
    }
    return true;
}

void Ath10kPCIDevice::drainControlWorkers()
{
    // The probe may still create the native interface and its worker. Finish
    // or cancel that producer before draining the worker it can create.
    blockPowerIOForTermination();
    cancelDeferredProbe();
    cancelWakeRecovery();
#ifdef ATH10K_NATIVE
    if(_nativeInterface)_nativeInterface->drainPowerCalls();
    stopNativeRequests();
#endif
}

void Ath10kPCIDevice::stop(IOService *provider)
{
    drainControlWorkers();
    // Rematching is queued only after every old DMA mapping/IRQ has been
    // stopped and detached. Retain provider until registerService returns.
    IOService *rematch=_reprobeRequested?provider:nullptr;
    if(rematch)rematch->retain();
    if (_reprobeRequested)
        ath10kSetRecoveryStatus(provider, "stop-started", "Driver stop began for a requested recovery rematch");
    ATH10K_TRACE("Ath10kMac: stop %s\n", chipName());
    drainControllerPowerCalls();
    releaseWMIRuntime();
    Ath10k::wipeConnectionSecrets(_connection);
    releaseRXReserve();

    if (_runtimeActive || _runtimeInterruptsEnabled) {
        const bool irqStopOk = stopRuntimeCEIRQ();
        const bool resetOk = irqStopOk && quiesceTargetAfterBMI();
        if (resetOk)
            releaseTargetWake(_runtimeWakeAsserted);
        else
            _dmaQuarantine = true;
        _runtimeWakeAsserted = false;
        IOLog("Ath10kMac: Phase 3 runtime stop irq=%s reset=%s\n",
              irqStopOk ? "yes" : "no", resetOk ? "yes" : "no");
    }

    const bool releaseDMA = stopDMAForRelease();
    if (_reprobeRequested) {
        ath10kSetRecoveryStatus(provider,
            releaseDMA && !_dmaQuarantine ? "stop-dma-released" : "stop-dma-blocked",
            releaseDMA && !_dmaQuarantine ? "DMA ownership was released safely" :
                "Safe DMA release failed; provider rematch was withheld");
    }
    restorePCIeBringupParity();
    if (releaseDMA) {
        _ceDiagBounce.release();
        _bmiRequestBuffer.release();
        _bmiResponseBuffer.release();
        _htcReadyBuffer.release();
        _htcControlBuffer.release();
        _wmiRxBuffer.release();
        _wmiTxBuffer.release();
        for(auto &memory:_wmiHostMemory)memory.release();
        for(auto &buffer:_httRecycled)buffer.release();
        _httTxBuffer.release();_httRxIndex.release();_httRxRing.release();_httRxPool.release();
        _connectionCommand.release();_packetBuffer.release();_packetCommand.release();_packetFragments.release();
        for(auto &buffers:_dataBuffers){buffers.payload.release();buffers.command.release();buffers.fragments.release();}
        _dmaTestBuffer.release();
        releaseCERings();
        releaseDeviceDMAMapper();
    }
    _regs.detach();
    unmapBAR0();

    if (_pci) {
        restoreOwnedPCICommand();
        if (_pci->isOpen(this))
            _pci->close(this);
        _pci = nullptr;
    }

#ifdef ATH10K_NATIVE
    releaseNativeInterface();
#endif
    if(_networkInterface){IONetworkController::detachInterface(_networkInterface,true);_networkInterface->release();_networkInterface=nullptr;}
    super::stop(provider);
    if(rematch){
        if(releaseDMA && !_dmaQuarantine && !rematch->isInactive()) {
            ath10kSetRecoveryStatus(rematch, "rematch-queued", "PCI provider registration queued for a new driver instance");
            rematch->registerService(kIOServiceAsynchronous);
        } else {
            ath10kSetRecoveryStatus(rematch, "rematch-suppressed",rematch->isInactive()?
                "PCI provider is inactive; rematch suppressed":"Driver stopped without a safe DMA release");
        }
        rematch->release();
    }
}

void Ath10kPCIDevice::free()
{
    // free() also covers partial initialization without a preceding stop().
    drainControlWorkers();
    drainControllerPowerCalls();
    releaseWMIRuntime();
    Ath10k::wipeConnectionSecrets(_connection);
    releaseRXReserve();
    if (_runtimeActive || _runtimeInterruptsEnabled) {
        const bool irqStopOk = stopRuntimeCEIRQ();
        const bool resetOk = irqStopOk && quiesceTargetAfterBMI();
        if (resetOk)
            releaseTargetWake(_runtimeWakeAsserted);
        else
            _dmaQuarantine = true;
        _runtimeWakeAsserted = false;
        IOLog("Ath10kMac: Phase 3 runtime free irq=%s reset=%s\n",
              irqStopOk ? "yes" : "no", resetOk ? "yes" : "no");
    }

    const bool releaseDMA = stopDMAForRelease();
    restorePCIeBringupParity();
    if (releaseDMA) {
        _ceDiagBounce.release();
        _bmiRequestBuffer.release();
        _bmiResponseBuffer.release();
        _htcReadyBuffer.release();
        _htcControlBuffer.release();
        _wmiRxBuffer.release();
        _wmiTxBuffer.release();
        for(auto &memory:_wmiHostMemory)memory.release();
        for(auto &buffer:_httRecycled)buffer.release();
        _httTxBuffer.release();_httRxIndex.release();_httRxRing.release();_httRxPool.release();
        _connectionCommand.release();_packetBuffer.release();_packetCommand.release();_packetFragments.release();
        for(auto &buffers:_dataBuffers){buffers.payload.release();buffers.command.release();buffers.fragments.release();}
        _dmaTestBuffer.release();
        releaseCERings();
        releaseDeviceDMAMapper();
    }
    _regs.detach();
    unmapBAR0();
#ifdef ATH10K_NATIVE
    releaseNativeInterface();
    releaseNativeLogs();
#endif
    // Keep both queues alive through the base controller's timer/gate cleanup.
    // super::free() can destroy this object: use only saved local pointers after it.
    IOLock *deferredProbeLock=_deferredProbeLock;
    _deferredProbeLock=nullptr;
    IOLock *probePowerLock=_probePowerLock;
    _probePowerLock=nullptr;
    IOWorkLoop *networkLoop=_networkWorkLoop;
    _networkWorkLoop=nullptr;
#ifdef ATH10K_NATIVE
    IO80211WorkQueue *nativeQueue=_nativeWorkQueue;
    _nativeWorkQueue=nullptr;
#endif
    super::free();
#ifdef ATH10K_NATIVE
    if(nativeQueue)nativeQueue->release();
#endif
    if(networkLoop)networkLoop->release();
    if(probePowerLock)IOLockFree(probePowerLock);
    if(deferredProbeLock)IOLockFree(deferredProbeLock);
}

bool Ath10kPCIDevice::prepareDeviceDMAMapper()
{
    _dmaMapperAttempted = true;
    _dmaMapperReady = false;
    _dmaDeviceMapperAvailable = false;
    _dmaMapperUsedByRings = false;
    _dmaMapperUsedByDiag = false;
    _dmaMapperUsedByTest = false;
    releaseDeviceDMAMapper();

    if (!_pci) {
        IOLog("Ath10kMac: DMA mapping policy ERROR PCI provider unavailable\n");
        return false;
    }

    _dmaMapper = IOMapper::copyMapperForDevice(_pci);
    _dmaDeviceMapperAvailable = (_dmaMapper != nullptr);
    _dmaMapperReady = true;

    if (_dmaMapper) {
        ATH10K_TRACE("Ath10kMac: DMA mapping policy READY device-bound=yes mapper=%p mode=kMapped explicit-sync=required\n",
              _dmaMapper);
    } else {
        ATH10K_TRACE("Ath10kMac: DMA mapping policy READY device-bound=no mapper=null mode=kMapped-default/buffered explicit-sync=required\n");
    }
    return true;
}

void Ath10kPCIDevice::releaseDeviceDMAMapper()
{
    if (_dmaMapper) {
        _dmaMapper->release();
        _dmaMapper = nullptr;
    }
    _dmaMapperReady = false;
    _dmaDeviceMapperAvailable = false;
}

bool Ath10kPCIDevice::identifyDevice()
{
    _vendorId = _pci->configRead16(kIOPCIConfigVendorID);
    _deviceId = _pci->configRead16(kIOPCIConfigDeviceID);
    _revisionId = _pci->configRead8(kIOPCIConfigRevisionID);
    _subsystemVendorId = _pci->configRead16(kIOPCIConfigSubSystemVendorID);
    _subsystemId = _pci->configRead16(kIOPCIConfigSubSystemID);
    _progIf = _pci->configRead8(kPCIConfigProgIf);
    _subclass = _pci->configRead8(kPCIConfigSubclass);
    _classCode = _pci->configRead8(kPCIConfigClassCode);
    _interruptLine = _pci->configRead8(kPCIConfigInterruptLine);
    _interruptPin = _pci->configRead8(kPCIConfigInterruptPin);

#ifdef ATH10K_NATIVE
    // Cached matching properties can be overridden. Validate the real profile
    // before enabling bus mastering, mapping BARs or starting target firmware.
    return ath10kNativePCIIdentity(_vendorId,_deviceId,_subsystemVendorId,_subsystemId);
#else
    return Ath10k::isSupportedDevice(_vendorId, _deviceId);
#endif
}

bool Ath10kPCIDevice::mapBAR0()
{
    _bar0Map = _pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!_bar0Map)
        return false;

    _bar0 = reinterpret_cast<volatile UInt8 *>(_bar0Map->getVirtualAddress());
    if (!_bar0) {
        _bar0Map->release();
        _bar0Map = nullptr;
        return false;
    }

    ATH10K_TRACE("Ath10kMac: BAR0 virtual=%p physical=0x%llx length=0x%llx\n",
          _bar0,
          (unsigned long long)_bar0Map->getPhysicalAddress(),
          (unsigned long long)_bar0Map->getLength());
    return true;
}

void Ath10kPCIDevice::unmapBAR0()
{
    if (_bar0Map) {
        _bar0Map->release();
        _bar0Map = nullptr;
        _bar0 = nullptr;
    }
}

void Ath10kPCIDevice::dumpPCIConfig()
{
    const UInt16 command = _pci->configRead16(kIOPCIConfigCommand);
    const UInt16 status = _pci->configRead16(kIOPCIConfigStatus);
    const UInt32 bar0 = _pci->configRead32(kIOPCIConfigBaseAddress0);
    const UInt32 bar1 = _pci->configRead32(kIOPCIConfigBaseAddress1);

    ATH10K_TRACE("Ath10kMac: PCI %04x:%04x subsystem=%04x:%04x pci-rev=%02x\n",
          _vendorId, _deviceId, _subsystemVendorId, _subsystemId, _revisionId);
    ATH10K_TRACE("Ath10kMac: class=%02x:%02x:%02x irq-line=%02x irq-pin=%02x\n",
          _classCode, _subclass, _progIf, _interruptLine, _interruptPin);
    ATH10K_TRACE("Ath10kMac: PCI command=%04x (original=%04x) status=%04x BAR0=%08x BAR1=%08x\n",
          command, _originalCommand, status, bar0, bar1);
    ATH10K_TRACE("Ath10kMac: NOTE pci-rev=%02x is PCI config revision, not ath10k target chip revision\n",
          _revisionId);
}

void Ath10kPCIDevice::dumpPCICapabilities()
{
    _hasPM = false;
    _hasMSI = false;
    _hasMSIX = false;
    _hasPCIe = false;
    _pmCapOffset = 0;
    _msiCapOffset = 0;
    _msixCapOffset = 0;
    _pcieCapOffset = 0;

    const UInt16 status = _pci->configRead16(kIOPCIConfigStatus);
    if ((status & kPCIStatusCapabilitiesList) == 0) {
        ATH10K_TRACE("Ath10kMac: PCI capabilities list not advertised\n");
        return;
    }

    UInt8 ptr = static_cast<UInt8>(_pci->configRead8(kPCIConfigCapabilities) & 0xfc);
    UInt32 guard = 0;

    while (ptr >= 0x40 && ptr != 0 && guard++ < 48) {
        const UInt8 capId = _pci->configRead8(ptr);
        const UInt8 next = static_cast<UInt8>(_pci->configRead8(ptr + 1) & 0xfc);

        ATH10K_TRACE("Ath10kMac: PCI capability id=%02x offset=%02x next=%02x\n",
              capId, ptr, next);

        switch (capId) {
            case kPCICapPowerManagement:
                _hasPM = true;
                _pmCapOffset = ptr;
                break;
            case kPCICapMSI:
                _hasMSI = true;
                _msiCapOffset = ptr;
                break;
            case kPCICapPCIExpress:
                _hasPCIe = true;
                _pcieCapOffset = ptr;
                break;
            case kPCICapMSIX:
                _hasMSIX = true;
                _msixCapOffset = ptr;
                break;
            default:
                break;
        }

        if (next == ptr)
            break;
        ptr = next;
    }

    ATH10K_TRACE("Ath10kMac: capabilities PM=%s MSI=%s MSI-X=%s PCIe=%s offsets pm=%02x msi=%02x msix=%02x pcie=%02x\n",
          _hasPM ? "yes" : "no",
          _hasMSI ? "yes" : "no",
          _hasMSIX ? "yes" : "no",
          _hasPCIe ? "yes" : "no",
          _pmCapOffset, _msiCapOffset, _msixCapOffset, _pcieCapOffset);
}

bool Ath10kPCIDevice::disableASPMForBringup()
{
    _aspmDisableAttempted = true;
    _aspmDisabled = false;
    _aspmRestoreSucceeded = false;
    _pcieLinkControlBefore = 0xffffu;
    _pcieLinkControlDuring = 0xffffu;
    _pcieLinkControlAfterRestore = 0xffffu;

    if (!_pci || !_hasPCIe || _pcieCapOffset == 0) {
        IOLog("Ath10kMac: PCIe bring-up ERROR PCIe capability unavailable; cannot audit ASPM\n");
        return false;
    }

    const UInt8 offset = static_cast<UInt8>(_pcieCapOffset + kPCIeLinkControlOffset);
    _pcieLinkControlBefore = _pci->configRead16(offset);
    if(_pcieLinkControlBefore==0xffffu)return false;
    const UInt16 aspmBefore = _pcieLinkControlBefore & kPCIeLinkControlASPMMask;
    const UInt16 desired = static_cast<UInt16>(_pcieLinkControlBefore & ~kPCIeLinkControlASPMMask);

    ATH10K_TRACE("Ath10kMac: PCIe Link Control original=0x%04x ASPM L0s=%s L1=%s; disabling ASPM for reset window\n",
          _pcieLinkControlBefore,
          (aspmBefore & 0x1) ? "yes" : "no",
          (aspmBefore & 0x2) ? "yes" : "no");

    _pci->configWrite16(offset, desired);
    _pcieLinkControlDuring = _pci->configRead16(offset);
    _aspmDisabled = ((_pcieLinkControlDuring & kPCIeLinkControlASPMMask) == 0);

    ATH10K_TRACE("Ath10kMac: PCIe Link Control ASPM-disable readback=%s value=0x%04x\n",
          _aspmDisabled ? "PASS" : "FAIL", _pcieLinkControlDuring);
    return _aspmDisabled;
}

void Ath10kPCIDevice::restoreASPM()
{
    if (_aspmRestoreSucceeded || !_aspmDisableAttempted || _pcieLinkControlBefore==0xffffu ||
        !_pci || !_hasPCIe || _pcieCapOffset == 0)
        return;

    const UInt8 offset = static_cast<UInt8>(_pcieCapOffset + kPCIeLinkControlOffset);
    const UInt16 current = _pci->configRead16(offset);
    if(current==0xffffu)return;
    const UInt16 restored = static_cast<UInt16>((current & ~kPCIeLinkControlASPMMask) |
                                                (_pcieLinkControlBefore & kPCIeLinkControlASPMMask));
    _pci->configWrite16(offset, restored);
    _pcieLinkControlAfterRestore = _pci->configRead16(offset);
    _aspmRestoreSucceeded = _pcieLinkControlAfterRestore!=0xffffu &&
                            ((_pcieLinkControlAfterRestore & kPCIeLinkControlASPMMask) ==
                             (_pcieLinkControlBefore & kPCIeLinkControlASPMMask));

    ATH10K_TRACE("Ath10kMac: PCIe Link Control restore readback=%s value=0x%04x original-ASPM=0x%x\n",
          _aspmRestoreSucceeded ? "PASS" : "FAIL",
          _pcieLinkControlAfterRestore,
          _pcieLinkControlBefore & kPCIeLinkControlASPMMask);
}

void Ath10kPCIDevice::restoreOwnedPCICommand()
{
    if(!_pci || !_pciConfigured)return;
    // Rematch/sleep already verified bus mastering off. Leave it off and avoid
    // configuration transactions if the parent has since dropped power.
    // The fresh PM-admitted instance enables its own configuration.
    if(!_reprobeRequested && !_power.needsRestart){
        if(!_dmaQuarantine)_pci->configWrite16(kIOPCIConfigCommand,_originalCommand);
        else _pci->setBusMasterEnable(false);
    }
    _pciConfigured=false;
}

void Ath10kPCIDevice::logMSIState(const char *stage)
{
    if (!_pci || !_hasMSI || _msiCapOffset == 0) {
        IOLog("Ath10kMac: MSI %s unavailable (capability not present)\n", stage ? stage : "state");
        return;
    }

    const UInt16 control = _pci->configRead16(_msiCapOffset + kMSIMessageControlOffset);
    const bool is64 = (control & kMSIControl64Bit) != 0;
    const bool pvm = (control & kMSIControlPerVectorMask) != 0;
    const UInt32 addressLo = _pci->configRead32(_msiCapOffset + 4);
    UInt32 addressHi = 0;
    UInt16 data = 0;
    if (is64) {
        addressHi = _pci->configRead32(_msiCapOffset + 8);
        data = _pci->configRead16(_msiCapOffset + 12);
    } else {
        data = _pci->configRead16(_msiCapOffset + 8);
    }

    ATH10K_TRACE("Ath10kMac: MSI %s control=0x%04x enabled=%s 64bit=%s pvm=%s mmc=%u mme=%u addr=%08x:%08x data=%04x\n",
          stage ? stage : "state", control,
          (control & kMSIControlEnable) ? "yes" : "no",
          is64 ? "yes" : "no", pvm ? "yes" : "no",
          (control >> 1) & 0x7, (control >> 4) & 0x7,
          addressHi, addressLo, data);
}

void Ath10kPCIDevice::interruptHandler(OSObject *target, void *refCon,
                                       IOService *nub, int source)
{
    (void)refCon;
    (void)nub;
    (void)source;
    auto *self = OSDynamicCast(Ath10kPCIDevice, target);
    if (self) {
        ++self->_irqObservedCount;
        // Keep the interrupt path bounded: a later work-loop/HTC step will
        // synchronize and consume the descriptor.  This MMIO read is enough
        // to prove that the CE1 read pointer moved without doing DMA mapping
        // work from interrupt context.
        if (self->_runtimeActive) {
            UInt32 ceRead = 0xffffffffu;
            if (self->_regs.read32(Ath10k::ceBaseAddress(Ath10k::kBMICENumToHost) +
                                   Ath10k::kCECurrentDstRead, ceRead))
                self->_runtimeLastCE1Read = ceRead;
        }
    }
}

bool Ath10kPCIDevice::prepareIRQParity()
{
    _irqPrepareAttempted=true;_irqPrepared=_irqRegistered=_irqEnabled=false;
    _irqMessaged=false;_irqIndex=-1;_irqType=0;_irqObservedCount=0;
    _msiControlBefore=_msiControlAfterRegister=_msiControlPostReset=_msiControlAfterWait=0xffffu;
    if(!_pci)return false;
    if(_hasMSI && _msiCapOffset)_msiControlBefore=_pci->configRead16(_msiCapOffset+kMSIMessageControlOffset);
    SInt32 legacy=-1,msi=-1;UInt32 legacyType=0,msiType=0;
    for(SInt32 index=0;index<32;++index){
        int type=0;if(_pci->getInterruptType(index,&type)!=kIOReturnSuccess)break;
        if(type&kIOInterruptTypePCIMessaged){if(msi<0){msi=index;msiType=UInt32(type);}}
        else if(legacy<0){legacy=index;legacyType=UInt32(type);}
    }
    if(msi>=0){
        _irqIndex=msi;_irqType=msiType;_irqMessaged=true;
        if(_pci->registerInterrupt(msi,this,&Ath10kPCIDevice::interruptHandler,nullptr)==kIOReturnSuccess){
            _irqRegistered=true;
            if(_hasMSI && _msiCapOffset)_msiControlAfterRegister=_pci->configRead16(_msiCapOffset+kMSIMessageControlOffset);
            if(!_hasMSI || !_msiCapOffset || (_msiControlAfterRegister&kMSIControlEnable)){
                _irqPrepared=true;logMSIState("msi-ready");return true;
            }
            _pci->unregisterInterrupt(msi);_irqRegistered=false;
        }
        IOLog("Ath10kMac: MSI unavailable; trying INTx index=%d\n",legacy);
    }
    if(legacy<0){_irqIndex=-1;return false;}
    _irqIndex=legacy;_irqType=legacyType;_irqMessaged=false;
    _intxCommandBefore=_pci->configRead16(kIOPCIConfigCommand);
    if(_intxCommandBefore==0xffffu)return false;
    _intxMSIBefore=(_hasMSI && _msiCapOffset)?_msiControlBefore:0;
    if(_intxMSIBefore==0xffffu)return false;
    _intxConfigChanged=true;
    if(_hasMSI && _msiCapOffset){
        _pci->configWrite16(_msiCapOffset+kMSIMessageControlOffset,_intxMSIBefore&~kMSIControlEnable);
        if(_pci->configRead16(_msiCapOffset+kMSIMessageControlOffset)&kMSIControlEnable){teardownIRQParity();return false;}
    }
    // Keep the shared line quiet throughout polled bootstrap. IOFilter takes
    // ownership before clearing Interrupt Disable. Never alter bus mastering.
    _pci->configWrite16(kIOPCIConfigCommand,_intxCommandBefore|UInt16(0x400));
    if(_pci->configRead16(kIOPCIConfigCommand)!=(_intxCommandBefore|0x400)){teardownIRQParity();return false;}
    if(_pci->registerInterrupt(legacy,this,&Ath10kPCIDevice::interruptHandler,nullptr)!=kIOReturnSuccess){teardownIRQParity();return false;}
    _irqRegistered=true;
    // Provider registration may restore PCI configuration; verify the fence
    // again before ROM initialization can raise any target cause.
    _pci->configWrite16(kIOPCIConfigCommand,_intxCommandBefore|UInt16(0x400));
    if(_pci->configRead16(kIOPCIConfigCommand)!=(_intxCommandBefore|0x400)){teardownIRQParity();return false;}
    _irqPrepared=true;
    ATH10K_TRACE("Ath10kMac: INTx prepared index=%d bootstrap=polling shared-filter=runtime\n",legacy);
    return true;
}

void Ath10kPCIDevice::teardownIRQParity()
{
    if(!_pci)return;
    if(_irqRegistered && _irqIndex>=0){
        if(_irqEnabled)_pci->disableInterrupt(_irqIndex);
        _pci->unregisterInterrupt(_irqIndex);_irqRegistered=false;
    }
    _irqEnabled=false;_irqPrepared=false;
    // Also restore after the raw handler was handed to IOFilterEventSource.
    if(_intxConfigChanged){
        bool restored=true;const UInt16 command=_pci->configRead16(kIOPCIConfigCommand);
        if(command==0xffffu)restored=false;
        else {
            const UInt16 desired=(command&~UInt16(0x400))|((_dmaQuarantine?UInt16(0x400):_intxCommandBefore)&0x400);
            _pci->configWrite16(kIOPCIConfigCommand,desired);
            restored=(_pci->configRead16(kIOPCIConfigCommand)&0x400)==(_intxCommandBefore&0x400);
        }
        if(_hasMSI && _msiCapOffset){
            const UInt16 current=_pci->configRead16(_msiCapOffset+kMSIMessageControlOffset);
            if(current==0xffffu)restored=false;
            else {
                const UInt16 desired=(current&~kMSIControlEnable)|(_dmaQuarantine?0:(_intxMSIBefore&kMSIControlEnable));
                _pci->configWrite16(_msiCapOffset+kMSIMessageControlOffset,desired);
                restored=restored && (_pci->configRead16(_msiCapOffset+kMSIMessageControlOffset)&kMSIControlEnable)==(_intxMSIBefore&kMSIControlEnable);
            }
        }
        if(restored)_intxConfigChanged=false;
        else ATH10K_TRACE("Ath10kMac: INTx config restoration unverified\n");
    }
}

bool Ath10kPCIDevice::startRuntimeCEIRQ()
{
    _runtimeAttempted = true;
    _runtimeActive = false;
    _runtimeCE1Armed = false;
    _runtimeInterruptsEnabled = false;
    _runtimeRxSlot = 0xffffffffu;
    _runtimeRxNext = 0xffffffffu;
    _runtimeRxPostedCount = 0;
    _runtimeRxCompletedCount = 0;
    _runtimeRxReplenishedCount = 0;
    _runtimeLastCE1Read = 0xffffffffu;
    _runtimeLastCE1Write = 0xffffffffu;
    _runtimeLastRxBytes = 0;
    _runtimeLastHostIE = 0xffffffffu;
    _runtimeLastMiscIE = 0xffffffffu;
    _runtimeLastPCIeIntrEnable = 0xffffffffu;
    _runtimeLastCoreCtrl = 0xffffffffu;
    _runtimeStatus = "runtime-prerequisite-mismatch";

    if (!_boot.doneSent || !_boot.readyValidated || !_boot.bmiClosed ||
        !_boot.rxOwned || !_htcReadyBuffer.allocated() || !_cePipesProgrammed ||
        !_ceDstRings[Ath10k::kBMICENumToHost].allocated() || !_pci ||
        !_irqRegistered || !_irqPrepared || _dmaQuarantine ||
        _bmiTransferUncertain || _ceDiagTransferUncertain)
        return false;

    _runtimeStatus = "runtime-rx-arm-failed";
    const UInt32 ce = Ath10k::kBMICENumToHost;
    const UInt32 entries = Ath10k::kCEHostConfig[ce].dstEntries;
    Ath10k::CESnapshot snapshot{};
    if (!Ath10k::readCESnapshot(_regs, ce, snapshot) ||
        !validateBMIPipeStructure(ce, snapshot) ||
        snapshot.dstWriteIndex >= entries || snapshot.currentDstRead >= entries ||
        snapshot.dstWriteIndex != snapshot.currentDstRead)
        return false;

    // waitForHTCReady consumed the descriptor at _boot.rxSlot.  Reuse the
    // same DMA buffer at the next free CE1 slot, which is the first persistent
    // receive slot for HTC/WMI traffic.
    const UInt32 slot = snapshot.dstWriteIndex;
    const UInt32 next = Ath10k::ceRingNext(slot, entries);
    auto *ring = reinterpret_cast<Ath10k::CEDescriptor32 *>(
        _ceDstRings[ce].virtualAddress());
    auto *desc = &ring[slot];
    bzero(desc, sizeof(*desc));
    desc->address = static_cast<UInt32>(_htcReadyBuffer.dmaAddress());
    if (!_htcReadyBuffer.syncForDevice())
        return false;
    OSSynchronizeIO();
    if (!_ceDstRings[ce].syncForDevice())
        return false;

    const UInt32 ceBase = Ath10k::ceBaseAddress(ce);
    const UInt32 doorbell = ceBase + Ath10k::kCEDstWriteIndex;
    if (!_regs.write32(doorbell, next) ||
        !_regs.read32(doorbell, _runtimeLastCE1Write) ||
        _runtimeLastCE1Write != next)
        return false;

    _runtimeRxSlot = slot;
    _runtimeRxNext = next;
    _runtimeRxPostedCount = 1;
    _runtimeRxReplenishedCount = 1;
    _runtimeLastCE1Read = snapshot.currentDstRead;
    _runtimeCE1Armed = true;

    _runtimeStatus = "runtime-ce1-irq-enable-failed";
    UInt32 hostIE = 0xffffffffu;
    UInt32 miscIE = 0xffffffffu;
    if (!_regs.read32(ceBase + Ath10k::kCEHostIE, hostIE) ||
        !_regs.read32(ceBase + Ath10k::kCEMiscIE, miscIE))
        return false;

    // Match Linux qcax CE behavior: copy-complete is enabled for an active
    // endpoint, while watermark and error sources remain masked until a
    // later HTC worker can acknowledge and drain them safely.
    const UInt32 runtimeHostIE =
        (hostIE & ~Ath10k::kCEHostWatermarkMask) |
        Ath10k::kCEHostCopyCompleteMask;
    const UInt32 runtimeMiscIE = miscIE & ~Ath10k::kCEMiscErrorMask;
    if (!_regs.write32(ceBase + Ath10k::kCEHostIE, runtimeHostIE) ||
        !_regs.write32(ceBase + Ath10k::kCEMiscIE, runtimeMiscIE) ||
        !_regs.read32(ceBase + Ath10k::kCEHostIE, _runtimeLastHostIE) ||
        !_regs.read32(ceBase + Ath10k::kCEMiscIE, _runtimeLastMiscIE) ||
        (_runtimeLastHostIE & Ath10k::kCEHostCopyCompleteMask) == 0 ||
        (_runtimeLastMiscIE & Ath10k::kCEMiscErrorMask) != 0)
        return false;

    const auto &layout = Ath10k::kQca6174RegisterLayout;
    const UInt32 intrEnable = Ath10k::socCoreOffset(layout, Ath10k::kPCIeIntrEnableAddress);
    const UInt32 intrClear = Ath10k::socCoreOffset(layout, Ath10k::kPCIeIntrClearAddress);
    const UInt32 coreCtrl = Ath10k::socCoreOffset(layout, Ath10k::kCoreCtrlAddress);
    const UInt32 intrMask = Ath10k::kPCIeIntrFirmwareMask | Ath10k::kPCIeIntrCEMaskAll;

    // Clear stale causes before unmasking, then flush the posted writes with a
    // readback exactly like ath10k_pci_enable_intx_irq().
    if (!_regs.write32(intrEnable, 0) ||
        !_regs.write32(intrClear, intrMask) ||
        !_regs.write32(intrEnable, intrMask) ||
        !_regs.read32(intrEnable, _runtimeLastPCIeIntrEnable) ||
        _runtimeLastPCIeIntrEnable != intrMask)
        return false;

    UInt32 coreBefore = 0xffffffffu;
    if (!_regs.read32(coreCtrl, coreBefore) ||
        !_regs.write32(coreCtrl, coreBefore | Ath10k::kCoreCtrlPCIeReg31Mask) ||
        !_regs.read32(coreCtrl, _runtimeLastCoreCtrl) ||
        (_runtimeLastCoreCtrl & Ath10k::kCoreCtrlPCIeReg31Mask) == 0)
        return false;

    // The IOPCI source is registered disabled. Enable it only after CE and
    // PCIe target-side masks are live, preventing an unhandled early MSI.
    _runtimeInterruptsEnabled = true;
    // Legacy bootstrap is polled until the shared-line filter replaces this
    // raw handler. Never dispatch an unacknowledged level interrupt here.
    const IOReturn enabled = _irqMessaged?_pci->enableInterrupt(_irqIndex):kIOReturnSuccess;
    if (enabled != kIOReturnSuccess) {
        _runtimeInterruptsEnabled = false;
        return false;
    }
    _irqEnabled = _irqMessaged;
    _runtimeActive = true;
    _runtimeStatus = "active";
    ATH10K_TRACE("Ath10kMac: Phase 3 CE/IRQ runtime ACTIVE CE1 slot=%u next=%u host-ie=0x%08x misc-ie=0x%08x pcie-intr=0x%08x core-ctrl=0x%08x irq-index=%d\n",
          _runtimeRxSlot, _runtimeRxNext, _runtimeLastHostIE,
          _runtimeLastMiscIE, _runtimeLastPCIeIntrEnable,
          _runtimeLastCoreCtrl, _irqIndex);
    return true;
}

bool Ath10kPCIDevice::stopRuntimeCEIRQ()
{
    bool ok = true;
    if (_pci && _irqEnabled && _irqIndex >= 0) {
        const IOReturn disabled = _pci->disableInterrupt(_irqIndex);
        ok &= disabled == kIOReturnSuccess;
        ATH10K_TRACE("Ath10kMac: Phase 3 CE/IRQ host interrupt disable ret=0x%08x\n",
              static_cast<UInt32>(disabled));
        _irqEnabled = false;
    }

    if (_regs.attached()) {
        const UInt32 intrEnable = Ath10k::socCoreOffset(
            Ath10k::kQca6174RegisterLayout, Ath10k::kPCIeIntrEnableAddress);
        const UInt32 intrClear = Ath10k::socCoreOffset(
            Ath10k::kQca6174RegisterLayout, Ath10k::kPCIeIntrClearAddress);
        const UInt32 coreCtrl = Ath10k::socCoreOffset(
            Ath10k::kQca6174RegisterLayout, Ath10k::kCoreCtrlAddress);
        const UInt32 intrMask = Ath10k::kPCIeIntrFirmwareMask |
                                Ath10k::kPCIeIntrCEMaskAll;

        for(UInt32 ce=1;ce<=3;++ce){
            const UInt32 base=Ath10k::ceBaseAddress(ce);
            ok &= _regs.write32(base+Ath10k::kCEHostIE,0);
            ok &= _regs.write32(base+Ath10k::kCEMiscIE,0);
        }
        ok &= _regs.write32(intrEnable, 0);
        ok &= _regs.write32(intrClear, intrMask);
        UInt32 core = 0xffffffffu;
        if (_regs.read32(coreCtrl, core))
            ok &= _regs.write32(coreCtrl, core & ~Ath10k::kCoreCtrlPCIeReg31Mask);
        else
            ok = false;

        UInt32 flush = 0xffffffffu;
        ok &= _regs.read32(intrEnable, flush) && flush == 0;
    }

    _runtimeInterruptsEnabled = false;
    _runtimeActive = false;
    _runtimeCE1Armed = false;
    _runtimeStatus = ok ? "stopped" : "stop-failed";
    IOLog("Ath10kMac: Phase 3 CE/IRQ runtime STOP %s posted=%u completed=%u replenished=%u\n",
          ok ? "PASS" : "FAIL", _runtimeRxPostedCount,
          _runtimeRxCompletedCount, _runtimeRxReplenishedCount);
    return ok;
}

bool Ath10kPCIDevice::prepareWMIRuntime()
{
    _wmiLive.attempted=true;
    _wmiLive.status="runtime-prerequisite";
    if (!_runtimeActive || !_wmiInit.completed || !_wmiInit.txConsumed ||
        _wmi.credits.outstanding || !_wmi.rxOwned || !_boot.rxOwned || _dmaQuarantine)
        return false;
    // Deliberately switch to polling: the bootstrap's raw MSI handler does not
    // drain/ack CE causes. No interrupt/DMA work races this single worker.
    _wmiLive.status="runtime-mask-failed";
    if (!stopRuntimeCEIRQ()) return false;
    _runtimeActive=true;
    _runtimeCE1Armed=true;
    _runtimeStatus="wmi-polling";
    _wmiLive.status="runtime-worker-allocation";
    _wmiWorkLoop=getWorkLoop();
    if(_wmiWorkLoop)_wmiWorkLoop->retain();
    if (!_wmiWorkLoop) return false;
    _wmiTimer=IOTimerEventSource::timerEventSource(
        kIOTimerEventSourceOptionsPriorityWorkLoop,this,wmiTimerFired);
    if (!_wmiTimer) return false;
    if (_wmiWorkLoop->addEventSource(_wmiTimer)!=kIOReturnSuccess) return false;
    _wmiTimerAdded=true;
    _wmiLive.active=true;
    _wmiLive.status="prepared";
    return true; // start() arms only after probeTarget finishes ownership transfer.
}

IOReturn Ath10kPCIDevice::controlWMIRuntime(OSObject *owner, void *reason,
                                          void *, void *, void *)
{
    auto *self=OSDynamicCast(Ath10kPCIDevice,owner);
    if (!self) return kIOReturnBadArgument;
    if (reason) {
        self->endWMIRuntime(static_cast<const char *>(reason));
        return kIOReturnSuccess;
    }
    if (self->powerIOBlocked() || !self->_wmiLive.active || !self->_wmiTimer) return kIOReturnNotReady;
    if (self->_wmiTimer->setTimeoutMS(10)!=kIOReturnSuccess) {
        self->endWMIRuntime("runtime-timer-failed");
        return kIOReturnError;
    }
    if(!self->prepareRuntimePower()){self->endWMIRuntime("runtime-wake-lock-allocation");return kIOReturnError;}
    const char *irqError=self->startRuntimeWorkerIRQ();
    if(irqError){self->endWMIRuntime(irqError);return kIOReturnError;}
    self->_wmiLive.status=self->_runtimeIRQArmed?"active-msi-worker":"active-polling";
    self->publishWMIRuntimeProperties();
    ATH10K_TRACE("Ath10kMac: WMI RUNTIME ACTIVE mode=%s watchdog=10ms CE1+CE2 budget=32 HTT budget=64\n",self->_wmiLive.status);
    return kIOReturnSuccess;
}

const char *Ath10kPCIDevice::serviceRuntime()
{
    uint64_t tick=0,tickNS=0;clock_get_uptime(&tick);absolutetime_to_nanoseconds(tick,&tickNS);
    const UInt64 tickMs=tickNS/1000000u;
    const bool maintenance=tickMs-_wmiLive.lastMaintenanceMs>=1000;
    const UInt64 started=Ath10k::uptimeNS();
    _rxDeliveryBatch=ATH10K_RX_BATCH;
    const char *error=drainWMIRuntime();
#ifdef ATH10K_NATIVE
    const bool macTransaction=_nativeMACActive;
    if(!error && macTransaction)error=serviceNativeMAC(tickMs);
#else
    const bool macTransaction=false;
#endif
    if (!error && !macTransaction) {
        uint64_t now=0,ns=0;clock_get_uptime(&now);absolutetime_to_nanoseconds(now,&ns);
        error=serviceWMIEcho(ns/1000000u);
        if (!error && _wmiEcho.completed) error=serviceRxSetup(ns/1000000u);
        if (!error && _wmiEcho.completed && _rxSetup.completed) error=serviceWMIVdevCreate(ns/1000000u);
        for(UInt32 i=0;i<Ath10k::kWMIRadioStepCount && !error;++i)
            if(i?_wmiRadio[i-1].completed:_wmiVdevCreate.completed)error=serviceWMIRadio(ns/1000000u,i);
        if (!error && _wmiVdevCreate.completed && _wmiRadio[0].completed && _wmiRadio[1].completed && _wmiRadio[Ath10k::kWMIPMFQoSIndex].completed) error=serviceWMIScanChannels(ns/1000000u);
        if (!error && _wmiScanChannels.completed) error=serviceWMIScan(ns/1000000u);
    }
    if (!error && !macTransaction) {uint64_t t=0,n=0;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&n);error=serviceConnection(n/1000000u);}
    const UInt64 rxStarted=Ath10k::uptimeNS();
    if (!error && tickMs>=_nextRxDoneRetryMs) {
        error=serviceHTTRx();
        _nextRxDoneRetryMs=_rxSetup.rxDoneConsecutiveWaits?tickMs+1:0;
    }
    _perf.rx.add(Ath10k::uptimeNS()-rxStarted);
    // RX and reorder delivery can unblock TCP ACK/data transmission. Check
    // completions again before returning rather than waiting for another poll.
    if (!error) error=serviceDataTX(Ath10k::uptimeNS()/1000000u);
#ifdef ATH10K_NATIVE
    if (!error) requestNativeService();
#endif
    // The full diagnostic sweep synchronizes 1023 mappings. Keep it off
    // sustained traffic; completion/DONE/refill validation still runs per MPDU.
    if(maintenance){
        const bool idle=_lastMaintenanceRxPackets==_connection.rxPackets && _lastMaintenanceTxPackets==_connection.txPackets;
        if(ATH10K_DIAGNOSTICS && !error && idle)error=sampleHTTRxPool();
        _lastMaintenanceRxPackets=_connection.rxPackets;_lastMaintenanceTxPackets=_connection.txPackets;
    }
    flushNetworkRX();_rxDeliveryBatch=false;
    if(_connection.fwStats.observedSerial!=_connection.fwStats.serial){
        _connection.fwStats.observedSerial=_connection.fwStats.serial;_connection.fwStats.observedMs=tickMs;
    }
    if(_connection.fwStats.observedNoiseSerial!=_connection.fwStats.noiseSerial){
        _connection.fwStats.observedNoiseSerial=_connection.fwStats.noiseSerial;_connection.fwStats.noiseObservedMs=tickMs;
    }
    if(_connection.fwStats.observedRateSerial!=_connection.fwStats.rateSerial){
        _connection.fwStats.observedRateSerial=_connection.fwStats.rateSerial;_connection.fwStats.rateObservedMs=tickMs;
    }
    _perf.worker.add(Ath10k::uptimeNS()-started);
    if(maintenance){_wmiLive.lastMaintenanceMs=tickMs;publishWMIRuntimeProperties();}
    return error;
}

void Ath10kPCIDevice::wmiTimerFired(OSObject *owner,IOTimerEventSource *timer)
{
    auto *self=OSDynamicCast(Ath10kPCIDevice,owner);
    if(!self || timer!=self->_wmiTimer || !self->_wmiLive.active || self->powerIOBlocked())return;
    if(self->_runtimeIRQArmed){
        if(self->_runtimeIRQMaskFailed || !self->maskRuntimeWorkerIRQ()){
            self->endWMIRuntime("runtime-irq-mask");return;
        }
        for(UInt32 ce=1;ce<=3;++ce)
            if(!self->_regs.write32(Ath10k::ceBaseAddress(ce)+Ath10k::kCEHostIS,Ath10k::kCEHostInterruptMask)){
                self->endWMIRuntime("runtime-irq-ack");return;
            }
    }
    const char *error=self->serviceRuntime();
    if(error){self->endWMIRuntime(error);return;}
    bool pending=self->_rxSetup.pendingMPDUs!=0;
    if(self->_runtimeIRQArmed){
        UInt32 summary=0;
        if(!self->_regs.read32(Ath10k::kQca6174RegisterLayout.ceWrapperBaseAddress,summary)){
            self->endWMIRuntime("runtime-irq-summary");return;
        }
        pending|=((summary>>8)&0x0e)!=0;
        self->_runtimeIRQDeferred=pending;
        if(!pending && !self->unmaskRuntimeWorkerIRQ()){
            self->endWMIRuntime("runtime-irq-unmask");return;
        }
    }
    bool asleep=false;error=self->idleRuntimePower(pending,asleep);
    if(error){self->endWMIRuntime(error);return;}
    self->_wmiLive.pollIntervalMs=asleep?100:(pending?1:
        (self->_connection.stage==Ath10k::LinkStage::Connected && !self->_runtimeIRQArmed?1:10));
    if(timer->setTimeoutMS(self->_wmiLive.pollIntervalMs)!=kIOReturnSuccess)self->endWMIRuntime("runtime-timer-failed");
}

void Ath10kPCIDevice::endWMIRuntime(const char *reason)
{
    // Called only under the work-loop gate, including stop/PM/shutdown paths.
    if (!_wmiLive.active) return;
#ifdef ATH10K_NATIVE
    if(_nativeMACWaiting){_nativeMACResult=kIOReturnError;_nativeMACActive=false;setProperty("Ath10kNativeMACStatus",reason);_nativeGate->commandWakeup(&_nativeMACResult);}
#endif
    IOLog("Ath10kMac: RUNTIME STOP context reason=%s stage=%u security=%u disconnect=%u peers=%u cmd=0x%x wmi-owned=%u credit-pending=%u tx-owned=%u first-error=%s\n",
        reason,static_cast<UInt32>(_connection.stage),_connection.securityPhase,static_cast<UInt32>(_connection.disconnect),
        _connection.peerCount,_connection.diagnostics.lastCommand,_connection.wmiOwned,_wmi.credits.outstanding,_connection.txOwned,_connection.error);
    stopRuntimeWorkerIRQ();
    const bool wakeOK=finishRuntimePower();
    pauseRXReserve();
    networkLink(false);
    if(_connection.stage!=Ath10k::LinkStage::Disabled){_connection.stage=Ath10k::LinkStage::Failed;_connection.error=reason;}
    _wmiLive.active=false;
    _wmiLive.status=reason;
    if (_rxSetup.attempted) _rxSetup.status=reason;
    if (_wmiVdevCreate.attempted && !_wmiVdevCreate.completed) _wmiVdevCreate.status=reason;
    for (auto &radio:_wmiRadio) if (radio.attempted && !radio.completed) radio.status=reason;
    if (_wmiScanChannels.attempted && !_wmiScanChannels.completed) _wmiScanChannels.status=reason;
    if (_wmiScan.attempted && !_wmiScan.completed) _wmiScan.status=reason;
    if (_wmiEcho.attempted && !_wmiEcho.completed) _wmiEcho.status=reason;
    if (_wmiTimer) { _wmiTimer->cancelTimeout(); _wmiTimer->disable(); }
    const bool irqOK=stopRuntimeCEIRQ();
    // Reset is still attempted if interrupt masking fails; DMA release also
    // has the independent bus-master-disable verification as a fallback.
    const bool resetOK=quiesceTargetAfterBMI();
    Ath10k::wipeConnectionSecrets(_connection);
#ifdef ATH10K_NATIVE
    discardNativeSecurity();
#endif
    if (resetOK) releaseTargetWake(_runtimeWakeAsserted);
    if (!wakeOK || !irqOK || !resetOK) _dmaQuarantine=true;
    _runtimeWakeAsserted=false;
    restorePCIeBringupParity();
    IOLog("Ath10kMac: WMI RUNTIME STOP reason=%s polls=%u rx=%u irq-stop=%s reset=%s\n",
          reason,_wmiLive.polls,_wmiLive.rxCount,irqOK?"yes":"no",resetOK?"yes":"no");
    publishDiagnosticProperties();
#ifdef ATH10K_NATIVE
    // DMA/IRQ are quiesced above. Report failure through WCL even though the
    // radio timer is now stopped; never leave a native scan/link hanging.
    if(_nativeScanPending){_wmiScan.failed=true;_wmiScan.completed=true;}
    requestNativeService();
#endif
}

void Ath10kPCIDevice::releaseWMIRuntime()
{
    if (_wmiWorkLoop)
        _wmiWorkLoop->runAction(controlWMIRuntime,this,const_cast<char *>("stopped"));
    if (_wmiTimer) {
        _wmiTimer->cancelTimeout();
        _wmiTimer->disable();
        if (_wmiWorkLoop && _wmiTimerAdded) _wmiWorkLoop->removeEventSource(_wmiTimer);
        _wmiTimerAdded=false;
        _wmiTimer->release();_wmiTimer=nullptr;
    }
    // Disarm under the same lock used by the filter before detaching it.
    stopRuntimeWorkerIRQ();
    if(_runtimeIRQ){
        if(_runtimeIRQAdded && _wmiWorkLoop)_wmiWorkLoop->removeEventSource(_runtimeIRQ);
        _runtimeIRQAdded=false;_runtimeIRQ->release();_runtimeIRQ=nullptr;
    }
    // The event source has unregistered its provider handler. Its lock must
    // outlive the source, including partial initialization failures.
    if(_runtimeIRQLock){IOSimpleLockFree(_runtimeIRQLock);_runtimeIRQLock=nullptr;}
    _regs.setAccessHook(nullptr,nullptr,nullptr);
    if(_runtimeWakeLock){IOSimpleLockFree(_runtimeWakeLock);_runtimeWakeLock=nullptr;}
    if (_wmiWorkLoop) { _wmiWorkLoop->release();_wmiWorkLoop=nullptr; }
}

IOReturn Ath10kPCIDevice::setPowerState(unsigned long state, IOService *device)
{
    (void)device;
    requestPowerEvent(state?Ath10k::PowerEvent::DeviceOn:Ath10k::PowerEvent::DeviceOff);
    return kIOPMAckImplied;
}

void Ath10kPCIDevice::systemWillShutdown(IOOptionBits specifier)
{
    // Shutdown can arrive before deferred firmware initialization finishes.
    // Use the same producer barrier and immediate I/O block as system sleep.
    requestPowerEvent(Ath10k::PowerEvent::DeviceOff);
    super::systemWillShutdown(specifier);
}

void Ath10kPCIDevice::publishWMIRuntimeProperties()
{
    setProperty("Ath10kWMIUnknownEvents",_wmiEvents.unknownCount,32);
    setProperty("Ath10kWMILastUnknownEvent",_wmiEvents.unknownId,32);
    setProperty("Ath10kWMITemperatureEvents",_wmiEvents.temperatureCount,32);
    setProperty("Ath10kWMITemperatureCelsius",static_cast<UInt32>(_wmiEvents.temperature),32);
    setProperty("Ath10kWMIRFKill",_wmiEvents.rfKill);
    setProperty("Ath10kWMIPeerKickouts",_wmiEvents.kickoutCount,32);
    setProperty("Ath10kWMIKeyNotifications",_wmiEvents.keyCompleteCount,32);
    setProperty("Ath10kFirmwareStatsDecoded",_wmiEvents.statsDecoded,32);
    setProperty("Ath10kFirmwareStatsUnsupported",_wmiEvents.statsUnsupported,32);
    setProperty("Ath10kWMIHostMemoryOwned",_wmiInit.hostMemoryOwned);
    setProperty("Ath10kWMIHostMemoryChunks",_wmiInit.hostMemoryCount,32);
    UInt32 hostBytes=0;for(UInt32 i=0;i<_wmiInit.hostMemoryCount && i<Ath10k::kWMIHostMemoryMax;++i)hostBytes+=_wmiInit.hostMemory[i].bytes;
    setProperty("Ath10kWMIHostMemoryBytes",hostBytes,32);
    publishRuntimePower();
    publishPowerState();
    publishConnection();
    setProperty("Ath10kWMIVdevCreateAttempted",_wmiVdevCreate.attempted);
    setProperty("Ath10kWMIVdevCreateSubmitted",_wmiVdevCreate.submitted);
    setProperty("Ath10kWMIVdevCreateTxConsumed",_wmiVdevCreate.txConsumed);
    setProperty("Ath10kWMIVdevCreateCreditReturned",_wmiVdevCreate.creditReturned);
    setProperty("Ath10kWMIVdevCreateTransportSucceeded",_wmiVdevCreate.completed);
    setProperty("Ath10kWMIVdevCreateTxDMAOwned",_wmiVdevCreate.txOwned);
    setProperty("Ath10kWMIVdevCreateStatus",_wmiVdevCreate.status);
    setProperty("Ath10kWMIVdevCreateElapsedMs",_wmiVdevCreate.elapsedMs,64);
    setProperty("Ath10kRxSetupStage",_rxSetup.stage,32);
    setProperty("Ath10kRxSetupStatus",_rxSetup.status);
    setProperty("Ath10kRxSetupTransportSucceeded",_rxSetup.completed);
    setProperty("Ath10kRxFilterBarrierReplied",_filterEcho.replyReceived);
    setProperty("Ath10kHTTVersionMajor",_rxSetup.major,32);
    setProperty("Ath10kHTTVersionMinor",_rxSetup.minor,32);
    setProperty("Ath10kHTTMessages",_rxSetup.messages,32);
    setProperty("Ath10kHTTLastType",_rxSetup.lastType,32);
    setProperty("Ath10kHTTRxPoolDone",_rxSetup.rxDone,32);
    setProperty("Ath10kHTTRxPoolPosted",Ath10k::kHTTRxPosted,32);
    setProperty("Ath10kHTTRxRingDMAOwned",_rxSetup.ringOwned);
    setProperty("Ath10kRegulatoryCountry",_regulatory.country?_regulatory.country->alpha2:"invalid");
    setProperty("Ath10kRegulatoryStatus",_regulatory.status);
    setProperty("Ath10kRegulatoryIndoor",_regulatory.indoor);
    setProperty("Ath10kRegulatoryEEPROM",(UInt64)_regulatory.eepromRD,32);
    setProperty("Ath10kRegulatoryDomain",(UInt64)_regulatory.domain,32);
    setProperty("Ath10kRegulatoryTxCeilingHalfDbm",(UInt64)_regulatory.ceilingHalfDbm,32);
    setProperty("Ath10kRegulatoryCapsPresent",_wmi.regCapsPresent);
    setProperty("Ath10kRegulatoryCapsTLVBytes",UInt64(_wmi.regCapsBytes),32);
    setProperty("Ath10kRegulatoryCapsTLVOffset",UInt64(_wmi.regCapsOffset),32);
    setProperty("Ath10kRegulatoryCapsTLVCount",UInt64(_wmi.regCapsCount),32);
    setProperty("Ath10kRegulatoryFirmwareLow2",UInt64(_wmi.low2),32);
    setProperty("Ath10kRegulatoryFirmwareHigh2",UInt64(_wmi.high2),32);
    setProperty("Ath10kRegulatoryFirmwareLow5",UInt64(_wmi.low5),32);
    setProperty("Ath10kRegulatoryFirmwareHigh5",UInt64(_wmi.high5),32);
    setProperty("Ath10kRegulatoryFirmwareMinPowerRaw",UInt64(_wmi.hwMinPower),32);
    setProperty("Ath10kRegulatoryFirmwareMaxPowerRaw",UInt64(_wmi.hwMaxPower),32);
    setProperty("Ath10kRegulatoryCommandComplete",_wmiRadio[2].completed);
    setProperty("Ath10kPower2GCommandComplete",_wmiRadio[3].completed);
    setProperty("Ath10kPower5GCommandComplete",_wmiRadio[4].completed);
    setProperty("Ath10kPMFQoSTransportComplete",_wmiRadio[Ath10k::kWMIPMFQoSIndex].completed);
    setProperty("Ath10kPMFQoSStatus",_wmiRadio[Ath10k::kWMIPMFQoSIndex].status);
    setProperty("Ath10kRegulatoryScanCount",(UInt64)Ath10k::regulatoryScanCount(&_regulatory),32);
    setProperty("Ath10kWMIRadioTXStatus",_wmiRadio[0].status);
    setProperty("Ath10kWMIRadioRXStatus",_wmiRadio[1].status);
    setProperty("Ath10kWMIRadioTransportSucceeded",_wmiRadio[0].completed && _wmiRadio[1].completed);
    setProperty("Ath10kWMIRuntimeRawCount",_wmiLive.rawCount,32);
    setProperty("Ath10kWMIRuntimeRawCE1",_wmiLive.rawCE1,32);
    setProperty("Ath10kWMIRuntimeRawCE2",_wmiLive.rawCE2,32);
    setProperty("Ath10kWMIRuntimeRawMgmt",_wmiLive.rawMgmt,32);
    setProperty("Ath10kWMIRuntimeRejected",_wmiLive.rejected,32);
    setProperty("Ath10kWMIScanChannelsAttempted",_wmiScanChannels.attempted);
    setProperty("Ath10kWMIScanChannelsSubmitted",_wmiScanChannels.submitted);
    setProperty("Ath10kWMIScanChannelsTxConsumed",_wmiScanChannels.txConsumed);
    setProperty("Ath10kWMIScanChannelsCreditReturned",_wmiScanChannels.creditReturned);
    setProperty("Ath10kWMIScanChannelsTransportSucceeded",_wmiScanChannels.completed);
    setProperty("Ath10kWMIScanChannelsTxDMAOwned",_wmiScanChannels.txOwned);
    setProperty("Ath10kWMIScanChannelsStatus",_wmiScanChannels.status);
    setProperty("Ath10kWMIScanChannelsElapsedMs",_wmiScanChannels.elapsedMs,64);
    setProperty("Ath10kWMIScanAttempted",_wmiScan.attempted);
    setProperty("Ath10kWMIScanSubmitted",_wmiScan.submitted);
    setProperty("Ath10kWMIScanTxConsumed",_wmiScan.txConsumed);
    setProperty("Ath10kWMIScanCreditReturned",_wmiScan.creditReturned);
    setProperty("Ath10kWMIScanSucceeded",_wmiScan.completed);
    setProperty("Ath10kWMIScanTxDMAOwned",_wmiScan.txOwned);
    setProperty("Ath10kWMIScanGeneration",_wmiScan.generation,32);
    setProperty("Ath10kWMIScanID",_wmiScan.id,32);
    setProperty("Ath10kWMIScanCancelRequested",_wmiScan.cancelRequested);
    setProperty("Ath10kWMIScanStopConsumed",_wmiScan.stopConsumed);
    setProperty("Ath10kWMIScanStatus",_wmiScan.status);
    setProperty("Ath10kWMIScanElapsedMs",_wmiScan.elapsedMs,64);
    setProperty("Ath10kWMIScanStarted",_wmiScan.started);
    setProperty("Ath10kWMIScanTerminal",_wmiScan.terminal);
    setProperty("Ath10kWMIScanFailed",_wmiScan.failed);
    setProperty("Ath10kWMIScanEventCount",_wmiScan.eventCount,32);
    setProperty("Ath10kWMIScanLastEventType",_wmiScan.lastType,32);
    setProperty("Ath10kWMIScanReason",_wmiScan.reason,32);
    setProperty("Ath10kWMIScanFrequency",_wmiScan.frequency,32);
    setProperty("Ath10kWMIScanPlannedChannels","1,2,3,4,5,6,7,8,9,10,11,12,13,36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165");
    setProperty("Ath10kWMIScanVisitedMask",_wmiScan.visitedMask,64);
    setProperty("Ath10kWMIScanAllChannelsObserved",_wmiScan.visitedMask==((1ULL<<Ath10k::kWMIScanChannelCount)-1));
    setProperty("Ath10kWMIScanDwellMs",Ath10k::kWMIScanDwellMs,32);
    setProperty("Ath10kWMIScanHostTimeoutMs",Ath10k::kWMIScanHostMs,32);
    setProperty("Ath10kWMIScanPowerHalfDbm",Ath10k::kWMIScanPowerHalfDbm,32);
    setProperty("Ath10kWMIScanPowerSource","regdb-country-board-fw-bounds-and-host-ceiling");
    setProperty("Ath10kWMIScanChannelInfoCount",_wmiScan.channelInfoCount,32);
    setProperty("Ath10kWMIScanChannelInfoErrors",_wmiScan.channelInfoErrors,32);
    setProperty("Ath10kWMIScanChannelInfoMask",_wmiScan.channelInfoMask,64);
    for (UInt32 i=0;i<Ath10k::kWMIScanChannelCount;++i) {
        if (!_wmiScan.channels[i].count) continue;
        for (UInt32 which=0;which<2;++which) {
            const auto &sample=which?_wmiScan.channels[i]:_wmiScan.firstChannels[i];
            char key[64]{},value[256]{};
            // Existing ChannelN key remains the latest snapshot; First is immutable.
            snprintf(key,sizeof(key),"Ath10kWMIScanChannel%u%s",Ath10k::wmiScanChannel(i),which?"":"First");
            const long long noise=sample.noise&0x80000000u?
                static_cast<long long>(sample.noise)-0x100000000LL:sample.noise;
            snprintf(value,sizeof(value),"freq=%u samples=%u errors=%u err-code=%u flags=0x%x noise=%lld clear=%u cycles=%u rx-frames-present=%u rx-frames=%u",
                     Ath10k::wmiScanFrequency(i),sample.count,sample.errors,sample.errorCode,sample.flags,noise,
                     sample.clear,sample.cycles,sample.rxFramesPresent?1u:0u,sample.rxFrames);
            setProperty(key,value);
        }
    }
    setProperty("Ath10kWMIScanMgmtCount",_wmiScan.mgmtCount,32);
    setProperty("Ath10kWMIScanMgmtBytes",_wmiScan.mgmtBytes,32);
    setProperty("Ath10kBSSLastReject",Ath10k::bssRejectName(_wmiBSS.lastReject));
    for(UInt32 i=0;i<Ath10k::BSSRejectCount;++i){
        char key[40]{};snprintf(key,sizeof(key),"Ath10kBSSReject%u",i);
        char value[96]{};snprintf(value,sizeof(value),"%s count=%u",Ath10k::bssRejectName(i),_wmiBSS.rejects[i]);
        setProperty(key,value);
    }
    setProperty("Ath10kHTTRxIndications",_rxSetup.rxIndications,32);
    setProperty("Ath10kHTTRxBatchMax",_rxSetup.batchMax,32);
    setProperty("Ath10kHTTRxBatchCalls",_rxSetup.batchCalls,32);
    setProperty("Ath10kHTTRxBudgetHits",_rxSetup.budgetHits,32);
    setProperty("Ath10kHTTRxPendingHigh",_rxSetup.pendingHigh,32);
    setProperty("Ath10kWMIRuntimePollIntervalMs",_wmiLive.pollIntervalMs,32);
    setProperty("Ath10kHTTRxPendingMPDUs",_rxSetup.pendingMPDUs,32);
    setProperty("Ath10kHTTRxPaddrIndications",_rxSetup.paddrIndications,32);
    setProperty("Ath10kHTTRxMode",_rxSetup.rxMode,32);
    setProperty("Ath10kHTTRxRecycled",_rxSetup.recycled,32);
    setProperty("Ath10kHTTRxDiscardedMPDUs",_rxSetup.discardedMPDUs,32);
    setProperty("Ath10kHTTRxCompletionNotDone",_rxSetup.rxCompletionNotDone,32);
    setProperty("Ath10kHTTRxCompletionWaits",_rxSetup.rxCompletionWaits,32);
    setProperty("Ath10kHTTRxDoneConsecutiveWaits",_rxSetup.rxDoneConsecutiveWaits,32);
    setProperty("Ath10kHTTRxDoneTimeouts",_rxSetup.rxDoneTimeouts,32);
    setProperty("Ath10kHTTRxDoneLastBuffer",_rxSetup.rxDoneLastBuffer,32);
    setProperty("Ath10kHTTRxDoneLastAttention",_rxSetup.rxDoneLastAttention,32);
    setProperty("Ath10kBSSCapacity",Ath10k::kWMIBSSMax,32);
    setProperty("Ath10kBSSCount",_wmiBSS.count,32);
    setProperty("Ath10kBSSAccepted",_wmiBSS.accepted,32);
    setProperty("Ath10kBSSMalformed",_wmiBSS.malformed,32);
    setProperty("Ath10kBSSRxErrors",_wmiBSS.rxErrors,32);
    setProperty("Ath10kBSSIgnored",_wmiBSS.ignored,32);
    setProperty("Ath10kBSSOverflow",_wmiBSS.overflow,32);
    setProperty("Ath10kBSSBeacons",_wmiBSS.beacons,32);
    setProperty("Ath10kBSSProbeResponses",_wmiBSS.probeResponses,32);
    setProperty("Ath10kBSSLastStatus",_wmiBSS.lastStatus,32);
    setProperty("Ath10kBSSLastChannel",_wmiBSS.lastChannel,32);
    setProperty("Ath10kBSSLastBytes",_wmiBSS.lastBytes,32);
    for(UInt32 i=0;i<_wmiBSS.count && i<Ath10k::kWMIBSSMax;++i){
        const auto &entry=_wmiBSS.entries[i];char ssid[129]{};Ath10k::formatWMISSID(entry,ssid);
        char key[24]{};snprintf(key,sizeof(key),"Ath10kBSS%u",i);
        char value[384]{};
        snprintf(value,sizeof(value),"bssid=%02x:%02x:%02x:%02x:%02x:%02x ssid=\"%s\" hidden=%u rx-channel=%u advertised-channel=%u snr=%u privacy=%u rsn-ie=%u wpa-ie=%u seen=%u",
                 entry.bssid[0],entry.bssid[1],entry.bssid[2],entry.bssid[3],entry.bssid[4],entry.bssid[5],
                 ssid,entry.hidden,entry.rxChannel,entry.advertisedChannel,entry.snr,
                 entry.privacy,entry.rsnPresent,entry.wpaPresent,entry.seen);
        setProperty(key,value);
    }

    setProperty("Ath10kWMIEchoAttempted",_wmiEcho.attempted);
    setProperty("Ath10kWMIEchoSubmitted",_wmiEcho.submitted);
    setProperty("Ath10kWMIEchoTxConsumed",_wmiEcho.txConsumed);
    setProperty("Ath10kWMIEchoReplyReceived",_wmiEcho.replyReceived);
    setProperty("Ath10kWMIEchoCreditReturned",_wmiEcho.creditReturned);
    setProperty("Ath10kWMIEchoSucceeded",_wmiEcho.completed);
    setProperty("Ath10kWMIEchoTxDMAOwned",_wmiEcho.txOwned);
    setProperty("Ath10kWMIEchoStatus",_wmiEcho.status);
    setProperty("Ath10kWMIEchoCookie",(UInt64)_wmiEcho.cookie,64);
    setProperty("Ath10kWMIEchoElapsedMs",_wmiEcho.elapsedMs,64);
    setProperty("Ath10kWMIRuntimeAttempted",_wmiLive.attempted);
    setProperty("Ath10kWMIRuntimeActive",_wmiLive.active);
    setProperty("Ath10kWMIRuntimeStatus",_wmiLive.status);
    setProperty("Ath10kWMIRuntimeMode",_runtimeIRQArmed?"irq-worker-with-adaptive-watchdog":"adaptive-polling");
    setProperty("Ath10kWMIRuntimePollCount",(UInt64)_wmiLive.polls,32);
    setProperty("Ath10kWMIRuntimeRxCount",(UInt64)_wmiLive.rxCount,32);
    setProperty("Ath10kWMIRuntimeBudgetHits",(UInt64)_wmiLive.budgetHits,32);
    setProperty("Ath10kWMIRuntimeLastEventID",(UInt64)_wmiLive.lastEventId,32);
    setProperty("Ath10kWMIRuntimeLastRxBytes",(UInt64)_wmiLive.lastBytes,32);
    setProperty("Ath10kWMIRuntimeStatsCount",(UInt64)_wmiEvents.statsCount,32);
    setProperty("Ath10kWMIRuntimeStatsBytes",(UInt64)_wmiEvents.statsBytes,32);
    setProperty("Ath10kWMIRuntimeDiagnosticCount",(UInt64)_wmiEvents.diagCount,32);
    setProperty("Ath10kWMIRuntimeDiagSupportedCount",(UInt64)_wmiEvents.diagSupportedCount,32);
    setProperty("Ath10kWMIRuntimeDiagSupportedEntries",(UInt64)_wmiEvents.diagSupportedEntries,32);
    setProperty("Ath10kWMIRuntimePackageIDCount",(UInt64)_wmiEvents.pkgIdCount,32);
    setProperty("Ath10kWMIRuntimePackageID",(UInt64)_wmiEvents.pkgId,32);
    setProperty("Ath10kWMICreditsAvailable",(UInt64)_wmi.credits.available,32);
    setProperty("Ath10kWMICreditsOutstanding",(UInt64)_wmi.credits.outstanding,32);
}

bool Ath10kPCIDevice::htcRepostRuntimeReceive()
{
    const UInt32 ce = Ath10k::kBMICENumToHost;
    const UInt32 entries = Ath10k::kCEHostConfig[ce].dstEntries;
    if (!_runtimeActive || !_runtimeCE1Armed || !_htcReadyBuffer.allocated() ||
        !_ceDstRings[ce].allocated() || entries == 0 ||
        _runtimeRxSlot >= entries || _runtimeRxNext >= entries)
        return false;

    Ath10k::CESnapshot snapshot{};
    if (!Ath10k::readCESnapshot(_regs, ce, snapshot) ||
        !validateBMIPipeStructure(ce, snapshot) ||
        snapshot.dstWriteIndex >= entries || snapshot.currentDstRead >= entries ||
        snapshot.dstWriteIndex != _runtimeRxNext)
        return false;

    auto *ring = reinterpret_cast<Ath10k::CEDescriptor32 *>(
        _ceDstRings[ce].virtualAddress());
    if (!_ceDstRings[ce].syncFromDevice() ||
        ring[_runtimeRxSlot].nbytes == 0 ||
        ring[_runtimeRxSlot].address != static_cast<UInt32>(_htcReadyBuffer.dmaAddress()))
        return false;

    // The just-completed descriptor is no longer device-owned. The next
    // descriptor is posted at CE's current write index, matching the same
    // one-buffer-at-a-time discipline used by the validated READY path.
    if (_runtimeRxSlot < entries) {
        ring[_runtimeRxSlot].nbytes = 0;
        ring[_runtimeRxSlot].flags = 0;
    }
    const UInt32 slot = snapshot.dstWriteIndex;
    const UInt32 next = Ath10k::ceRingNext(slot, entries);
    auto *desc = &ring[slot];
    bzero(desc, sizeof(*desc));
    desc->address = static_cast<UInt32>(_htcReadyBuffer.dmaAddress());
    bzero(_htcReadyBuffer.virtualAddress(),
          static_cast<size_t>(_htcReadyBuffer.length()));
    OSSynchronizeIO();
    if (!_htcReadyBuffer.syncForDevice() || !_ceDstRings[ce].syncForDevice())
        return false;

    const UInt32 base = Ath10k::ceBaseAddress(ce);
    const UInt32 doorbell = base + Ath10k::kCEDstWriteIndex;
    UInt32 writeback = 0xffffffffu;
    if (!_regs.write32(doorbell, next) ||
        !_regs.read32(doorbell, writeback) || writeback != next)
        return false;

    _runtimeRxSlot = slot;
    _runtimeRxNext = next;
    _runtimeLastCE1Read = snapshot.currentDstRead;
    _runtimeLastCE1Write = writeback;
    ++_runtimeRxPostedCount;
    ++_runtimeRxReplenishedCount;
    return true;
}

bool Ath10kPCIDevice::htcSubmitControlFrame(const UInt8 *request,
                                             UInt32 requestBytes,
                                             bool expectResponse,
                                             UInt16 expectedMessageId,
                                             UInt8 *response,
                                             UInt32 &responseBytes)
{
    const UInt32 txCE = Ath10k::kBMICENumToTarget;
    const UInt32 rxCE = Ath10k::kBMICENumToHost;
    const UInt32 txEntries = Ath10k::kCEHostConfig[txCE].srcEntries;
    const UInt32 rxEntries = Ath10k::kCEHostConfig[rxCE].dstEntries;
    const UInt32 responseCapacity = responseBytes;
    responseBytes = 0;
    _htcStatus = "control-prerequisite-mismatch";
    _htcControlLastWaitMs = 0;
    _htcControlLastRxBytes = 0;

    if (!request || requestBytes < Ath10k::kHTCHeaderBytes || requestBytes > Ath10k::kHTCControlMaxBytes ||
        (expectResponse && (!response || responseCapacity == 0 ||
                            responseCapacity > Ath10k::kHTCControlMaxBytes)) ||
        !_runtimeActive || !_runtimeCE1Armed || !_cePipesProgrammed ||
        !_boot.readyValidated || !_htcReadyBuffer.allocated() ||
        !_ceSrcRings[txCE].allocated() || !_ceDstRings[rxCE].allocated() ||
        txEntries == 0 || rxEntries == 0 || _dmaQuarantine ||
        _htcControlExchangeUncertain)
        return false;

    _htcStatus = "control-buffer-allocation-failed";
    if (!_htcControlBuffer.allocated() &&
        !_htcControlBuffer.allocate(Ath10k::kHTCControlMaxBytes,
                                    Ath10k::kCEDescriptorRingAlign, _dmaMapper))
        return false;

    _htcStatus = "control-ring-state-invalid";
    Ath10k::CESnapshot txBefore{};
    Ath10k::CESnapshot rxBefore{};
    if (!Ath10k::readCESnapshot(_regs, txCE, txBefore) ||
        !Ath10k::readCESnapshot(_regs, rxCE, rxBefore) ||
        !validateBMIPipeStructure(txCE, txBefore) ||
        !validateBMIPipeStructure(rxCE, rxBefore) ||
        txBefore.srcWriteIndex >= txEntries || txBefore.currentSrcRead >= txEntries ||
        txBefore.srcWriteIndex != txBefore.currentSrcRead ||
        rxBefore.dstWriteIndex >= rxEntries || rxBefore.currentDstRead >= rxEntries ||
        rxBefore.dstWriteIndex != _runtimeRxNext ||
        _runtimeRxSlot >= rxEntries)
        return false;

    auto *txRing = reinterpret_cast<Ath10k::CEDescriptor32 *>(
        _ceSrcRings[txCE].virtualAddress());
    auto *rxRing = reinterpret_cast<Ath10k::CEDescriptor32 *>(
        _ceDstRings[rxCE].virtualAddress());
    if (!_ceDstRings[rxCE].syncFromDevice() ||
        rxRing[_runtimeRxSlot].nbytes != 0 ||
        rxRing[_runtimeRxSlot].address != static_cast<UInt32>(_htcReadyBuffer.dmaAddress()))
        return false;

    _htcStatus = "control-tx-sync-failed";
    bzero(_htcControlBuffer.virtualAddress(),
          static_cast<size_t>(_htcControlBuffer.length()));
    bcopy(request, _htcControlBuffer.virtualAddress(), requestBytes);
    // Endpoint 0 sequence starts at zero; failed submissions require reset.
    reinterpret_cast<UInt8 *>(_htcControlBuffer.virtualAddress())[5] =
        static_cast<UInt8>(_htcControlTxCount);
    if (!_htcControlBuffer.syncForDevice())
        return false;

    const UInt32 txSlot = txBefore.srcWriteIndex;
    const UInt32 txNext = Ath10k::ceRingNext(txSlot, txEntries);
    auto *txDesc = &txRing[txSlot];
    bzero(txDesc, sizeof(*txDesc));
    txDesc->address = static_cast<UInt32>(_htcControlBuffer.dmaAddress());
    txDesc->nbytes = static_cast<UInt16>(requestBytes);
    txDesc->flags = 0; // HTC transfer_id is endpoint 0, not BMI anonymous metadata.
    OSSynchronizeIO();
    if (!_ceSrcRings[txCE].syncForDevice())
        return false;

    _htcControlLastTxSlot = txSlot;
    _htcControlLastTxNext = txNext;
    _htcControlLastTxBytes = requestBytes;
    _htcControlLastRxSlot = _runtimeRxSlot;
    _htcControlLastRxNext = _runtimeRxNext;
    _htcControlExchangeUncertain = true;

    const UInt32 txBase = Ath10k::ceBaseAddress(txCE);
    const UInt32 rxBase = Ath10k::ceBaseAddress(rxCE);
    UInt32 txWrite = 0xffffffffu;
    if (!_regs.write32(txBase + Ath10k::kCESrcWriteIndex, txNext) ||
        !_regs.read32(txBase + Ath10k::kCESrcWriteIndex, txWrite) ||
        txWrite != txNext) {
        _htcStatus = "control-submit-failed";
        return false;
    }

    UInt32 waitedMs = 0;
    UInt32 srcRead = 0xffffffffu;
    UInt32 dstRead = 0xffffffffu;
    UInt16 completedBytes = 0;
    bool txDone = false;
    bool rxDone = false;
    for (;;) {
        _htcControlLastWaitMs = waitedMs;
        OSSynchronizeIO();
        if (!_regs.read32(txBase + Ath10k::kCECurrentSrcRead, srcRead) ||
            !_regs.read32(rxBase + Ath10k::kCECurrentDstRead, dstRead) ||
            srcRead >= txEntries || dstRead >= rxEntries ||
            !_regs.read32(Ath10k::kQca6174RegisterLayout.fwIndicatorAddress,
                          _boot.fwIndicator) ||
            _boot.fwIndicator == 0xffffffffu) {
            _htcStatus = "control-poll-failed";
            return false;
        }
        _runtimeLastCE1Read = dstRead;
        if (_boot.fwIndicator & Ath10k::kFwIndicatorEventPending) {
            _boot.firmwareCrash = true;
            _htcStatus = "firmware-crash-indicated";
            ATH10K_TRACE("Ath10kMac: HTC firmware crash waited=%u ms CE0 src=%u/%u CE1 dst=%u/%u indicator=0x%x\n",
                  waitedMs, srcRead, txNext, dstRead, _runtimeRxNext, _boot.fwIndicator);
            return false;
        }
        if (!_ceDstRings[rxCE].syncFromDevice()) {
            _htcStatus = "control-rx-sync-failed";
            return false;
        }

        txDone = srcRead == txNext;
        completedBytes = rxRing[_runtimeRxSlot].nbytes;
        rxDone = completedBytes != 0;
        _htcControlLastRxBytes = completedBytes;
        if (txDone && (expectResponse ? rxDone : !rxDone))
            break;
        if (waitedMs >= Ath10k::kHTCServiceTimeoutMs)
            break;
        IOSleep(Ath10k::kHTCServicePollMs);
        ++waitedMs;
    }

    _htcControlLastWaitMs = waitedMs;
    _runtimeLastCE1Read = dstRead;
    _runtimeLastRxBytes = completedBytes;
    if (!txDone || (expectResponse && !rxDone) || (!expectResponse && rxDone)) {
        _htcStatus = !txDone ? "control-tx-timeout" :
            (expectResponse ? "control-response-timeout" : "unexpected-control-response");
        IOLog("Ath10kMac: HTC control TIMEOUT/UNEXPECTED response=%s waited=%u ms tx=%s rx-bytes=%u CE0 src=%u/%u CE1 dst=%u/%u\n",
              expectResponse ? "required" : "forbidden", waitedMs,
              txDone ? "done" : "pending", completedBytes, srcRead, txNext,
              dstRead, _runtimeRxNext);
        return false;
    }

    // The source descriptor is complete and can be reused. Keep the receive
    // descriptor owned until the response has been copied and reposted.
    txDesc->nbytes = 0;
    txDesc->flags = 0;
    OSSynchronizeIO();
    if (!_ceSrcRings[txCE].syncForDevice()) {
        _htcStatus = "control-tx-cleanup-failed";
        return false;
    }

    if (expectResponse) {
        if (completedBytes > responseCapacity ||
            completedBytes > _htcReadyBuffer.length() ||
            !_htcReadyBuffer.syncFromDevice()) {
            _htcStatus = "control-response-size-or-sync-failed";
            return false;
        }
        // Capture the synchronized packet before validation or RX repost clears it.
        // One bounded log line includes the entire observed 20-byte response.
        const auto *raw = reinterpret_cast<const UInt8 *>(_htcReadyBuffer.virtualAddress());
        const UInt32 prefixBytes = completedBytes < 32 ? completedBytes : 32;
        const char hex[] = "0123456789abcdef";
        char prefix[65]{};
        for (UInt32 i = 0; i < prefixBytes; ++i) {
            prefix[2 * i] = hex[raw[i] >> 4];
            prefix[2 * i + 1] = hex[raw[i] & 15];
        }
        ATH10K_TRACE("Ath10kMac: HTC CONTROL RX bytes=%u payload=%u flags=0x%02x trailer-field=%u prefix=%s\n",
              completedBytes, completedBytes >= 4 ? Ath10k::htcReadLE16(raw + 2) : 0,
              completedBytes >= 2 ? raw[1] : 0, completedBytes >= 5 ? raw[4] : 0, prefix);
        if (completedBytes < Ath10k::kHTCHeaderBytes + 2 ||
            Ath10k::htcReadLE16(reinterpret_cast<const UInt8 *>(
                _htcReadyBuffer.virtualAddress()) + Ath10k::kHTCHeaderBytes) !=
                expectedMessageId) {
            _htcStatus = "unexpected-control-response-id";
            return false;
        }
        bcopy(_htcReadyBuffer.virtualAddress(), response, completedBytes);
        responseBytes = completedBytes;
        ++_runtimeRxCompletedCount;
        ++_htcControlRxCount;
        _htcControlLastRxBytes = completedBytes;

        if (!htcRepostRuntimeReceive()) {
            _htcStatus = "control-rx-repost-failed";
            return false;
        }
        _htcControlExchangeUncertain = false;
    } else {
        _htcControlExchangeUncertain = false;
    }

    ++_htcControlTxCount;
    _htcStatus = expectResponse ? "control-exchange-complete" :
                                  "control-submit-complete";
    ATH10K_TRACE("Ath10kMac: HTC control %s tx=%u bytes=%u waited=%u ms CE0 src=%u->%u\n",
          expectResponse ? "EXCHANGE COMPLETE" : "SUBMIT COMPLETE",
          _htcControlTxCount, requestBytes, waitedMs, txSlot, txNext);
    return true;
}

bool Ath10kPCIDevice::runHTCServiceSetup()
{
    // Never erase ownership uncertainty or repeat service assignment without reset.
    if (_htcAttempted || _htcControlExchangeUncertain)
        return false;
    _htcAttempted = true;
    _htcSetupCompleteSent = false;
    _htcReservedControlConnected = false;
    _htcServiceSetupSucceeded = false;
    _htcControlExchangeUncertain = false;
    _htcCreditFlowDeferred = true;
    _htcControlTxCount = 0;
    _htcControlRxCount = 0;
    _htcControlLastTxBytes = 0;
    _htcControlLastRxBytes = 0;
    _htcControlLastTxSlot = 0xffffffffu;
    _htcControlLastTxNext = 0xffffffffu;
    _htcControlLastRxSlot = 0xffffffffu;
    _htcControlLastRxNext = 0xffffffffu;
    _htcControlLastWaitMs = 0;
    _htcStatus = "service-prerequisite-mismatch";
    _htcReservedControl = Ath10k::HTCServiceInfo{};
    _htcWMIControl = Ath10k::HTCServiceInfo{};
    _htcHTTData = Ath10k::HTCServiceInfo{};

    if (!_runtimeActive || !_runtimeCE1Armed || !_boot.readyValidated ||
        !_boot.ready.maxEndpoints || !_boot.ready.credits || _boot.ready.credits > 255 || !_cePipesProgrammed || !_htcReadyBuffer.allocated() ||
        _dmaQuarantine)
        return false;
    if (!_htcControlBuffer.allocated() &&
        !_htcControlBuffer.allocate(Ath10k::kHTCControlMaxBytes,
                                    Ath10k::kCEDescriptorRingAlign, _dmaMapper)) {
        _htcStatus = "control-buffer-allocation-failed";
        return false;
    }

    UInt8 reservedUplink = 0xff;
    UInt8 reservedDownlink = 0xff;
    if (!Ath10k::mapQCA6174ServiceToPipes(Ath10k::kSvcRsvdCtrl,
                                           reservedUplink, reservedDownlink)) {
        _htcStatus = "reserved-control-map-failed";
        return false;
    }
    _htcReservedControl.serviceId = Ath10k::kSvcRsvdCtrl;
    _htcReservedControl.endpoint = 0;
    _htcReservedControl.maxMessageSize = Ath10k::kHTCControlMaxBytes;
    _htcReservedControl.uplinkPipe = reservedUplink;
    _htcReservedControl.downlinkPipe = reservedDownlink;
    _htcReservedControl.status = Ath10k::kHTCServiceStatusSuccess;
    _htcReservedControl.connected = true;
    _htcReservedControl.creditFlowEnabled = false;
    _htcReservedControlConnected = true;
    ATH10K_TRACE("Ath10kMac: HTC reserved-control LOCAL endpoint=0 uplink=CE%u downlink=CE%u max=%u\n",
          reservedUplink, reservedDownlink, _htcReservedControl.maxMessageSize);

    UInt8 frame[Ath10k::kHTCControlMaxBytes]{};
    UInt8 response[Ath10k::kHTCControlMaxBytes]{};
    UInt32 responseBytes = sizeof(response);
    const UInt16 deferredFlags = Ath10k::kHTCConnFlagDisableCreditFlow;
    _htcStatus = "htt-data-build-failed";
    const UInt16 httService = static_cast<UInt16>(Ath10k::kSvcHTTData);
    const UInt32 httRequestBytes = Ath10k::buildHTCConnectServiceFrame(
        frame, sizeof(frame), httService, deferredFlags);
    if (!httRequestBytes ||
        !htcSubmitControlFrame(frame, httRequestBytes, true,
                                Ath10k::kHTCMessageConnectServiceResponse,
                                response, responseBytes)) {
        return false;
    }
    const char *httError = Ath10k::parseHTCConnectServiceResponse(
        response, responseBytes, httService, _boot.ready.maxEndpoints,
        _htcHTTData);
    if (httError) {
        _htcStatus = httError;
        IOLog("Ath10kMac: HTC SERVICE HTT_DATA INVALID reason=%s status=%u endpoint=%u max=%u\n",
              httError, _htcHTTData.status, _htcHTTData.endpoint,
              _htcHTTData.maxMessageSize);
        return false;
    }
    if (!Ath10k::mapQCA6174ServiceToPipes(Ath10k::kSvcHTTData,
                                           _htcHTTData.uplinkPipe,
                                           _htcHTTData.downlinkPipe)) {
        _htcStatus = "htt-data-map-failed";
        return false;
    }
    _htcHTTData.creditFlowEnabled = false;
    ATH10K_TRACE("Ath10kMac: HTC SERVICE HTT_DATA READY endpoint=%u max=%u uplink=CE%u downlink=CE%u\n",
          _htcHTTData.endpoint, _htcHTTData.maxMessageSize,
          _htcHTTData.uplinkPipe, _htcHTTData.downlinkPipe);

    responseBytes = sizeof(response);
    _htcStatus = "wmi-control-build-failed";
    const UInt16 wmiService = static_cast<UInt16>(Ath10k::kSvcWMIControl);
    const UInt32 wmiRequestBytes = Ath10k::buildHTCConnectServiceFrame(
        frame, sizeof(frame), wmiService, static_cast<UInt16>(_boot.ready.credits << 8));
    if (!wmiRequestBytes ||
        !htcSubmitControlFrame(frame, wmiRequestBytes, true,
                                Ath10k::kHTCMessageConnectServiceResponse,
                                response, responseBytes)) {
        return false;
    }
    const char *wmiError = Ath10k::parseHTCConnectServiceResponse(
        response, responseBytes, wmiService, _boot.ready.maxEndpoints,
        _htcWMIControl);
    if (wmiError) {
        _htcStatus = wmiError;
        IOLog("Ath10kMac: HTC SERVICE WMI_CONTROL INVALID reason=%s status=%u endpoint=%u max=%u\n",
              wmiError, _htcWMIControl.status, _htcWMIControl.endpoint,
              _htcWMIControl.maxMessageSize);
        return false;
    }
    if (!Ath10k::mapQCA6174ServiceToPipes(Ath10k::kSvcWMIControl,
                                           _htcWMIControl.uplinkPipe,
                                           _htcWMIControl.downlinkPipe)) {
        _htcStatus = "wmi-control-map-failed";
        return false;
    }
    _htcWMIControl.creditFlowEnabled = true;
    ATH10K_TRACE("Ath10kMac: HTC SERVICE WMI_CONTROL READY endpoint=%u max=%u uplink=CE%u downlink=CE%u credit-flow=negotiated-data-tx-deferred\n",
          _htcWMIControl.endpoint, _htcWMIControl.maxMessageSize,
          _htcWMIControl.uplinkPipe, _htcWMIControl.downlinkPipe);

    if (_htcWMIControl.endpoint == _htcHTTData.endpoint) {
        _htcStatus = "duplicate-service-endpoint";
        return false;
    }
    // SETUP_COMPLETE enables WMI SERVICE_READY. CE2 receive and event handling
    // must exist before starting it; this milestone only assigns endpoints.
    ATH10K_TRACE("Ath10kMac: HTC services connected; SETUP_COMPLETE awaits CE2 arm\n");
    _htcServiceSetupSucceeded = true;
    _htcStatus = "services-connected-setup-deferred";
    ATH10K_TRACE("Ath10kMac: Phase 3 step 2 HTC SERVICES READY reserved=endpoint0 WMI=endpoint%u HTT=endpoint%u tx=%u rx=%u setup-complete=deferred\n",
          _htcWMIControl.endpoint, _htcHTTData.endpoint,
          _htcControlTxCount, _htcControlRxCount);
    return true;
}

bool Ath10kPCIDevice::runWMIBootstrap()
{
    if (_wmi.attempted || _wmi.rxOwned)
        return false;
    _wmi.attempted = true;
    _wmi.status = "wmi-prerequisite-mismatch";
    if (!_runtimeActive || !_htcServiceSetupSucceeded || !_htcWMIControl.connected ||
        !_htcWMIControl.creditFlowEnabled || _htcSetupCompleteSent || _dmaQuarantine)
        return false;
    _wmi.creditsInitialized = _wmi.credits.initialize(
        _boot.ready.credits, _boot.ready.creditSize, _htcWMIControl.endpoint);
    if (!_wmi.creditsInitialized)
        return false;

    // Bounded bootstrap: a single CE2 descriptor, polled with interrupts masked.
    // Handle AVAILABLE before READY, then reset; no unserviced live WMI runtime.
    const UInt32 ce = 2, entries = Ath10k::kCEHostConfig[ce].dstEntries;
    const UInt32 base = Ath10k::ceBaseAddress(ce);
    _wmi.status = "wmi-rx-allocation-failed";
    if (!_wmiRxBuffer.allocated() &&
        !_wmiRxBuffer.allocate(Ath10k::kWMIRxBytes, Ath10k::kCEDescriptorRingAlign, _dmaMapper))
        return false;
    _wmi.status = "wmi-ce2-state-invalid";
    Ath10k::CESnapshot snap{};
    if (!entries || !Ath10k::readCESnapshot(_regs, ce, snap) ||
        !validateBMIPipeStructure(ce, snap) ||
        snap.dstWriteIndex >= entries || snap.currentDstRead >= entries ||
        snap.dstWriteIndex != snap.currentDstRead ||
        !_regs.write32(base + Ath10k::kCEHostIE, 0) ||
        !_regs.write32(base + Ath10k::kCEMiscIE, 0))
        return false;
    UInt32 mask = 0xffffffffu;
    if (!_regs.read32(base + Ath10k::kCEHostIE, mask) || mask != 0 ||
        !_regs.read32(base + Ath10k::kCEMiscIE, mask) || mask != 0)
        return false;
    _wmi.slot = snap.dstWriteIndex;
    _wmi.next = Ath10k::ceRingNext(_wmi.slot, entries);
    auto *ring = reinterpret_cast<Ath10k::CEDescriptor32 *>(_ceDstRings[ce].virtualAddress());
    _wmi.status = "wmi-rx-sync-failed";
    if (!_ceDstRings[ce].syncFromDevice())
        return false;
    auto *desc = &ring[_wmi.slot];
    bzero(desc, sizeof(*desc));
    desc->address = static_cast<UInt32>(_wmiRxBuffer.dmaAddress());
    bzero(_wmiRxBuffer.virtualAddress(), static_cast<size_t>(_wmiRxBuffer.length()));
    OSSynchronizeIO();
    if (!_wmiRxBuffer.syncForDevice() || !_ceDstRings[ce].syncForDevice())
        return false;
    _wmi.rxOwned = true; // Doorbell may take effect even if its readback fails.
    _wmi.status = "wmi-rx-doorbell-failed";
    UInt32 writeback = 0xffffffffu;
    if (!_regs.write32(base + Ath10k::kCEDstWriteIndex, _wmi.next) ||
        !_regs.read32(base + Ath10k::kCEDstWriteIndex, writeback) || writeback != _wmi.next)
        return false;
    _wmi.rxPosted = true;
    ATH10K_TRACE("Ath10kMac: WMI CE2 RX ARMED slot=%u next=%u bytes=%u credits=%u size=%u\n",
          _wmi.slot, _wmi.next, Ath10k::kWMIRxBytes, _wmi.credits.available, _wmi.credits.size);

    UInt8 setup[Ath10k::kHTCSetupCompleteExFrameBytes]{};
    const UInt32 length = Ath10k::buildHTCSetupCompleteExFrame(setup, sizeof(setup), 0);
    UInt32 unused = 0;
    if (!htcSubmitControlFrame(setup, length, false, 0, nullptr, unused)) {
        _wmi.status = _htcStatus;
        return false;
    }
    _htcSetupCompleteSent = true; // CE consumed, not yet firmware service-ready.
    ATH10K_TRACE("Ath10kMac: HTC SETUP_COMPLETE_EX consumed; waiting for WMI SERVICE_READY\n");
    for (_wmi.waitMs = 0; ; ++_wmi.waitMs) {
        _wmi.status = "wmi-poll-failed";
        UInt32 read = 0xffffffffu;
        if (!_regs.read32(base + Ath10k::kCECurrentDstRead, read) || read >= entries ||
            !_regs.read32(Ath10k::kQca6174RegisterLayout.fwIndicatorAddress, _boot.fwIndicator) ||
            _boot.fwIndicator == 0xffffffffu)
            return false;
        if (_boot.fwIndicator & Ath10k::kFwIndicatorEventPending) {
            _boot.firmwareCrash = true;
            _wmi.status = "wmi-firmware-crash-indicated";
            return false;
        }
        OSSynchronizeIO();
        _wmi.status = "wmi-rx-sync-failed";
        if (!_ceDstRings[ce].syncFromDevice()) return false;
        _wmi.rxBytes = desc->nbytes;
        if (_wmi.rxBytes) {
            _wmi.status = "wmi-rx-descriptor-invalid";
            if (_wmi.rxBytes > _wmiRxBuffer.length() ||
                desc->address != static_cast<UInt32>(_wmiRxBuffer.dmaAddress()) ||
                !_wmiRxBuffer.syncFromDevice())
                return false;
            const auto *data = reinterpret_cast<const UInt8 *>(_wmiRxBuffer.virtualAddress());
            char prefix[65]{};
            const char hex[] = "0123456789abcdef";
            for (UInt32 i = 0; i < _wmi.rxBytes && i < 32; ++i) {
                prefix[2*i] = hex[data[i] >> 4]; prefix[2*i+1] = hex[data[i] & 15];
            }
            ATH10K_TRACE("Ath10kMac: WMI CE2 RX bytes=%u waited=%u ms prefix=%s\n", _wmi.rxBytes, _wmi.waitMs, prefix);
            const char *error = Ath10k::parseWMIBootstrapEvent(data, _wmi.rxBytes, _htcWMIControl.endpoint, _wmi);
            if (error) {
                _wmi.status = error;
                IOLog("Ath10kMac: WMI bootstrap PARSE ERROR reason=%s event=%u reg-tlv-bytes=%u reg-tlv-offset=%u reg-tlv-count=%u rx-bytes=%u\n",
                      error,_wmi.eventId,_wmi.regCapsBytes,_wmi.regCapsOffset,_wmi.regCapsCount,_wmi.rxBytes);
                return false;
            }
            ++_wmi.rxCount;
            if (_wmi.serviceReady) {
                _wmi.status = "service-ready-before-reset";
                ATH10K_TRACE("Ath10kMac: WMI SERVICE_READY VERIFIED event=%u build=0x%x abi=0x%x chains=%u mem-requests=%u reg-tlv-bytes=%u reg-tlv-count=%u; preparing WMI_INIT\n",
                      _wmi.eventId, _wmi.fwBuild, _wmi.abi0, _wmi.rfChains, _wmi.memoryRequests,
                      _wmi.regCapsBytes,_wmi.regCapsCount);
                return true;
            }
            ATH10K_TRACE("Ath10kMac: WMI SERVICE_AVAILABLE VERIFIED count=%u bits=%u; awaiting SERVICE_READY\n",
                  _wmi.availableCount, _wmi.availableBits);
            // Bound even an uninterrupted stream of valid AVAILABLE events.
            if (_wmi.availableCount >= 8) { _wmi.status = "wmi-event-limit"; return false; }
            if (_wmi.waitMs >= Ath10k::kWMIServiceReadyTimeoutMs) {
                _wmi.status = "wmi-service-ready-timeout"; return false;
            }
            if (!repostWMIBuffer()) return false;
            desc=&ring[_wmi.slot];
        }
        if (_wmi.waitMs >= Ath10k::kWMIServiceReadyTimeoutMs) {
            _wmi.status = "wmi-service-ready-timeout"; return false;
        }
        IOSleep(1);
    }
}

bool Ath10kPCIDevice::repostWMIBuffer()
{
    const char *priorStatus=_wmi.status;
    const UInt32 ce=2, entries=Ath10k::kCEHostConfig[ce].dstEntries;
    _wmi.status="wmi-repost-state-invalid";
    Ath10k::CESnapshot snap{};
    if (!_wmi.rxOwned || _wmi.slot>=entries || _wmi.next>=entries ||
        !Ath10k::readCESnapshot(_regs,ce,snap) || !validateBMIPipeStructure(ce,snap) ||
        snap.dstWriteIndex!=_wmi.next || snap.currentDstRead>=entries ||
        !_ceDstRings[ce].syncFromDevice()) return false;
    auto *ring=reinterpret_cast<Ath10k::CEDescriptor32 *>(_ceDstRings[ce].virtualAddress());
    auto *old=&ring[_wmi.slot];
    if (!old->nbytes || old->address!=static_cast<UInt32>(_wmiRxBuffer.dmaAddress())) return false;
    const UInt32 slot=_wmi.next,next=Ath10k::ceRingNext(slot,entries);
    bzero(old,sizeof(*old));
    auto *desc=&ring[slot]; bzero(desc,sizeof(*desc));
    desc->address=static_cast<UInt32>(_wmiRxBuffer.dmaAddress());
    bzero(_wmiRxBuffer.virtualAddress(),static_cast<size_t>(_wmiRxBuffer.length()));
    OSSynchronizeIO();
    _wmi.status="wmi-repost-sync-failed";
    if (!_wmiRxBuffer.syncForDevice() || !_ceDstRings[ce].syncForDevice()) return false;
    _wmi.slot=slot;_wmi.next=next;
    UInt32 back=0xffffffffu;_wmi.status="wmi-repost-doorbell-failed";
    if (!_regs.write32(Ath10k::ceBaseAddress(ce)+Ath10k::kCEDstWriteIndex,next) ||
        !_regs.read32(Ath10k::ceBaseAddress(ce)+Ath10k::kCEDstWriteIndex,back) || back!=next) return false;
    ++_wmi.repostCount;_wmi.status=priorStatus;return true;
}

bool Ath10kPCIDevice::runWMIInit()
{
    if (_wmiInit.attempted || _wmiInit.txOwned) return false;
    _wmiInit.attempted=true;_wmiInit.status="wmi-init-prerequisite";
    if (!_runtimeActive || !_wmi.serviceReady || !_wmi.rxOwned || !_htcSetupCompleteSent ||
        _dmaQuarantine || !_htcWMIControl.connected || _htcWMIControl.uplinkPipe!=3 ||
        _htcWMIControl.maxMessageSize<Ath10k::kWMIInitFrameBytes-8) return false;
    _wmiInit.status="wmi-init-allocation-failed";
    if (!_wmiTxBuffer.allocated() && !_wmiTxBuffer.allocate(Ath10k::kWMIScanChannelsFrameBytes,
        Ath10k::kCEDescriptorRingAlign,_dmaMapper)) return false;
    _wmiInit.hostMemoryCount=_wmi.hostMemoryCount;
    _wmiInit.frameBytes=Ath10k::kWMIInitFrameBytes+20*_wmiInit.hostMemoryCount;
    if(_wmiInit.frameBytes-8>_htcWMIControl.maxMessageSize || _wmiInit.hostMemoryCount>Ath10k::kWMIHostMemoryMax)return false;
    for(UInt32 i=0;i<_wmiInit.hostMemoryCount;++i){
        const auto &r=_wmi.hostMemory[i];const UInt32 size=Ath10k::hostMemoryRequestBytes(r);auto &buffer=_wmiHostMemory[i];
        if(!size || (!buffer.allocated() && !buffer.allocate(size,128,_dmaMapper)) ||
           buffer.length()!=size || !Ath10k::dmaSpan32(buffer.dmaAddress(),size)){
            _wmiInit.status="wmi-host-memory-allocation";return false;}
        bzero(buffer.virtualAddress(),size);
        if(!buffer.syncForDevice()){_wmiInit.status="wmi-host-memory-sync";return false;}
        auto &chunk=_wmiInit.hostMemory[i];chunk.id=r.id;chunk.bytes=size;chunk.address=static_cast<UInt32>(buffer.dmaAddress());
    }
    const char *error=Ath10k::buildWMIInit(reinterpret_cast<UInt8 *>(_wmiTxBuffer.virtualAddress()),
        static_cast<UInt32>(_wmiTxBuffer.length()),_wmi,_htcWMIControl.endpoint,_deviceId==Ath10k::kQCA9377Device?1:2,_wmiInit.hostMemory,_wmiInit.hostMemoryCount);
    if (error) { _wmiInit.status=error;return false; }
    _wmiInit.requestedChainMask=Ath10k::wmiReadLE32(
        static_cast<const UInt8 *>(_wmiTxBuffer.virtualAddress())+76);
    ATH10K_TRACE("Ath10kMac: WMI INIT RF chains=%u requested-tx-mask=0x%x requested-rx-mask=0x%x source=SERVICE_READY\n",
          _wmi.rfChains,_wmiInit.requestedChainMask,_wmiInit.requestedChainMask);
    const UInt32 ce=3, entries=Ath10k::kCEHostConfig[ce].srcEntries;
    Ath10k::CESnapshot snap{};_wmiInit.status="wmi-init-ce3-state";
    if (!Ath10k::readCESnapshot(_regs,ce,snap) || !validateBMIPipeStructure(ce,snap) ||
        snap.srcWriteIndex>=entries || snap.currentSrcRead!=snap.srcWriteIndex) return false;
    // Arm response before submitting INIT. Never resend INIT after uncertain DMA.
    if (!repostWMIBuffer()) { _wmiInit.status=_wmi.status;return false; }
    _wmiInit.txSlot=snap.srcWriteIndex;_wmiInit.txNext=Ath10k::ceRingNext(_wmiInit.txSlot,entries);
    auto *ring=reinterpret_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[ce].virtualAddress());
    auto *tx=&ring[_wmiInit.txSlot];bzero(tx,sizeof(*tx));
    tx->address=static_cast<UInt32>(_wmiTxBuffer.dmaAddress());
    tx->nbytes=static_cast<UInt16>(_wmiInit.frameBytes);
    tx->flags=static_cast<UInt16>(_htcWMIControl.endpoint<<2); // QCA6174 metadata shift.
    OSSynchronizeIO();_wmiInit.status="wmi-init-tx-sync";
    if (!_wmiTxBuffer.syncForDevice() || !_ceSrcRings[ce].syncForDevice()) return false;
    _wmiInit.status="wmi-init-no-credits";
    if (!_wmi.credits.reserve(_wmiInit.frameBytes)) return false;
    _wmiInit.txOwned=true;
    _wmiInit.hostMemoryOwned=_wmiInit.hostMemoryCount!=0; // Firmware may DMA until verified reset/bus-master disable.
    UInt32 back=0xffffffffu;_wmiInit.status="wmi-init-submit-failed";
    const UInt32 base=Ath10k::ceBaseAddress(ce);
    if (!_regs.write32(base+Ath10k::kCESrcWriteIndex,_wmiInit.txNext) ||
        !_regs.read32(base+Ath10k::kCESrcWriteIndex,back) || back!=_wmiInit.txNext) return false;
    _wmiInit.submitted=true;
    ATH10K_TRACE("Ath10kMac: WMI INIT SUBMITTED CE3 endpoint=%u bytes=%u credits=%u outstanding=%u\n",
          _htcWMIControl.endpoint,_wmiInit.frameBytes,_wmi.credits.available,_wmi.credits.outstanding);
    for (_wmiInit.waitMs=0;;++_wmiInit.waitMs) {
        _wmiInit.status="wmi-init-poll-failed";
        UInt32 read=0xffffffffu;
        if (!_regs.read32(base+Ath10k::kCECurrentSrcRead,read) || read>=entries ||
            !_regs.read32(Ath10k::kQca6174RegisterLayout.fwIndicatorAddress,_boot.fwIndicator) ||
            _boot.fwIndicator==0xffffffffu) return false;
        if (_boot.fwIndicator&Ath10k::kFwIndicatorEventPending) {
            _boot.firmwareCrash=true;_wmiInit.status="wmi-init-firmware-crash";return false;
        }
        _wmiInit.txConsumed=read==_wmiInit.txNext;
        // Credit reports may arrive separately on CE1 or piggybacked on CE2.
        for (UInt32 rxCE=1;rxCE<=2 &&
             !(_wmiInit.ready && _wmiInit.creditReturned && _wmiInit.txConsumed);++rxCE) {
            const UInt32 slot=rxCE==1?_runtimeRxSlot:_wmi.slot;
            auto *rxRing=reinterpret_cast<Ath10k::CEDescriptor32 *>(_ceDstRings[rxCE].virtualAddress());
            if (slot>=Ath10k::kCEHostConfig[rxCE].dstEntries || !_ceDstRings[rxCE].syncFromDevice()) return false;
            auto *desc=&rxRing[slot];const UInt32 bytes=desc->nbytes;
            if (!bytes) continue;
            auto &buffer=rxCE==1?_htcReadyBuffer:_wmiRxBuffer;
            _wmiInit.status="wmi-init-rx-descriptor";
            if (desc->address!=static_cast<UInt32>(buffer.dmaAddress()) || bytes>buffer.length() ||
                !buffer.syncFromDevice()) return false;
            const auto *raw=reinterpret_cast<const UInt8 *>(buffer.virtualAddress());
            const char hex[]="0123456789abcdef";char prefix[129]{};
            for (UInt32 i=0;i<bytes && i<64;++i) {
                prefix[2*i]=hex[raw[i]>>4];prefix[2*i+1]=hex[raw[i]&15];
            }
            ATH10K_TRACE("Ath10kMac: WMI INIT RX RAW CE%u bytes=%u prefix=%s\n",rxCE,bytes,prefix);
            error=Ath10k::parseWMIInitResponse(reinterpret_cast<const UInt8 *>(buffer.virtualAddress()),
                bytes,_htcWMIControl.endpoint,_wmi.credits,_wmiInit);
            if (error) {_wmiInit.status=error;return false;}
            ++_wmiInit.rxCount;
            if (_wmiInit.lastEventId==0x1d011)
                ATH10K_TRACE("Ath10kMac: WMI DIAG ACCEPTED count=%u payload-total=%u; continuing credit wait\n",
                      _wmiInit.diagCount,_wmiInit.diagBytes);
            if (_wmiInit.lastEventId==0x1d019)
                ATH10K_TRACE("Ath10kMac: WMI PKGID ACCEPTED count=%u id=0x%08x; continuing credit wait\n",
                      _wmiInit.pkgIdCount,_wmiInit.pkgId);
            if (_wmiInit.lastEventId==0x16006)
                ATH10K_TRACE("Ath10kMac: WMI DIAG SUPPORTED count=%u entries=%u; continuing INIT\n",
                      _wmiInit.diagSupportedCount,_wmiInit.diagSupportedEntries);
            ATH10K_TRACE("Ath10kMac: WMI INIT RX CE%u bytes=%u ready=%s credits=%u outstanding=%u\n",
                  rxCE,bytes,_wmiInit.ready?"yes":"no",_wmi.credits.available,_wmi.credits.outstanding);
            if (_wmiInit.rxCount>32) {_wmiInit.status="wmi-init-event-limit";return false;}
            if (rxCE==1) {
                if (!htcRepostRuntimeReceive()) {_wmiInit.status="wmi-init-ce1-repost";return false;}
            } else if (!repostWMIBuffer()) {_wmiInit.status=_wmi.status;return false;}
            // The loop condition now hands any pending packet on the other CE
            // to the persistent receiver once READY/TX/credit are complete.
        }
        if (_wmiInit.ready && _wmiInit.creditReturned && _wmiInit.txConsumed) {
            _wmiInit.completed=true;
            _htcCreditFlowDeferred=false;
            _wmiInit.status="ready-and-credit-returned";
            ATH10K_TRACE("Ath10kMac: WMI INIT PASS READY status=0 credit-returned=yes waited=%u ms\n",_wmiInit.waitMs);
            return true;
        }
        if (_wmiInit.waitMs>=5000) {
            _wmiInit.status=!_wmiInit.txConsumed?"wmi-init-tx-timeout":
                (!_wmiInit.ready?"wmi-ready-timeout":"wmi-credit-return-timeout");return false;
        }
        IOSleep(1);
    }
}

const char *Ath10kPCIDevice::serviceWMIEcho(UInt64 nowMs)
{
    if (_wmiEcho.completed) return nullptr;
    if (!_wmiEcho.attempted) {
        _wmiEcho.attempted=true;_wmiEcho.startedMs=nowMs;
        _wmiEcho.status="wmi-echo-prerequisite";
        if (!_wmiLive.active || !_runtimeActive || !_wmiInit.completed ||
            !_wmiInit.txConsumed || !_wmi.rxOwned || !_runtimeCE1Armed || _dmaQuarantine ||
            !_htcWMIControl.connected || _htcWMIControl.uplinkPipe!=3 ||
            _htcWMIControl.maxMessageSize<12 || _wmi.credits.outstanding ||
            !_wmiTxBuffer.allocated() || !_ceSrcRings[3].allocated()) return _wmiEcho.status;
        Ath10k::CESnapshot snap{};
        const UInt32 entries=Ath10k::kCEHostConfig[3].srcEntries;
        _wmiEcho.status="wmi-echo-ce3-state";
        if (!Ath10k::readCESnapshot(_regs,3,snap) || !validateBMIPipeStructure(3,snap) ||
            snap.srcWriteIndex>=entries || snap.currentSrcRead!=snap.srcWriteIndex ||
            snap.srcWriteIndex!=_wmiInit.txNext || !_ceSrcRings[3].syncFromDevice())
            return _wmiEcho.status;
        // INIT has been consumed and the CE3 ring is idle: its retained DMA
        // buffer can now hold ECHO. The allocation remains retained until reset.
        _wmiEcho.status="wmi-echo-build";
        if (!Ath10k::buildWMIEcho(static_cast<UInt8 *>(_wmiTxBuffer.virtualAddress()),
            static_cast<UInt32>(_wmiTxBuffer.length()),_htcWMIControl.endpoint,_wmiEcho.cookie))
            return _wmiEcho.status;
        _wmiEcho.txSlot=snap.srcWriteIndex;
        _wmiEcho.txNext=Ath10k::ceRingNext(_wmiEcho.txSlot,entries);
        auto *ring=static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[3].virtualAddress());
        auto *desc=&ring[_wmiEcho.txSlot];bzero(desc,sizeof(*desc));
        desc->address=static_cast<UInt32>(_wmiTxBuffer.dmaAddress());
        desc->nbytes=Ath10k::kWMIEchoFrameBytes;
        desc->flags=static_cast<UInt16>(_htcWMIControl.endpoint<<2);
        OSSynchronizeIO();_wmiEcho.status="wmi-echo-tx-sync";
        if (!_wmiTxBuffer.syncForDevice() || !_ceSrcRings[3].syncForDevice()) return _wmiEcho.status;
        _wmiEcho.status="wmi-echo-no-credit";
        if (!_wmi.credits.reserve(Ath10k::kWMIEchoFrameBytes)) return _wmiEcho.status;
        // Never refund or retry after an uncertain doorbell.
        _wmiEcho.txOwned=true;_wmiEcho.status="wmi-echo-submit-failed";
        UInt32 back=0xffffffffu;
        const UInt32 doorbell=Ath10k::ceBaseAddress(3)+Ath10k::kCESrcWriteIndex;
        if (!_regs.write32(doorbell,_wmiEcho.txNext) || !_regs.read32(doorbell,back) || back!=_wmiEcho.txNext)
            return _wmiEcho.status;
        _wmiEcho.submitted=true;_wmiEcho.status="waiting-for-echo-and-credit";
        ATH10K_TRACE("Ath10kMac: WMI ECHO SUBMITTED CE3 cookie=0x%08x sequence=1 bytes=%u credits=%u outstanding=%u\n",
              _wmiEcho.cookie,Ath10k::kWMIEchoFrameBytes,_wmi.credits.available,_wmi.credits.outstanding);
    }
    if (!_wmiEcho.submitted) return _wmiEcho.status;
    _wmiEcho.elapsedMs=nowMs-_wmiEcho.startedMs;
    UInt32 read=0xffffffffu;
    if (!_regs.read32(Ath10k::ceBaseAddress(3)+Ath10k::kCECurrentSrcRead,read) ||
        (read!=_wmiEcho.txSlot && read!=_wmiEcho.txNext)) {
        _wmiEcho.status="wmi-echo-tx-index";return _wmiEcho.status;
    }
    _wmiEcho.txConsumed=read==_wmiEcho.txNext;
    _wmiEcho.creditReturned=_wmi.credits.outstanding==0;
    if (_wmiEcho.elapsedMs>=5000) {
        _wmiEcho.status=!_wmiEcho.txConsumed?"wmi-echo-tx-timeout":
            (!_wmiEcho.replyReceived?"wmi-echo-reply-timeout":
             (!_wmiEcho.creditReturned?"wmi-echo-credit-timeout":"wmi-echo-deadline"));
        return _wmiEcho.status;
    }
    if (_wmiEcho.txConsumed && _wmiEcho.replyReceived && _wmiEcho.creditReturned) {
        _wmiEcho.completed=true;_wmiEcho.status="reply-and-credit-returned";
        ATH10K_TRACE("Ath10kMac: WMI ECHO PASS cookie=0x%08x tx-consumed=yes reply=yes credit-returned=yes elapsed=%llu ms\n",
              _wmiEcho.cookie,static_cast<unsigned long long>(_wmiEcho.elapsedMs));
    }
    return nullptr;
}

const char *Ath10kPCIDevice::serviceRxSetup(UInt64 nowMs)
{
    auto &s=_rxSetup;
    if(!_wmiLive.active || !_runtimeActive || _dmaQuarantine)return "rx-setup-inactive";
    if(s.completed)return nullptr;
    if(s.ringOwned && !s.submitted && s.stage==4)return "rx-setup-ring-owned";
    if(!s.attempted){
        s.attempted=true;s.wmiNext=_wmiEcho.txNext;
        if(!_wmiLive.active || !_runtimeActive || !_wmiInit.ready || !_wmiEcho.completed ||
           !_wmiEcho.txConsumed || !_htcHTTData.connected || _htcHTTData.uplinkPipe!=4 ||
           _htcHTTData.downlinkPipe!=1 || _htcHTTData.creditFlowEnabled || _dmaQuarantine)
            return "rx-setup-prerequisite";
        if(!_httTxBuffer.allocate(64,8,_dmaMapper))return "htt-tx-allocation";
        _filterEcho={};_filterEcho.cookie=Ath10k::kHTTSetupCookie;
    }
    if(s.stage>=6)return "rx-setup-stage";
    const UInt32 ce=s.stage<3?3:4;
    auto &buffer=ce==3?_wmiTxBuffer:_httTxBuffer;
    if(!s.submitted){
        if(s.stage==4){
            // One diagnostic pool, never recycled or rewritten while DMA is live.
            const UInt64 poolBytes=UInt64(Ath10k::kHTTRxPosted)*Ath10k::kHTTRxBufferBytes;
            if(!_httRxIndex.allocate(8,8,_dmaMapper) ||
               !_httRxRing.allocate(Ath10k::kHTTRingEntries*4,8,_dmaMapper) ||
               !_httRxPool.allocate(poolBytes,128,_dmaMapper,Ath10kDMABuffer::Role::Payload))return "htt-rx-allocation";
            if(!Ath10k::dmaSpan32(_httRxPool.dmaAddress(),poolBytes) ||
               !Ath10k::dmaSpan32(_httRxRing.dmaAddress(),_httRxRing.length()) ||
               !Ath10k::dmaSpan32(_httRxIndex.dmaAddress(),_httRxIndex.length()))return "htt-rx-address";
            auto *ring=static_cast<UInt8 *>(_httRxRing.virtualAddress());
            auto *pool=static_cast<UInt8 *>(_httRxPool.virtualAddress());
            bzero(ring,static_cast<size_t>(_httRxRing.length()));
            bzero(pool,static_cast<size_t>(poolBytes));
            for(UInt32 i=0;i<Ath10k::kHTTRxPosted;++i){
                s.bufferIds[i]=static_cast<UInt16>(i);
                s.addresses[i]=static_cast<UInt32>(_httRxPool.dmaAddress()+UInt64(i)*Ath10k::kHTTRxBufferBytes);
            }
            for(UInt32 i=0;i<Ath10k::kHTTRxPosted;++i)
                Ath10k::htcWriteLE32(ring+4*i,static_cast<UInt32>(_httRxPool.dmaAddress()+UInt64(i)*Ath10k::kHTTRxBufferBytes));
            Ath10k::htcWriteLE32(static_cast<UInt8 *>(_httRxIndex.virtualAddress()),Ath10k::kHTTRxPosted);
            if(!_httRxPool.syncForDevice() || !_httRxRing.syncForDevice() || !_httRxIndex.syncForDevice())return "htt-rx-sync";
        }
        Ath10k::CESnapshot snap{};
        const UInt32 entries=Ath10k::kCEHostConfig[ce].srcEntries;
        if(!_ceSrcRings[ce].allocated() || !Ath10k::readCESnapshot(_regs,ce,snap) ||
           !validateBMIPipeStructure(ce,snap) || snap.srcWriteIndex>=entries ||
           snap.srcWriteIndex!=snap.currentSrcRead ||
           (ce==3 && snap.srcWriteIndex!=s.wmiNext) ||
           (ce==4 && s.stage>3 && snap.srcWriteIndex!=s.httNext) ||
           !_ceSrcRings[ce].syncFromDevice())return "rx-setup-ce-state";
        if(ce==3 && _wmi.credits.outstanding)return "rx-setup-credit-outstanding";
        auto *frame=static_cast<UInt8 *>(buffer.virtualAddress());
        const UInt32 bytes=Ath10k::buildRxSetup(frame,static_cast<UInt32>(buffer.length()),s.stage,
            _htcWMIControl.endpoint,_htcHTTData.endpoint,_wmiInit.mac,
            _httRxIndex.dmaAddress(),_httRxRing.dmaAddress());
        if(!Ath10k::dmaSpan32(buffer.dmaAddress(),bytes))return "rx-setup-tx-address";
        if(!bytes || (bytes-8)>(ce==3?_htcWMIControl.maxMessageSize:_htcHTTData.maxMessageSize))return "rx-setup-build";
        s.txSlot=snap.srcWriteIndex;s.txNext=Ath10k::ceRingNext(s.txSlot,entries);
        auto *desc=static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[ce].virtualAddress())+s.txSlot;
        bzero(desc,sizeof(*desc));desc->address=static_cast<UInt32>(buffer.dmaAddress());
        desc->nbytes=static_cast<UInt16>(bytes);desc->flags=static_cast<UInt16>(frame[0]<<2);
        OSSynchronizeIO();
        if(!buffer.syncForDevice() || !_ceSrcRings[ce].syncForDevice())return "rx-setup-tx-sync";
        if(ce==3 && !_wmi.credits.reserve(bytes))return "rx-setup-no-credit";
        if(s.stage==2)_filterEcho.submitted=true;
        if(s.stage==3)s.versionRequested=true;
        // Ownership precedes the first doorbell attempt, even if MMIO fails.
        s.txOwned=true;if(s.stage==4)s.ringOwned=true;
        s.startedMs=nowMs;
        UInt32 back=0xffffffffu;
        const UInt32 doorbell=Ath10k::ceBaseAddress(ce)+Ath10k::kCESrcWriteIndex;
        s.status="rx-setup-submit-failed";
        if(!_regs.write32(doorbell,s.txNext) || !_regs.read32(doorbell,back) || back!=s.txNext)return s.status;
        s.submitted=true;s.status="waiting-for-setup-stage";
        ATH10K_TRACE("Ath10kMac: RX SETUP SUBMITTED stage=%u CE%u bytes=%u\n",s.stage,ce,bytes);
    }
    UInt32 read=0xffffffffu;
    if(!_regs.read32(Ath10k::ceBaseAddress(ce)+Ath10k::kCECurrentSrcRead,read) ||
       (read!=s.txSlot && read!=s.txNext))return "rx-setup-tx-index";
    const bool consumed=read==s.txNext;
    const bool creditsOK=ce!=3 || !_wmi.credits.outstanding;
    const bool responseOK=(s.stage!=2 || _filterEcho.replyReceived) && (s.stage!=3 || s.versionReceived);
    if(nowMs-s.startedMs>=5000)return !consumed?"rx-setup-tx-timeout":
        (!creditsOK?"rx-setup-credit-timeout":"rx-setup-response-timeout");
    if(consumed && creditsOK && responseOK){
        if(ce==3)s.wmiNext=s.txNext;else s.httNext=s.txNext;
        ATH10K_TRACE("Ath10kMac: RX SETUP STAGE COMPLETE stage=%u CE%u version=%u.%u\n",s.stage,ce,s.major,s.minor);
        ++s.stage;s.submitted=false;
        if(s.stage==6){s.completed=true;s.status="transport-complete-ordered-rx-ready";}
    }
    return nullptr;
}

const char *Ath10kPCIDevice::serviceHTTRx()
{
    auto &s=_rxSetup;UInt32 processed=0;bool poolSynced=false;
    bool waitingForDone=false;
    if(s.pendingMPDUs>s.pendingHigh)s.pendingHigh=s.pendingMPDUs;
    if(s.pendingMPDUs)++s.batchCalls;
    while(s.pendingMPDUs && processed<Ath10k::kHTTRxBudget){
        const char *error=serviceHTTRxOne(poolSynced);
        if(error){
            // Missing DONE is not proof of completed DMA. Keep ownership and
            // indices unchanged, retry on the next tick, but stop safely if
            // the descriptor never becomes ready. Linux treats this as EIO.
            if(!strcmp(error,"htt-rx-completion-not-ready") || !strcmp(error,"htt-rx-chain-not-ready")){
                waitingForDone=true;break;
            }
            return error;
        }
        s.rxDoneConsecutiveWaits=0;
        ++processed;
    }
    if(processed>s.batchMax)s.batchMax=processed;
    if(s.pendingMPDUs && processed==Ath10k::kHTTRxBudget)++s.budgetHits;
    if(waitingForDone){
        ++s.rxCompletionWaits;
        if(++s.rxDoneConsecutiveWaits>=Ath10k::kHTTRxDoneRetryLimit){
            ++s.rxDoneTimeouts;
            IOLog("Ath10kMac: HTT RX DONE timeout mode=%u buffer=%u attention=0x%08x pending=%u read=%u status-read=%u waits=%u recycled=%u\n",
                  s.rxMode,s.rxDoneLastBuffer,s.rxDoneLastAttention,s.pendingMPDUs,
                  s.readIndex,s.statusRead,s.rxDoneConsecutiveWaits,s.recycled);
            return "htt-rx-completion-done-timeout";
        }
    }
    return nullptr;
}

const char *Ath10kPCIDevice::serviceHTTRxOne(bool &poolSynced)
{
    auto &s=_rxSetup;
    if(!s.pendingMPDUs)return nullptr;
    if(!_wmiLive.active || !s.ringOwned || _dmaQuarantine)return "htt-rx-inactive";
    // One completion; the caller bounds the batch.  A QCA6174 descriptor can
    // announce additional ring2 buffers through frag_info.ring2_more_count
    // (byte 10).  The chained buffers contain payload only, so join them with
    // the first descriptor before decoding and recycle every ownership slot.
    // Bound ownership processing separately from the reusable payload scratch
    // on the IOService. Never place an aggregate-sized array on the kernel
    // stack, even though the ring itself has 1023 posted entries.
    static constexpr UInt32 kServiceBuffers=256;
    UInt16 ids[kServiceBuffers]{};
    UInt16 lengths[kServiceBuffers]{},peers[kServiceBuffers]{};
    UInt8 statuses[kServiceBuffers]{},chainCounts[kServiceBuffers]{};
    UInt32 count=0,logical=0,metadataConsumed=0;bool last=false,splitMPDU=false;
    bzero(_rxServiceData,sizeof(_rxServiceData));
    auto bufferData=[&](UInt16 id)->const UInt8 *{
        for(UInt32 i=0;i<count;++i)if(ids[i]==id && _rxServiceData[i])return _rxServiceData[i];
        if(_httRecycled[id].allocated()){
            // The recycled mapping is a distinct IODMACommand/bounce buffer.
            // A previous syncForDevice() only made the buffer visible to the
            // device; it does not invalidate the CPU view after RX DMA.
            if(!_httRecycled[id].syncFromDevice())return static_cast<const UInt8 *>(nullptr);
            return static_cast<const UInt8 *>(_httRecycled[id].virtualAddress());
        }
        if(!poolSynced){if(!_httRxPool.syncFromDevice())return static_cast<const UInt8 *>(nullptr);poolSynced=true;}
        return static_cast<const UInt8 *>(_httRxPool.virtualAddress())+UInt64(id)*Ath10k::kHTTRxBufferBytes;
    };
    while(logical<kServiceBuffers && !last){
        const UInt32 firstSlot=(s.readIndex+count)&(Ath10k::kHTTRingEntries-1);
        const UInt32 metadataSlot=(s.statusRead+metadataConsumed)&(Ath10k::kHTTRingEntries-1);
        const UInt32 id=s.rxMode==2?s.completionIds[metadataSlot]:s.bufferIds[firstSlot];
        if(id>=Ath10k::kHTTRxPosted)return "htt-rx-buffer-id";
        if(s.rxMode==2 && !s.addressPending[id])return "htt-paddr-not-pending";
        const UInt8 *data=bufferData(static_cast<UInt16>(id));
        if(!data)return "htt-rx-buffer-sync";
        if(!(Ath10k::wmiReadLE32(data+4)&0x80000000u)){
            ++s.rxCompletionNotDone;
            s.rxDoneLastBuffer=id;
            s.rxDoneLastAttention=Ath10k::wmiReadLE32(data+4);
            return "htt-rx-completion-not-ready";
        }
        UInt32 chain=data[10];
        // The reusable scratch covers the advertised 3839-byte A-MSDU.
        // Larger chains are still consumed and refilled, but are
        // counted as a recoverable drop instead of stopping the kext.
        const UInt32 availableMeta=s.rxMode==2?s.pendingMPDUs-metadataConsumed:0;
        if(s.rxMode==2 && 1u+chain>availableMeta){
            ++s.rxChainWaits;return "htt-rx-chain-not-ready";
        }
        if(count+1u+chain>kServiceBuffers){
            // Never clamp a chain: doing so reinterprets payload as a descriptor.
            return "htt-rx-chain-too-many-buffers";
        }
        const UInt32 fragments=1u+chain;
        statuses[logical]=s.mpduStatus[metadataSlot];
        peers[logical]=s.mpduPeer[metadataSlot];lengths[logical]=s.rxMode==2?s.completionLengths[metadataSlot]:0;
        chainCounts[logical]=static_cast<UInt8>(chain);
        for(UInt32 part=0;part<fragments;++part){
            const UInt32 slot=(s.readIndex+count)&(Ath10k::kHTTRingEntries-1);
            const UInt32 partMeta=(s.statusRead+metadataConsumed)&(Ath10k::kHTTRingEntries-1);
            const UInt32 partID=s.rxMode==2?s.completionIds[partMeta]:s.bufferIds[slot];
            if(partID>=Ath10k::kHTTRxPosted)return "htt-rx-buffer-id";
            if(s.rxMode==2 && !s.addressPending[partID])return "htt-paddr-not-pending";
            for(UInt32 n=0;n<count;++n)if(ids[n]==partID)return "htt-rx-duplicate-buffer";
            ids[count]=static_cast<UInt16>(partID);
            _rxServiceData[count]=part?nullptr:data;++count;
            if(s.rxMode==2)++metadataConsumed;
        }
        const UInt32 msduFlags=Ath10k::wmiReadLE32(data+56);
        const bool splitAMSDU=(data[236]&0x80) && (data[260]&0x80);
        if(!logical)splitMPDU=splitAMSDU || (msduFlags&0xc000u)==0x4000u;
        last=(msduFlags&0x8000u)!=0 || (s.rxMode==2 && !splitMPDU);
        ++logical;
        if(s.rxMode==2 && !last && metadataConsumed==s.pendingMPDUs){
            ++s.rxChainWaits;return "htt-rx-chain-not-ready";
        }
    }
    if(!last)return "htt-rx-missing-last-msdu";
    // Ordered RX_IND ranges count MPDUs, not MSDUs or payload buffers.
    // Every MSDU through LAST belongs to this single metadata entry.
    if(s.rxMode!=2)metadataConsumed=1;
    _rxBatch.reset();
    _rxBatch.fragment=s.completionFragment[s.statusRead];
    for(UInt32 n=0;n<logical;++n){
        const UInt32 fragments=1u+chainCounts[n];
        // Scratch lives on the IOService instance, not the kernel stack.
        if(fragments*Ath10k::kHTTRxBufferBytes>sizeof(_rxAssembly)){
            ++_connection.rxChainDrops;_rxBatch.failed=true;
            Ath10k::rxDrop(_connection,Ath10k::ConnectionState::Length);continue;}
        UInt32 base=0;for(UInt32 prior=0;prior<n;++prior)base+=1u+chainCounts[prior];
        const UInt8 *assembled=_rxServiceData[base];
        if(fragments==1)++_perf.rxDirect;
        else {
            ++_perf.rxAssembly;assembled=_rxAssembly;
            for(UInt32 part=0;part<fragments;++part){
                const UInt8 *data=bufferData(ids[base+part]);if(!data)return "htt-rx-buffer-sync";
                bcopy(data,_rxAssembly+part*Ath10k::kHTTRxBufferBytes,Ath10k::kHTTRxBufferBytes);
            }
        }
        // Peer, sequence and TID in MPDU start belong to the first MSDU;
        // continuations must not contribute fictitious BA sequences/peers.
        if(!n && statuses[n]==1){Ath10k::recordRxBlockAck(_connection,assembled);Ath10k::recordRXRate(_perf.rate,assembled,fragments*Ath10k::kHTTRxBufferBytes);}
        const UInt32 available=fragments*Ath10k::kHTTRxBufferBytes;
        deliverNetworkRX(assembled,statuses[n],peers[n],lengths[n],available);
        if(chainCounts[n])_connection.rxChainedBuffers+=chainCounts[n];
    }
    finishNetworkRX();
    // Original contiguous pool stays mapped and is never rewritten. Each buffer
    // migrates to a separate DMA mapping after its first explicit completion.
    // This avoids synchronizing CPU zeros over still-device-owned neighbours
    // when IODMACommand uses a bounce buffer for the original pool.
    for(UInt32 n=0;n<count;++n){
        auto &buffer=_httRecycled[ids[n]];
        if(!buffer.allocated() && !buffer.allocate(Ath10k::kHTTRxBufferBytes,128,_dmaMapper,Ath10kDMABuffer::Role::Payload))return "htt-refill-allocation";
        if(!Ath10k::dmaSpan32(buffer.dmaAddress(),Ath10k::kHTTRxBufferBytes))return "htt-refill-address";
        bzero(buffer.virtualAddress(),Ath10k::kHTTRxBufferBytes);
        if(!buffer.syncForDevice())return "htt-refill-buffer-sync";
    }
    auto *ring=static_cast<UInt8 *>(_httRxRing.virtualAddress());
    for(UInt32 n=0;n<count;++n){
        UInt32 slot=(s.writeIndex+n)&(Ath10k::kHTTRingEntries-1);
        Ath10k::htcWriteLE32(ring+4*slot,static_cast<UInt32>(_httRecycled[ids[n]].dmaAddress()));
        s.bufferIds[slot]=ids[n];
    }
    if(!_httRxRing.syncForDevice())return "htt-refill-ring-sync";
    OSSynchronizeIO();
    const UInt32 next=(s.writeIndex+count)&(Ath10k::kHTTRingEntries-1);
    Ath10k::htcWriteLE32(static_cast<UInt8 *>(_httRxIndex.virtualAddress()),next);
    if(!_httRxIndex.syncForDevice())return "htt-refill-index-sync";
    s.writeIndex=next;s.readIndex=(s.readIndex+count)&(Ath10k::kHTTRingEntries-1);
    s.statusRead=(s.statusRead+metadataConsumed)&(Ath10k::kHTTRingEntries-1);
    for(UInt32 n=0;n<count;++n){
        s.addresses[ids[n]]=static_cast<UInt32>(_httRecycled[ids[n]].dmaAddress());
        s.addressPending[ids[n]]=false;
    }
    s.recycled+=count;
    if(s.rxMode==2){if(s.pendingMPDUs<metadataConsumed)return "htt-paddr-pending-underflow";s.pendingMPDUs-=metadataConsumed;}
    else {if(!s.pendingMPDUs)return "htt-rx-pending-underflow";--s.pendingMPDUs;}
    if(_connection.stage!=Ath10k::LinkStage::Connected)++s.discardedMPDUs;
    return nullptr;
}

const char *Ath10kPCIDevice::sampleHTTRxPool()
{
    if(!_rxSetup.ringOwned)return nullptr;
    if(!_httRxPool.allocated() || !_httRxPool.syncFromDevice())return "htt-pool-read-sync";
    const auto *pool=static_cast<const UInt8 *>(_httRxPool.virtualAddress());
    UInt32 done=0;
    for(UInt32 i=0;i<Ath10k::kHTTRxPosted;++i){
        const UInt8 *data=pool+UInt64(i)*Ath10k::kHTTRxBufferBytes;
        if(_httRecycled[i].allocated()){
            if(!_httRecycled[i].syncFromDevice())return "htt-pool-buffer-sync";
            data=static_cast<const UInt8 *>(_httRecycled[i].virtualAddress());
        }
        if(Ath10k::wmiReadLE32(data+4)&0x80000000u)++done;
    }
    _rxSetup.rxDone=done;
    // MSDU_DONE without RX_IND never authorizes reuse. Detect total exhaustion
    // when there is no outstanding completion to process.
    return done==Ath10k::kHTTRxPosted && !_rxSetup.pendingMPDUs?"htt-diagnostic-pool-exhausted":nullptr;
}

const char *Ath10kPCIDevice::serviceWMIVdevCreate(UInt64 nowMs)
{
    auto &state=_wmiVdevCreate;
    if (state.completed) return nullptr;
    if (!state.attempted) {
        state.attempted=true;state.startedMs=nowMs;state.status="vdev-create-prerequisite";
        if (!_wmiLive.active || !_runtimeActive || !_rxSetup.completed || !_wmiEcho.completed || !_wmiEcho.txConsumed ||
            !_wmiInit.ready || !_wmi.rxOwned || !_runtimeCE1Armed || _dmaQuarantine ||
            !_htcWMIControl.connected || _htcWMIControl.uplinkPipe!=3 ||
            _htcWMIControl.maxMessageSize<28 || _wmi.credits.outstanding ||
            !_wmiTxBuffer.allocated() || !_ceSrcRings[3].allocated()) return state.status;
        Ath10k::CESnapshot snap{};
        const UInt32 entries=Ath10k::kCEHostConfig[3].srcEntries;
        state.status="vdev-create-ce3-state";
        if (!Ath10k::readCESnapshot(_regs,3,snap) || !validateBMIPipeStructure(3,snap) ||
            snap.srcWriteIndex>=entries || snap.currentSrcRead!=snap.srcWriteIndex ||
            snap.srcWriteIndex!=_rxSetup.wmiNext || !_ceSrcRings[3].syncFromDevice()) return state.status;
        state.status="vdev-create-build";
        if (!Ath10k::buildWMIVdevCreate(static_cast<UInt8 *>(_wmiTxBuffer.virtualAddress()),
            static_cast<UInt32>(_wmiTxBuffer.length()),_htcWMIControl.endpoint,_wmiInit.mac)) return state.status;
        state.txSlot=snap.srcWriteIndex;state.txNext=Ath10k::ceRingNext(state.txSlot,entries);
        auto *ring=static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[3].virtualAddress());
        auto *desc=&ring[state.txSlot];bzero(desc,sizeof(*desc));
        desc->address=static_cast<UInt32>(_wmiTxBuffer.dmaAddress());
        desc->nbytes=Ath10k::kWMIVdevCreateFrameBytes;
        desc->flags=static_cast<UInt16>(_htcWMIControl.endpoint<<2);
        OSSynchronizeIO();state.status="vdev-create-tx-sync";
        if (!_wmiTxBuffer.syncForDevice() || !_ceSrcRings[3].syncForDevice()) return state.status;
        state.status="vdev-create-no-credit";
        if (!_wmi.credits.reserve(Ath10k::kWMIVdevCreateFrameBytes)) return state.status;
        state.txOwned=true;state.status="vdev-create-submit-failed";
        UInt32 back=0xffffffffu;
        const UInt32 doorbell=Ath10k::ceBaseAddress(3)+Ath10k::kCESrcWriteIndex;
        if (!_regs.write32(doorbell,state.txNext) || !_regs.read32(doorbell,back) || back!=state.txNext)
            return state.status;
        state.submitted=true;state.status="waiting-for-tx-and-credit";
        ATH10K_TRACE("Ath10kMac: WMI VDEV_CREATE SUBMITTED id=0 type=STA sequence=5 bytes=36 credits=%u outstanding=%u\n",
              _wmi.credits.available,_wmi.credits.outstanding);
    }
    if (!state.submitted) return state.status; // No retry after any failed attempt.
    state.elapsedMs=nowMs-state.startedMs;
    UInt32 read=0xffffffffu;
    if (!_regs.read32(Ath10k::ceBaseAddress(3)+Ath10k::kCECurrentSrcRead,read) ||
        (read!=state.txSlot && read!=state.txNext)) {state.status="vdev-create-tx-index";return state.status;}
    state.txConsumed=read==state.txNext;
    state.creditReturned=_wmi.credits.outstanding==0;
    if (state.elapsedMs>=5000) {
        state.status=!state.txConsumed?"vdev-create-tx-timeout":
            (!state.creditReturned?"vdev-create-credit-timeout":"vdev-create-deadline");
        return state.status;
    }
    if (state.txConsumed && state.creditReturned) {
        state.completed=true;state.status="transport-complete-no-create-response";
        // VDEV_CREATE has no semantic success response in this ath10k TLV path.
        ATH10K_TRACE("Ath10kMac: WMI VDEV_CREATE TRANSPORT PASS id=0 tx-consumed=yes credit-returned=yes elapsed=%llu ms; semantic creation not confirmed\n",
              static_cast<unsigned long long>(state.elapsedMs));
    }
    return nullptr;
}

const char *Ath10kPCIDevice::serviceWMIRadio(UInt64 nowMs, UInt32 index)
{
    if (index>=Ath10k::kWMIRadioStepCount) return "radio-index";
    auto &state=_wmiRadio[index];
    const bool previousDone=index?_wmiRadio[index-1].completed:_wmiVdevCreate.completed;
    const bool previousConsumed=index?_wmiRadio[index-1].txConsumed:_wmiVdevCreate.txConsumed;
    const UInt32 previousNext=index?_wmiRadio[index-1].txNext:_wmiVdevCreate.txNext;
    if (state.completed) return nullptr;
    if (!state.attempted) {
        if(index==2){
            if(!_wmi.regCapsPresent)return "regulatory-capabilities-missing";
            const char *e=Ath10k::configureRegulatory(_regulatory,_wmi.eepromRD,_wmi.low2,_wmi.high2,_wmi.low5,_wmi.high5);
            if(e){_regulatory.status=e;return e;}
        }
        const UInt32 frameBytes=index==2?Ath10k::kWMIRegdomainFrameBytes:Ath10k::kWMIRadioFrameBytes;

        state.attempted=true;state.startedMs=nowMs;state.status="radio-prerequisite";
        if (!_wmiLive.active || !_runtimeActive || !previousDone || !previousConsumed ||
            !_wmiInit.ready || !_wmi.rxOwned || !_runtimeCE1Armed || _dmaQuarantine ||
            !_htcWMIControl.connected || _htcWMIControl.uplinkPipe!=3 ||
            _htcWMIControl.maxMessageSize<Ath10k::kWMIRegdomainFrameBytes-8 || _wmi.credits.outstanding ||
            !_wmiTxBuffer.allocated() || !_ceSrcRings[3].allocated()) return state.status;
        Ath10k::CESnapshot snap{};
        const UInt32 entries=Ath10k::kCEHostConfig[3].srcEntries;
        state.status="radio-ce3-state";
        if (!Ath10k::readCESnapshot(_regs,3,snap) || !validateBMIPipeStructure(3,snap) ||
            snap.srcWriteIndex>=entries || snap.currentSrcRead!=snap.srcWriteIndex ||
            snap.srcWriteIndex!=previousNext || !_ceSrcRings[3].syncFromDevice()) return state.status;
        state.status="radio-build";
        UInt8 *frame=static_cast<UInt8 *>(_wmiTxBuffer.virtualAddress());
        const UInt32 param=index==Ath10k::kWMIPMFQoSIndex?Ath10k::kWMIPMFQoSParam:(index<2?index+1:index);
        const UInt32 value=index==Ath10k::kWMIPMFQoSIndex?1:(index<2?_wmiInit.requestedChainMask:_regulatory.ceilingHalfDbm);
        const bool built=index==2?Ath10k::buildWMIRegdomain(frame,static_cast<UInt32>(_wmiTxBuffer.length()),_htcWMIControl.endpoint,_regulatory):
            Ath10k::buildWMIRadio(frame,static_cast<UInt32>(_wmiTxBuffer.length()),_htcWMIControl.endpoint,param,value);
        if(!built)return state.status;
        frame[5]=static_cast<UInt8>(index+6);
        state.txSlot=snap.srcWriteIndex;state.txNext=Ath10k::ceRingNext(state.txSlot,entries);
        auto *ring=static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[3].virtualAddress());
        auto *desc=&ring[state.txSlot];bzero(desc,sizeof(*desc));
        desc->address=static_cast<UInt32>(_wmiTxBuffer.dmaAddress());
        desc->nbytes=static_cast<UInt16>(frameBytes);
        desc->flags=static_cast<UInt16>(_htcWMIControl.endpoint<<2);
        OSSynchronizeIO();state.status="radio-tx-sync";
        if (!_wmiTxBuffer.syncForDevice() || !_ceSrcRings[3].syncForDevice()) return state.status;
        state.status="radio-no-credit";
        if (!_wmi.credits.reserve(frameBytes)) return state.status;
        state.txOwned=true;state.status="radio-submit-failed";
        UInt32 back=0xffffffffu;
        const UInt32 doorbell=Ath10k::ceBaseAddress(3)+Ath10k::kCESrcWriteIndex;
        if (!_regs.write32(doorbell,state.txNext) || !_regs.read32(doorbell,back) || back!=state.txNext)
            return state.status;
        state.submitted=true;state.status="waiting-for-tx-and-credit";
        ATH10K_TRACE("Ath10kMac: WMI RADIO SUBMITTED step=%u command=0x%x param=%u value=%u sequence=%u bytes=%u\n",
              index,index==2?0x4001:0x4003,param,value,index+6,frameBytes);
    }
    if (!state.submitted) return state.status; // No retry after any failed attempt.
    state.elapsedMs=nowMs-state.startedMs;
    UInt32 read=0xffffffffu;
    if (!_regs.read32(Ath10k::ceBaseAddress(3)+Ath10k::kCECurrentSrcRead,read) ||
        (read!=state.txSlot && read!=state.txNext)) {state.status="radio-tx-index";return state.status;}
    state.txConsumed=read==state.txNext;
    state.creditReturned=_wmi.credits.outstanding==0;
    if (state.elapsedMs>=5000) {
        state.status=!state.txConsumed?"radio-tx-timeout":
            (!state.creditReturned?"radio-credit-timeout":"radio-deadline");
        return state.status;
    }
    if (state.txConsumed && state.creditReturned) {
        state.completed=true;state.status="transport-complete-rf-unconfirmed";
        // Credits prove transport completion, not application by the RF hardware.
        ATH10K_TRACE("Ath10kMac: WMI RADIO TRANSPORT PASS step=%u elapsed=%llu ms; RF application unconfirmed\n",
              index,static_cast<unsigned long long>(state.elapsedMs));
    }
    return nullptr;
}

const char *Ath10kPCIDevice::serviceWMIScanChannels(UInt64 nowMs)
{
    auto &state=_wmiScanChannels;
    if (state.completed) return nullptr;
    if (!state.attempted) {
        state.attempted=true;state.startedMs=nowMs;state.status="scan-channels-prerequisite";
        if (!_wmiLive.active || !_runtimeActive || !_wmiRadio[0].completed || !_wmiRadio[1].completed || !_wmiRadio[Ath10k::kWMIPMFQoSIndex].completed || !_wmiRadio[Ath10k::kWMIPMFQoSIndex].txConsumed ||
            !_wmiInit.ready || !_wmi.rxOwned || !_runtimeCE1Armed || _dmaQuarantine ||
            !_htcWMIControl.connected || _htcWMIControl.uplinkPipe!=3 ||
            _htcWMIControl.maxMessageSize<Ath10k::kWMIScanChannelsFrameBytes-8 || _wmi.credits.outstanding ||
            !_wmiTxBuffer.allocated() || !_ceSrcRings[3].allocated()) return state.status;
        Ath10k::CESnapshot snap{};
        const UInt32 entries=Ath10k::kCEHostConfig[3].srcEntries;
        state.status="scan-channels-ce3-state";
        if (!Ath10k::readCESnapshot(_regs,3,snap) || !validateBMIPipeStructure(3,snap) ||
            snap.srcWriteIndex>=entries || snap.currentSrcRead!=snap.srcWriteIndex ||
            snap.srcWriteIndex!=_wmiRadio[Ath10k::kWMIPMFQoSIndex].txNext || !_ceSrcRings[3].syncFromDevice()) return state.status;
        state.status="scan-channels-build";
        if (!Ath10k::buildWMIScanChannels(static_cast<UInt8 *>(_wmiTxBuffer.virtualAddress()),
            static_cast<UInt32>(_wmiTxBuffer.length()),_htcWMIControl.endpoint,&_regulatory)) return state.status;
        const UInt32 frameBytes=8+Ath10k::htcReadLE16(static_cast<const UInt8 *>(_wmiTxBuffer.virtualAddress())+2);
        state.txSlot=snap.srcWriteIndex;state.txNext=Ath10k::ceRingNext(state.txSlot,entries);
        auto *ring=static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[3].virtualAddress());
        auto *desc=&ring[state.txSlot];bzero(desc,sizeof(*desc));
        desc->address=static_cast<UInt32>(_wmiTxBuffer.dmaAddress());
        desc->nbytes=static_cast<UInt16>(frameBytes);
        desc->flags=static_cast<UInt16>(_htcWMIControl.endpoint<<2);
        OSSynchronizeIO();state.status="scan-channels-tx-sync";
        if (!_wmiTxBuffer.syncForDevice() || !_ceSrcRings[3].syncForDevice()) return state.status;
        state.status="scan-channels-no-credit";
        if (!_wmi.credits.reserve(frameBytes)) return state.status;
        state.txOwned=true;state.status="scan-channels-submit-failed";
        UInt32 back=0xffffffffu;
        const UInt32 doorbell=Ath10k::ceBaseAddress(3)+Ath10k::kCESrcWriteIndex;
        if (!_regs.write32(doorbell,state.txNext) || !_regs.read32(doorbell,back) || back!=state.txNext)
            return state.status;
        state.submitted=true;state.status="waiting-for-tx-and-credit";
        ATH10K_TRACE("Ath10kMac: WMI SCAN_CHAN_LIST SUBMITTED channels=%u bands=2.4+5GHz passive=yes sequence=8 bytes=%u credits=%u outstanding=%u\n",
              Ath10k::regulatoryScanCount(&_regulatory),Ath10k::htcReadLE16(static_cast<UInt8 *>(_wmiTxBuffer.virtualAddress())+2)+8,_wmi.credits.available,_wmi.credits.outstanding);
    }
    if (!state.submitted) return state.status; // No retry after any failed attempt.
    state.elapsedMs=nowMs-state.startedMs;
    UInt32 read=0xffffffffu;
    if (!_regs.read32(Ath10k::ceBaseAddress(3)+Ath10k::kCECurrentSrcRead,read) ||
        (read!=state.txSlot && read!=state.txNext)) {state.status="scan-channels-tx-index";return state.status;}
    state.txConsumed=read==state.txNext;
    state.creditReturned=_wmi.credits.outstanding==0;
    if (state.elapsedMs>=5000) {
        state.status=!state.txConsumed?"scan-channels-tx-timeout":
            (!state.creditReturned?"scan-channels-credit-timeout":"scan-channels-deadline");
        return state.status;
    }
    if (state.txConsumed && state.creditReturned) {
        state.completed=true;state.status="transport-complete-scan-not-started";
        // Transport completion does not prove channel acceptance or scan success.
        ATH10K_TRACE("Ath10kMac: WMI SCAN_CHAN_LIST TRANSPORT PASS channels=%u bands=2.4+5GHz passive=yes tx-consumed=yes credit-returned=yes elapsed=%llu ms; scan not started; channel acceptance not confirmed\n",
              Ath10k::regulatoryScanCount(&_regulatory),static_cast<unsigned long long>(state.elapsedMs));
    }
    return nullptr;
}

const char *Ath10kPCIDevice::serviceWMIScan(UInt64 nowMs)
{
    auto &state=_wmiScan;
    if (state.completed) return nullptr;
    // CSA may arrive after native scan admission. Cancel before acquiring
    // firmware ownership; an already submitted scan follows STOP_SCAN below.
    if(_connection.csa.phase!=Ath10k::ChannelSwitchPhase::Idle || _connection.pmfTXPending)state.cancelRequested=true;
    // An accepted request cancelled before submission owns no DMA or firmware
    // scan. Finish it locally instead of starting a scan merely to stop it.
    if(!state.attempted && state.cancelRequested){
        state.completed=true;state.terminal=true;state.reason=1;
        state.status="scan-cancelled-before-start";return nullptr;
    }
    if (!state.attempted && (_connection.stats.step || _connection.maintenance.step || _connection.radio.step || _connection.radio.appliedPS))return nullptr;
    if (!state.attempted) {
        state.attempted=true;state.startedMs=nowMs;state.status="scan-prerequisite";
        if (!_wmiLive.active || !_runtimeActive || !_wmiScanChannels.completed || !_wmiScanChannels.txConsumed ||
            !_wmiInit.ready || !_wmi.rxOwned || !_runtimeCE1Armed || _dmaQuarantine ||
            !_htcWMIControl.connected || _htcWMIControl.uplinkPipe!=3 ||
            _htcWMIControl.maxMessageSize<Ath10k::kWMIScanFrameBytes-8 || _wmi.credits.outstanding ||
            !_wmiTxBuffer.allocated() || !_ceSrcRings[3].allocated()) return state.status;
        Ath10k::CESnapshot snap{};
        const UInt32 entries=Ath10k::kCEHostConfig[3].srcEntries;
        state.status="scan-ce3-state";
        if (!Ath10k::readCESnapshot(_regs,3,snap) || !validateBMIPipeStructure(3,snap) ||
            snap.srcWriteIndex>=entries || snap.currentSrcRead!=snap.srcWriteIndex ||
            (!state.generation && snap.srcWriteIndex!=_wmiScanChannels.txNext) || !_ceSrcRings[3].syncFromDevice()) return state.status;
        state.status="scan-build";
        if (!Ath10k::buildWMIScan(static_cast<UInt8 *>(_wmiTxBuffer.virtualAddress()),
            static_cast<UInt32>(_wmiTxBuffer.length()),_htcWMIControl.endpoint,state.id,_connection.stage==Ath10k::LinkStage::Connected,&_regulatory,
#ifdef ATH10K_NATIVE
            _nativeScanDwellMs,
            Ath10k::wmiRequestedScanMask(_nativeScanPending && _nativeScan.channelCount?_nativeScan.channels:nullptr,sizeof(_nativeScan.channels))
#else
            Ath10k::kWMIScanDwellMs
#endif
            )) return state.status;
        const UInt32 frameBytes=8+Ath10k::htcReadLE16(static_cast<const UInt8 *>(_wmiTxBuffer.virtualAddress())+2);
        state.txSlot=snap.srcWriteIndex;state.txNext=Ath10k::ceRingNext(state.txSlot,entries);
        auto *ring=static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[3].virtualAddress());
        auto *desc=&ring[state.txSlot];bzero(desc,sizeof(*desc));
        desc->address=static_cast<UInt32>(_wmiTxBuffer.dmaAddress());
        desc->nbytes=static_cast<UInt16>(frameBytes);
        desc->flags=static_cast<UInt16>(_htcWMIControl.endpoint<<2);
        OSSynchronizeIO();state.status="scan-tx-sync";
        if (!_wmiTxBuffer.syncForDevice() || !_ceSrcRings[3].syncForDevice()) return state.status;
        state.status="scan-no-credit";
        if (!_wmi.credits.reserve(frameBytes)) return state.status;
        state.txOwned=true;state.status="scan-submit-failed";
        UInt32 back=0xffffffffu;
        const UInt32 doorbell=Ath10k::ceBaseAddress(3)+Ath10k::kCESrcWriteIndex;
        if (!_regs.write32(doorbell,state.txNext) || !_regs.read32(doorbell,back) || back!=state.txNext)
            return state.status;
        state.submitted=true;state.status="waiting-for-scan-events";
        ATH10K_TRACE("Ath10kMac: WMI START_SCAN SUBMITTED id=0x%x request=0xa001 vdev=0 channels=%u passive=yes sequence=9 bytes=%u credits=%u outstanding=%u\n",
              state.id,Ath10k::regulatoryScanCount(&_regulatory),Ath10k::htcReadLE16(static_cast<UInt8 *>(_wmiTxBuffer.virtualAddress())+2)+8,_wmi.credits.available,_wmi.credits.outstanding);
    }
    if (!state.submitted) return state.status; // No retry after any failed attempt.
    state.elapsedMs=nowMs-state.startedMs;
    UInt32 read=0xffffffffu;
    if(!state.txConsumed){
        if (!_regs.read32(Ath10k::ceBaseAddress(3)+Ath10k::kCECurrentSrcRead,read) ||
            (read!=state.txSlot && read!=state.txNext)) {state.status="scan-tx-index";return state.status;}
        state.txConsumed=read==state.txNext;
    }
    state.creditReturned=_wmi.credits.outstanding==0;
    if(state.txConsumed && state.creditReturned && !state.stopSubmitted)state.txOwned=false;
    if(state.cancelRequested && state.txConsumed && state.creditReturned && !state.terminal){
        if(!state.stopSubmitted){
            Ath10k::CESnapshot snap{};const UInt32 entries=Ath10k::kCEHostConfig[3].srcEntries;
            if(_connection.wmiOwned || !Ath10k::readCESnapshot(_regs,3,snap) || snap.currentSrcRead!=snap.srcWriteIndex)return nullptr;
            if(!Ath10k::buildWMIStopScan(static_cast<UInt8 *>(_wmiTxBuffer.virtualAddress()),static_cast<UInt32>(_wmiTxBuffer.length()),_htcWMIControl.endpoint,state.id,static_cast<UInt8>(_connection.wmiSequence++)))return "scan-stop-build";
            state.stopSlot=snap.srcWriteIndex;state.stopNext=Ath10k::ceRingNext(snap.srcWriteIndex,entries);
            auto *ring=static_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[3].virtualAddress());auto &desc=ring[snap.srcWriteIndex];
            bzero(&desc,sizeof(desc));desc.address=static_cast<UInt32>(_wmiTxBuffer.dmaAddress());desc.nbytes=32;desc.flags=static_cast<UInt16>(_htcWMIControl.endpoint<<2);
            if(!_wmiTxBuffer.syncForDevice() || !_ceSrcRings[3].syncForDevice() || !_wmi.credits.reserve(32))return "scan-stop-sync";
            state.txOwned=true;UInt32 back=0;const UInt32 doorbell=Ath10k::ceBaseAddress(3)+Ath10k::kCESrcWriteIndex;
            if(!_regs.write32(doorbell,state.stopNext) || !_regs.read32(doorbell,back) || back!=state.stopNext)return "scan-stop-doorbell";
            state.stopSubmitted=true;return nullptr;
        }
    }
    if(state.stopSubmitted && !state.stopConsumed){
        if(!_regs.read32(Ath10k::ceBaseAddress(3)+Ath10k::kCECurrentSrcRead,read))return "scan-stop-index";
        state.stopConsumed=read==state.stopNext;
        if(state.stopConsumed && !_wmi.credits.outstanding)state.txOwned=false;
    }
    if (state.failed) {state.status="scan-firmware-failed";return state.status;}
    if (state.elapsedMs>=Ath10k::kWMIScanHostMs) {
        state.status=!state.txConsumed?"scan-tx-timeout":
            (!state.creditReturned?"scan-credit-timeout":(!state.started?"scan-start-timeout":"scan-completion-timeout"));
        return state.status;
    }
    if (state.txConsumed && state.creditReturned && (state.started || state.cancelRequested) && state.terminal && (!state.stopSubmitted || state.stopConsumed)) {
        state.completed=true;state.status="scan-completed";
        // Success requires correlated STARTED and COMPLETED(reason=0), plus TX/credit.
        ATH10K_TRACE("Ath10kMac: WMI START_SCAN PASS id=0x%x channels=%u started=%s completed=yes reason=%u tx-consumed=yes credit-returned=yes elapsed=%llu ms\n",
              state.id,Ath10k::regulatoryScanCount(&_regulatory),state.started?"yes":"no",state.reason,static_cast<unsigned long long>(state.elapsedMs));
    }
    return nullptr;
}

const char *Ath10kPCIDevice::drainWMIRuntime()
{
    if (!_wmiLive.active || !_runtimeActive || _dmaQuarantine)
        return "runtime-not-active";
    ++_wmiLive.polls;
    if (!_regs.read32(Ath10k::kQca6174RegisterLayout.fwIndicatorAddress,_boot.fwIndicator) ||
        _boot.fwIndicator==0xffffffffu) return "runtime-mmio-failed";
    if (_boot.fwIndicator&Ath10k::kFwIndicatorEventPending) {
        _boot.firmwareCrash=true;return "runtime-firmware-crash";
    }
    UInt32 consumed=0;
    // Two CEs, one reusable DMA buffer each. Fair bounded drain under the gate.
    while (consumed<32) {
        bool progress=false;
        for (UInt32 ce=1;ce<=2 && consumed<32;++ce) {
            const UInt32 slot=ce==1?_runtimeRxSlot:_wmi.slot;
            auto &buffer=ce==1?_htcReadyBuffer:_wmiRxBuffer;
            if (!buffer.allocated() || !_ceDstRings[ce].allocated() ||
                slot>=Ath10k::kCEHostConfig[ce].dstEntries ||
                !_ceDstRings[ce].syncFromDevice()) return "runtime-rx-ring";
            auto *ring=reinterpret_cast<Ath10k::CEDescriptor32 *>(_ceDstRings[ce].virtualAddress());
            const UInt32 bytes=ring[slot].nbytes;
            if (!bytes) continue;
            _wmiLive.lastBytes=bytes;
            if (ring[slot].address!=static_cast<UInt32>(buffer.dmaAddress()) ||
                bytes<8 || bytes>buffer.length() || !buffer.syncFromDevice())
                return "runtime-rx-descriptor";
            const auto *raw=reinterpret_cast<const UInt8 *>(buffer.virtualAddress());
            const UInt32 payload=Ath10k::htcReadLE16(raw+2);
            const UInt32 trailer=(raw[1]&Ath10k::kHTCTrailerPresent)?raw[4]:0;
            _wmiLive.lastEventId=bytes>=12 && payload>=trailer+4 && raw[0]==_htcWMIControl.endpoint
                ? Ath10k::wmiReadLE32(raw+8)&0xffffffu : 0;
            ++_wmiLive.rawCount;
            if (ce==1) ++_wmiLive.rawCE1; else ++_wmiLive.rawCE2;
            if (_wmiLive.lastEventId==0x7001) ++_wmiLive.rawMgmt;
            // Bound diagnostic volume; log headers only, never SSID/packet contents.
            if (_wmiLive.rawCount<=64)
                ATH10K_TRACE("Ath10kMac: WMI RX ENVELOPE CE%u endpoint=%u id=0x%x bytes=%u payload=%u trailer=%u\n",
                      ce,raw[0],_wmiLive.lastEventId,bytes,payload,trailer);
            // INIT has already completed; a second READY is not a new success.
            if (_wmiLive.lastEventId==2) return "runtime-unexpected-ready";
            const bool isHTT=_htcHTTData.connected && raw[0]==_htcHTTData.endpoint;
            Ath10k::WMIManagementConsumer managementConsumer{};
#ifdef ATH10K_NATIVE
            managementConsumer.context=this;managementConsumer.receive=nativePMFManagement;
#endif
            const char *error=isHTT?
                Ath10k::parseHTTSetup(raw,bytes,_htcHTTData.endpoint,_wmi.credits,_rxSetup,&_connection):
                Ath10k::parseWMIInitResponse(raw,bytes,_htcWMIControl.endpoint,_wmi.credits,_wmiEvents,
#ifdef ATH10K_NATIVE
                    _nativeMACActive?&_nativeMACEcho:
#endif
                    (_connection.stats.step==2 || _connection.maintenance.step==8 || _connection.radio.step==8 || _connection.csa.phase==Ath10k::ChannelSwitchPhase::Barrier || _connection.disconnect==Ath10k::DisconnectStage::Barrier || _connection.stage==Ath10k::LinkStage::Barrier || (_connection.secure && _connection.securityPhase==4))?&_connectionEcho:((_rxSetup.attempted && _rxSetup.stage==2)?&_filterEcho:&_wmiEcho),&_wmiScan,&_wmiBSS,
                    _rxSetup.attempted && (_rxSetup.stage>1 || (_rxSetup.stage==1 && _rxSetup.submitted)),&_connection,&managementConsumer);
            const bool httMessage=isHTT && bytes>=12 && payload>=trailer+4;
            if(httMessage && !error && _rxSetup.messages<=16)
                ATH10K_TRACE("Ath10kMac: HTT RX type=0x%x bytes=%u version=%u.%u\n",_rxSetup.lastType,bytes,_rxSetup.major,_rxSetup.minor);
            if(httMessage && !error && (raw[8]==3 || raw[8]==4) &&
               _connection.diagnostics.maps+_connection.diagnostics.unmaps<=128){
                const auto &d=_connection.diagnostics;
                IOLog("Ath10kMac: PEER %s id=%u vdev=%u mac=%02x:%02x:%02x:%02x:%02x:%02x own=%s count=%u->%u stage=%u security=%u disconnect=%u first-error=%s last-cmd=0x%x\n",
                    raw[8]==3?"MAP":"UNMAP",d.peer,d.vdev,d.peerMAC[0],d.peerMAC[1],d.peerMAC[2],d.peerMAC[3],d.peerMAC[4],d.peerMAC[5],
                    d.own?"yes":"no",d.peerBefore,d.peerAfter,static_cast<UInt32>(_connection.stage),_connection.securityPhase,
                    static_cast<UInt32>(_connection.disconnect),_connection.error,d.lastCommand);
            }
            if(httMessage && !error && raw[8]==0xb && _connection.securityInd.received<=16){
                const auto &d=_connection.securityInd;
                ATH10K_TRACE("Ath10kMac: SECURITY_IND peer=%u flags=0x%02x disconnect=%u mapped=%s retired=%s ack=%s action=%s\n",
                      d.peer,d.flags,d.disconnect,d.mapped?"yes":"no",d.retired?"yes":"no",d.ack?"yes":"no",d.action);
            }
            if (error) {
                ++_wmiLive.rejected;
                const char hex[]="0123456789abcdef";char prefix[129]{};
                const UInt32 prefixBytes=isHTT && bytes>8 && raw[8]==0xb?12:64;
                for (UInt32 i=0;i<bytes && i<prefixBytes;++i) {prefix[2*i]=hex[raw[i]>>4];prefix[2*i+1]=hex[raw[i]&15];}
                IOLog("Ath10kMac: WMI RUNTIME RX REJECT CE%u id=0x%x bytes=%u prefix=%s error=%s\n",
                      ce,_wmiLive.lastEventId,bytes,prefix,error);
                return error;
            }
            ++_wmiLive.rxCount;++consumed;progress=true;
            if (!isHTT && _wmiEvents.lastEventId==0x3001)
                ATH10K_TRACE("Ath10kMac: WMI SCAN EVENT type=0x%x reason=%u freq=%u count=%u\n",
                      _wmiScan.lastType,_wmiScan.reason,_wmiScan.frequency,_wmiScan.eventCount);
            if (!isHTT && _wmiEvents.lastEventId==0x1d019)
                ATH10K_TRACE("Ath10kMac: WMI RUNTIME PKGID count=%u value=0x%08x\n",
                      _wmiEvents.pkgIdCount,_wmiEvents.pkgId);
            if (!isHTT && _wmiEvents.lastEventId==0x16006)
                ATH10K_TRACE("Ath10kMac: WMI RUNTIME DIAG SUPPORTED count=%u entries=%u\n",
                      _wmiEvents.diagSupportedCount,_wmiEvents.diagSupportedEntries);
            if (ce==1) {
                ++_runtimeRxCompletedCount;_runtimeLastRxBytes=bytes;
                if (!htcRepostRuntimeReceive()) return "runtime-ce1-repost";
            } else if (!repostWMIBuffer()) return "runtime-ce2-repost";
        }
        if (!progress) break;
    }
    if (consumed==32) ++_wmiLive.budgetHits;
    return nullptr;
}

bool Ath10kPCIDevice::preparePCIeBringupParity()
{
    _pcieParityAttempted = true;
    _pcieParityPrepared = false;
    _pcieParityRestored = false;

    ATH10K_TRACE("Ath10kMac: PCIe bring-up parity begin (ASPM off + IOPCIFamily IRQ registration)\n");

    if (!disableASPMForBringup()) {
        IOLog("Ath10kMac: PCIe bring-up parity stopped: ASPM disable/readback failed\n");
        return false;
    }

    if (!prepareIRQParity()) {
        restoreASPM();
        IOLog("Ath10kMac: PCIe bring-up parity stopped: IRQ preparation failed\n");
        return false;
    }

    _pcieParityPrepared = true;
    ATH10K_TRACE("Ath10kMac: PCIe bring-up parity READY ASPM=off IRQ=%s\n",
          _irqMessaged?"MSI-registered-disabled":"INTx-polled-bootstrap");
    return true;
}

void Ath10kPCIDevice::restorePCIeBringupParity()
{
    teardownIRQParity();
    restoreASPM();
    if (_pcieParityAttempted) {
        _pcieParityRestored = (!_irqRegistered && !_intxConfigChanged &&
                               (!_aspmDisableAttempted || _aspmRestoreSucceeded));
    }
}

void Ath10kPCIDevice::validateRegisterLayer()
{
    ATH10K_TRACE("Ath10kMac: MMIO layer attached=%s length=0x%llx\n",
          _regs.attached() ? "yes" : "no",
          (unsigned long long)_regs.length());
    ATH10K_TRACE("Ath10kMac: MMIO policy=QCA6174 Host Interest + board/OTP/WLAN + BMI_DONE + HTC READY + Phase 3 HTC services in 0.0.91\n");

    if (!_regs.valid32(0))
        IOLog("Ath10kMac: ERROR BAR0 cannot satisfy even a 32-bit MMIO access\n");
}

void Ath10kPCIDevice::dumpHardwareCandidates()
{
    ATH10K_TRACE("Ath10kMac: hardware candidates for PCI device %04x:\n", _deviceId);
    UInt32 count = 0;
    for (const auto &candidate : Ath10k::kCandidates) {
        if (candidate.deviceId != _deviceId)
            continue;
        ++count;
        ATH10K_TRACE("Ath10kMac:   candidate=%s chip-rev=%u firmware=%s cal=%u cpu=%u diag-ce=%s\n",
              candidate.targetRevision,
              candidate.chipRevision,
              candidate.firmwareDirectory,
              candidate.calibrationDataLength,
              candidate.targetCpuFrequencyHz,
              candidate.firmwareDiagCeDownload ? "yes" : "no");
    }
    ATH10K_TRACE("Ath10kMac: %u candidate target profile(s); final selection remains deferred until post-reset chip-id\n", count);
}


void Ath10kPCIDevice::clearTargetProbeState()
{
    _pll={};
    _runtimeAttempted = false;
    _runtimeActive = false;
    _runtimeCE1Armed = false;
    _runtimeInterruptsEnabled = false;
    _runtimeWakeAsserted = false;
    _runtimeRxSlot = 0xffffffffu;
    _runtimeRxNext = 0xffffffffu;
    _runtimeRxPostedCount = 0;
    _runtimeRxCompletedCount = 0;
    _runtimeRxReplenishedCount = 0;
    _runtimeLastCE1Read = 0xffffffffu;
    _runtimeLastCE1Write = 0xffffffffu;
    _runtimeLastRxBytes = 0;
    _runtimeLastHostIE = 0xffffffffu;
    _runtimeLastMiscIE = 0xffffffffu;
    _runtimeLastPCIeIntrEnable = 0xffffffffu;
    _runtimeLastCoreCtrl = 0xffffffffu;
    _runtimeStatus = "not-run";

    _htcAttempted = false;
    _htcSetupCompleteSent = false;
    _htcReservedControlConnected = false;
    _htcServiceSetupSucceeded = false;
    _htcControlExchangeUncertain = false;
    _htcCreditFlowDeferred = true;
    _htcControlTxCount = 0;
    _htcControlRxCount = 0;
    _htcControlLastTxBytes = 0;
    _htcControlLastRxBytes = 0;
    _htcControlLastTxSlot = 0xffffffffu;
    _htcControlLastTxNext = 0xffffffffu;
    _htcControlLastRxSlot = 0xffffffffu;
    _htcControlLastRxNext = 0xffffffffu;
    _htcControlLastWaitMs = 0;
    _htcStatus = "not-run";
    _wmi = Ath10k::WMIBootstrapState{};
    _wmiInit = Ath10k::WMIInitState{};
    _htcReservedControl = Ath10k::HTCServiceInfo{};
    _htcWMIControl = Ath10k::HTCServiceInfo{};
    _htcHTTData = Ath10k::HTCServiceInfo{};

    _targetProbeAttempted = false;
    _targetWakeSucceeded = false;
    _postResetWakeSucceeded = false;
    _targetWasAlreadyAwake = false;
    _chipIdValid = false;
    _rtcStateBefore = 0xffffffffu;
    _rtcStateAfter = 0xffffffffu;
    _wakeDelayUs = 0;
    _chipId = 0xffffffffu;
    _chipRevision = 0xffu;
    _selectedHw = nullptr;

    _coldResetAttempted = false;
    _coldResetSucceeded = false;
    _socGlobalResetBefore = 0xffffffffu;
    _socGlobalResetAfter = 0xffffffffu;

    _targetInitWaitAttempted = false;
    _targetInitWaitSucceeded = false;
    _targetInitWaitMs = 0;
    _fwIndicatorBefore = 0xffffffffu;
    _fwIndicatorAfter = 0xffffffffu;
    _fwIndicatorEventPending = false;
    _fwIndicatorNonZeroSeen = false;
    _fwIndicatorSampleCount = 0;
    _fwIndicatorChangeCount = 0;

    _coldTargetInitSucceeded = false;
    _coldTargetInitMs = 0;
    _coldFWIndicator = 0xffffffffu;
    _warmResetAttempted = false;
    _warmResetSucceeded = false;
    _warmResetIRQMasked = false;
    _warmResetSI0Succeeded = false;
    _warmResetCPU1Succeeded = false;
    _warmResetCPU2Succeeded = false;
    _warmResetCESucceeded = false;
    _warmResetLFCleared = false;
    _warmResetFirstTargetInitSucceeded = false;
    _warmResetSecondTargetInitSucceeded = false;
    _warmResetFirstTargetInitMs = 0;
    _warmResetSecondTargetInitMs = 0;
    _warmResetFirstFWIndicator = 0xffffffffu;
    _warmResetSecondFWIndicator = 0xffffffffu;
    _warmResetControlBefore = 0xffffffffu;
    _warmResetControlAfter = 0xffffffffu;
    _warmResetLFBefore = 0xffffffffu;
    _warmResetLFAfter = 0xffffffffu;
    _postWarmChipId = 0xffffffffu;
    _postWarmChipRevision = 0xffu;

    _cePipeInitAttempted = false;
    _cePipeInitSucceeded = false;
    _cePipesProgrammed = false;
    _cePipeDeinitSucceeded = false;
    _cePipeInitPassCount = 0;
    _cePipeLastVerifiedCount = 0;
    _cePipeDeinitVerifiedCount = 0;

    _ceDiagBounce.release();
    _ceDiagReadAttempted = false;
    _ceDiagReadSucceeded = false;
    _ceDiagReadOnly = true;
    _ceDiagTransferUncertain = false;
    _ceDiagTransactionCount = 0;
    _ceDiagCoreCtrl = 0xffffffffu;
    _ceDiagLastTargetAddress = 0xffffffffu;
    _ceDiagLastCEAddress = 0xffffffffu;
    _ceDiagLastCompletedBytes = 0;
    _ceDiagLastWaitUs = 0;
    _ceDiagLastSrcWriteIndex = 0xffffffffu;
    _ceDiagLastDstWriteIndex = 0xffffffffu;
    _ceDiagLastSrcReadIndex = 0xffffffffu;
    _ceDiagLastDstReadIndex = 0xffffffffu;
    _ceDiagPCIeStateAddress = 0xffffffffu;
    _ceDiagPipeCfgAddress = 0xffffffffu;
    _ceDiagServiceMapAddress = 0xffffffffu;
    _ceDiagConfigFlags = 0xffffffffu;
    _ceDiagWriteAttempted = false;
    _ceDiagWriteSucceeded = false;
    _ceDiagWriteTransactionCount = 0;
    _initConfigAttempted = false;
    _initConfigSucceeded = false;
    _initConfigNoOpWriteVerified = false;
    _initConfigPipeCfgVerified = false;
    _initConfigServiceMapVerified = false;
    _initConfigConfigFlagsVerified = false;
    _initConfigEarlyAllocVerified = false;
    _initConfigOptionFlag2Verified = false;
    _initConfigRollbackAttempted = false;
    _initConfigRollbackSucceeded = false;
    _initConfigTargetDirty = false;
    _initConfigOriginalConfigFlags = 0xffffffffu;
    _initConfigAppliedConfigFlags = 0xffffffffu;
    _initConfigOriginalEarlyAlloc = 0xffffffffu;
    _initConfigAppliedEarlyAlloc = 0xffffffffu;
    _initConfigOriginalOptionFlag2 = 0xffffffffu;
    _initConfigAppliedOptionFlag2 = 0xffffffffu;
    _initConfigTargetNumBanks = 0;
    _initConfigPersistent = false;

    _wakeTargetCPUAttempted = false;
    _wakeTargetCPUSucceeded = false;
    _wakeTargetCPUCoreCtrlBefore = 0xffffffffu;
    _wakeTargetCPUCoreCtrlWritten = 0xffffffffu;
    _wakeTargetCPUCoreCtrlAfter = 0xffffffffu;

    _bmiRequestBuffer.release();
    _bmiResponseBuffer.release();
    _htcReadyBuffer.release();
    _htcControlBuffer.release();
    _wmiRxBuffer.release();
        _wmiTxBuffer.release();
        for(auto &memory:_wmiHostMemory)memory.release();
        for(auto &buffer:_httRecycled)buffer.release();
        _httTxBuffer.release();_httRxIndex.release();_httRxRing.release();_httRxPool.release();
    _boot = Ath10k::BootState{};
    _bmiAttempted = false;
    _bmiSucceeded = false;
    _bmiGetTargetInfoAttempted = false;
    _bmiGetTargetInfoSucceeded = false;
    _bmiPipeValidationAttempted = false;
    _bmiPipeStructuralValidationSucceeded = false;
    _bmiPipeStrictValidationMismatch = false;
    _bmiTransferUncertain = false;
    _bmiTimedOut = false;
    _bmiQuiesceAttempted = false;
    _bmiQuiesceSucceeded = false;
    _dmaQuarantine = false;
    _bmiRequestLength = 0;
    _bmiResponseLength = 0;
    _bmiTargetInfoLength = 0xffffffffu;
    _bmiTargetVersion = 0xffffffffu;
    _bmiTargetType = 0xffffffffu;
    _bmiWaitUs = 0;
    _bmiCE0SrcWrite = 0xffffffffu;
    _bmiCE0SrcRead = 0xffffffffu;
    _bmiCE1DstWrite = 0xffffffffu;
    _bmiCE1DstRead = 0xffffffffu;
    _bmiCompletedBytes = 0;
    _bmiWakeAtTimeout = _bmiRTCAtTimeout = 0xffffffffu;
    _bmiReadMemoryAttempted = false;
    _bmiReadMemorySucceeded = false;
    _bmiReadMemoryTransactionCount = 0;
    _bmiBoardUploadAttempted = false;
    _bmiBoardUploadSucceeded = false;
    _bmiBoardReadbackSucceeded = false;
    _bmiBoardInitVerified = false;
    _bmiWriteMemoryTransactionCount = 0;
    _bmiBoardBytesWritten = 0;
    _bmiBoardBytesVerified = 0;
    _bmiBoardUploadStatus = "not-run";
    _otpAttempted = false;
    _otpStreamStarted = false;
    _otpStreamClosed = false;
    _otpDownloadSucceeded = false;
    _otpExecuteAttempted = false;
    _otpExecuteSucceeded = false;
    _otpResultAccepted = false;
    _otpResultIgnored = false;
    _otpResult = 0xffffffffu;
    _otpWireBytesSent = 0;
    _otpLZDataCommands = 0;
    _otpExecuteResponseBytes = 0;
    _otpStatus = "not-run";
    _fwAttempted = false;
    _fwStreamStarted = false;
    _fwStreamClosed = false;
    _fwDownloadSucceeded = false;
    _fwPostBMIProbeSucceeded = false;
    _fwReadbackAttempted = false;
    _fwReadbackSucceeded = false;
    _fwReadbackInProgress = false;
    _fwWireBytesSent = 0;
    _fwLZDataCommands = 0;
    _fwReadbackBytes = 0;
    _fwVerifiedSegments = 0;
    _fwFailedSegment = 0xffffffffu;
    _fwMismatchAddress = 0xffffffffu;
    _fwPostTargetVersion = 0xffffffffu;
    _fwPostTargetType = 0xffffffffu;
    _fwStatus = "not-run";
    _bmiBoardPrereqAttempted = false;
    _bmiBoardPrereqSucceeded = false;
    _bmiInterconnectMatchesDiag = false;
    _bmiEarlyCfgDoneSeen = false;
    _bmiBoardDataPointerValid = false;
    _bmiBoardExtDataValid = false;
    _bmiDesiredCPUSpeed = 0xffffffffu;
    _bmiBoardDataAddress = 0xffffffffu;
    _bmiBoardDataInitialized = 0xffffffffu;
    _bmiBoardExtDataAddress = 0xffffffffu;
    _bmiBoardExtDataConfig = 0xffffffffu;
    _bmiBoardExtDataSize = 0;
    _bmiCalDataAddress = 0xffffffffu;
    _bmiInterconnectState = 0xffffffffu;
    _bmiOptionFlag2 = 0xffffffffu;
    for (UInt32 &word : _bmiBoardFingerprint)
        word = 0xffffffffu;
}

bool Ath10kPCIDevice::wakeTarget(bool &assertedWake)
{
    assertedWake = false;
    const auto &layout = Ath10k::kQca6174RegisterLayout;
    const UInt32 rtcOffset = Ath10k::rtcStateOffset(layout);
    const UInt32 wakeOffset = Ath10k::socWakeOffset(layout);

    if (!_regs.read32(rtcOffset, _rtcStateBefore)) {
        IOLog("Ath10kMac: target wake ERROR cannot read RTC_STATE offset=0x%08x\n",
              rtcOffset);
        return false;
    }

    const UInt32 beforeState = Ath10k::rtcStateValue(_rtcStateBefore);
    ATH10K_TRACE("Ath10kMac: target RTC_STATE before=0x%08x state=%u expected-on=%u\n",
          _rtcStateBefore, beforeState, layout.rtcStateValueOn);

    // RTC=ON describes the current state, not ownership of a wake request.
    // Linux __ath10k_pci_wake() asserts SOC_WAKE even when already awake.
    _targetWasAlreadyAwake = beforeState == layout.rtcStateValueOn;

    ATH10K_TRACE("Ath10kMac: target wake assert offset=0x%08x value=0x%08x\n",
          wakeOffset, Ath10k::kPcieSocWakeMask);
    if (!_regs.write32(wakeOffset, Ath10k::kPcieSocWakeMask)) {
        IOLog("Ath10kMac: target wake ERROR cannot write SOC_WAKE\n");
        return false;
    }
    assertedWake = true;

    UInt32 totalDelay = 0;
    UInt32 currentDelay = 5;
    UInt32 rtcValue = 0xffffffffu;

    while (totalDelay < Ath10k::kWakeTimeoutUs) {
        if (!_regs.read32(rtcOffset, rtcValue)) {
            IOLog("Ath10kMac: target wake ERROR RTC_STATE became unreadable\n");
            return false;
        }

        if (Ath10k::rtcStateValue(rtcValue) == layout.rtcStateValueOn) {
            _rtcStateAfter = rtcValue;
            _wakeDelayUs = totalDelay;
            ATH10K_TRACE("Ath10kMac: target awake RTC_STATE=0x%08x delay=%u us%s\n",
                  rtcValue, totalDelay,
                  totalDelay > Ath10k::kWakeLateUs ? " (late)" : "");
            return true;
        }

        IODelay(currentDelay);
        totalDelay += currentDelay;
        if (currentDelay < 50)
            currentDelay += 5;
    }

    _rtcStateAfter = rtcValue;
    _wakeDelayUs = totalDelay;
    IOLog("Ath10kMac: target wake TIMEOUT RTC_STATE=0x%08x after=%u us\n",
          rtcValue, totalDelay);
    return false;
}

void Ath10kPCIDevice::releaseTargetWake(bool assertedWake)
{
    if (!assertedWake)
        return;

    const UInt32 wakeOffset = Ath10k::socWakeOffset(Ath10k::kQca6174RegisterLayout);
    if (_regs.write32(wakeOffset, Ath10k::kPcieSocWakeReset))
        ATH10K_TRACE("Ath10kMac: target wake released; SOC_WAKE=0\n");
    else
        IOLog("Ath10kMac: target wake release ERROR\n");
}

bool Ath10kPCIDevice::coldResetTarget()
{
    _coldResetAttempted = true;
    _coldResetSucceeded = false;

    const auto &layout = Ath10k::kQca6174RegisterLayout;
    const UInt32 offset = Ath10k::globalResetOffset(layout);
    UInt32 before = 0xffffffffu;

    ATH10K_TRACE("Ath10kMac: cold reset begin offset=0x%08x mask=0x%08x delay=%u+%u ms\n",
          offset, Ath10k::kSocGlobalResetMask,
          Ath10k::kColdResetDelayMs, Ath10k::kColdResetDelayMs);

    if (!_regs.read32(offset, before) || before == 0xffffffffu) {
        IOLog("Ath10kMac: cold reset ERROR cannot read SOC_GLOBAL_RESET (value=0x%08x)\n",
              before);
        return false;
    }

    _socGlobalResetBefore = before;
    const UInt32 asserted = before | Ath10k::kSocGlobalResetMask;
    const UInt32 deasserted = before & ~Ath10k::kSocGlobalResetMask;

    ATH10K_TRACE("Ath10kMac: cold reset assert SOC_GLOBAL_RESET 0x%08x -> 0x%08x\n",
          before, asserted);
    if (!_regs.write32(offset, asserted)) {
        IOLog("Ath10kMac: cold reset ERROR assert write failed\n");
        return false;
    }

    // Linux explicitly avoids immediate PCIe register access after asserting
    // this reset because the link/controller may be temporarily unstable.
    IOSleep(Ath10k::kColdResetDelayMs);

    ATH10K_TRACE("Ath10kMac: cold reset deassert SOC_GLOBAL_RESET value=0x%08x\n",
          deasserted);
    if (!_regs.write32(offset, deasserted)) {
        IOLog("Ath10kMac: cold reset ERROR deassert write failed\n");
        return false;
    }

    IOSleep(Ath10k::kColdResetDelayMs);

    // Re-assert host-side PCI command bits in case the device reset affected
    // its configuration state. This does not touch CE/firmware state.
    if (_pci) {
        _pci->setMemoryEnable(true);
        _pci->setBusMasterEnable(true);
        ATH10K_TRACE("Ath10kMac: cold reset PCI command after=0x%04x\n",
              _pci->configRead16(kIOPCIConfigCommand));
    }

    // Do not issue a target MMIO read here. Linux's cold-reset path also
    // returns after the second delay; 0.0.91 verifies the released bit only
    // after reacquiring target wake in probeTarget().
    _coldResetSucceeded = true;
    ATH10K_TRACE("Ath10kMac: cold reset pulse COMPLETE; post-reset readback deferred until wake reacquire\n");
    return true;
}

bool Ath10kPCIDevice::waitForTargetInit()
{
    const bool ready=[&]()->bool{
    _targetInitWaitAttempted = true;
    _targetInitWaitSucceeded = false;
    _targetInitWaitMs = 0;
    _fwIndicatorEventPending = false;
    _fwIndicatorNonZeroSeen = false;
    _fwIndicatorSampleCount = 0;
    _fwIndicatorChangeCount = 0;

    const UInt32 offset = Ath10k::kQca6174RegisterLayout.fwIndicatorAddress;
    UInt32 value = 0xffffffffu;
    UInt32 lastValue = 0xffffffffu;

    if (_regs.read32(offset, value))
        _fwIndicatorBefore = value;

    IOLog("Ath10kMac: target-init wait begin FW_INDICATOR=0x%08x timeout=%u ms poll=%u ms irq-mode=%s\n",
          offset, Ath10k::kTargetInitTimeoutMs, Ath10k::kTargetInitPollMs,
          _irqMessaged ? "MSI" : "legacy");

    UInt32 elapsed = 0;
    while (elapsed <= Ath10k::kTargetInitTimeoutMs) {
        if (!_regs.read32(offset, value)) {
            IOLog("Ath10kMac: target-init ERROR FW_INDICATOR MMIO read failed\n");
            _fwIndicatorAfter = 0xffffffffu;
            return false;
        }

        ++_fwIndicatorSampleCount;
        _fwIndicatorAfter = value;
        if (value != 0 && value != 0xffffffffu)
            _fwIndicatorNonZeroSeen = true;
        if (value != lastValue) {
            if (lastValue != 0xffffffffu)
                ++_fwIndicatorChangeCount;
            ATH10K_TRACE("Ath10kMac: target-init indicator change elapsed=%u ms value=0x%08x\n",
                  elapsed, value);
            lastValue = value;
        } else {
            const bool checkpoint =
                elapsed == 10 || elapsed == 50 || elapsed == 100 ||
                elapsed == 250 || elapsed == 500 || elapsed == 1000 ||
                elapsed == 1500 || elapsed == 2000 || elapsed == 2500 ||
                elapsed == 3000;
            if (checkpoint)
                ATH10K_TRACE("Ath10kMac: target-init indicator sample elapsed=%u ms value=0x%08x\n",
                      elapsed, value);
        }

        if (value != 0xffffffffu) {
            if (value & Ath10k::kFwIndicatorEventPending) {
                _fwIndicatorEventPending = true;
                IOLog("Ath10kMac: target-init ERROR event-pending indicator=0x%08x elapsed=%u ms\n",
                      value, elapsed);
                return false;
            }

            if (value & Ath10k::kFwIndicatorInitialized) {
                _targetInitWaitSucceeded = true;
                _targetInitWaitMs = elapsed;
                ATH10K_TRACE("Ath10kMac: target-init READY indicator=0x%08x elapsed=%u ms samples=%u changes=%u\n",
                      value, elapsed, _fwIndicatorSampleCount, _fwIndicatorChangeCount);
                return true;
            }
        }

        if (elapsed == Ath10k::kTargetInitTimeoutMs)
            break;

        if(!_irqMessaged && value!=0xffffffffu){
            // Linux's INTx target/BAR initialization race workaround. PCI
            // Interrupt Disable remains set while the bootstrap is polled.
            UInt32 flush=0;const UInt32 mask=Ath10k::kPCIeIntrFirmwareMask|Ath10k::kPCIeIntrCEMaskAll;
            const UInt32 enable=Ath10k::socCoreOffset(Ath10k::kQca6174RegisterLayout,Ath10k::kPCIeIntrEnableAddress);
            if(!_regs.write32(enable,mask) || !_regs.read32(enable,flush) || flush!=mask)return false;
        }
        IOSleep(Ath10k::kTargetInitPollMs);
        elapsed += Ath10k::kTargetInitPollMs;
        if (elapsed > Ath10k::kTargetInitTimeoutMs)
            elapsed = Ath10k::kTargetInitTimeoutMs;
    }

    _targetInitWaitMs = elapsed;
    IOLog("Ath10kMac: target-init TIMEOUT indicator=0x%08x elapsed=%u ms samples=%u changes=%u nonzero-seen=%s\n",
          _fwIndicatorAfter, elapsed, _fwIndicatorSampleCount,
          _fwIndicatorChangeCount, _fwIndicatorNonZeroSeen ? "yes" : "no");
    return false;
    }();
    // Always mask and clear on departure, including timeout/crash. The PCI
    // command may be restored during cleanup, so no stale level may remain.
    if(!_irqMessaged){
        UInt32 flush=0;const auto &l=Ath10k::kQca6174RegisterLayout;
        const bool masked=_regs.write32(Ath10k::socCoreOffset(l,Ath10k::kPCIeIntrEnableAddress),0) &&
            _regs.write32(Ath10k::socCoreOffset(l,Ath10k::kPCIeIntrClearAddress),Ath10k::kPCIeIntrFirmwareMask|Ath10k::kPCIeIntrCEMaskAll) &&
            _regs.read32(Ath10k::socCoreOffset(l,Ath10k::kPCIeIntrEnableAddress),flush) && flush==0;
        if(!masked){_targetInitWaitSucceeded=false;_dmaQuarantine=true;return false;}
    }
    return ready;
}

bool Ath10kPCIDevice::readPostResetChipId()
{
    const auto &layout = Ath10k::kQca6174RegisterLayout;
    const UInt32 offset = Ath10k::chipIdOffset(layout);
    UInt32 chipId = 0xffffffffu;

    if (!_regs.read32(offset, chipId)) {
        IOLog("Ath10kMac: post-reset chip-id ERROR invalid/unreadable offset=0x%08x\n",
              offset);
        return false;
    }

    _chipId = chipId;
    if (chipId == 0xffffffffu) {
        IOLog("Ath10kMac: post-reset chip-id ERROR device returned 0xffffffff\n");
        return false;
    }

    _chipRevision = Ath10k::chipRevision(chipId);
    _selectedHw = Ath10k::findCandidate(_deviceId, _chipRevision);
    _chipIdValid = (_selectedHw != nullptr);

    ATH10K_TRACE("Ath10kMac: SOC_CHIP_ID post-reset=0x%08x chip-rev-field=%u (0x%x) supported=%s\n",
          _chipId, _chipRevision, _chipRevision,
          _chipIdValid ? "yes" : "no");

    if (_selectedHw) {
        ATH10K_TRACE("Ath10kMac: selected target profile=%s firmware=%s cpu=%u diag-ce=%s\n",
              _selectedHw->targetRevision,
              _selectedHw->firmwareDirectory,
              _selectedHw->targetCpuFrequencyHz,
              _selectedHw->firmwareDiagCeDownload ? "yes" : "no");
    } else {
        ATH10K_TRACE("Ath10kMac: post-reset chip revision is not in supported table for PCI device %04x\n",
              _deviceId);
    }

    return _chipIdValid;
}

bool Ath10kPCIDevice::maskTargetIRQsForWarmReset()
{
    bool ok = true;
    for (UInt32 ceId = 0; ceId < Ath10k::kCECount; ++ceId) {
        const auto &cfg = Ath10k::kCEHostConfig[ceId];
        if (cfg.poll)
            continue; // Linux ath10k_ce_disable_interrupt() skips polling CEs.

        const UInt32 base = Ath10k::ceBaseAddress(ceId);
        UInt32 hostIE = 0xffffffffu;
        UInt32 miscIE = 0xffffffffu;
        if (!_regs.read32(base + Ath10k::kCEHostIE, hostIE) ||
            !_regs.read32(base + Ath10k::kCEMiscIE, miscIE)) {
            ok = false;
            continue;
        }
        ok &= _regs.write32(base + Ath10k::kCEHostIE,
                            hostIE & ~Ath10k::kCEHostInterruptMask);
        ok &= _regs.write32(base + Ath10k::kCEMiscIE,
                            miscIE & ~Ath10k::kCEMiscErrorMask);
    }

    const auto &layout = Ath10k::kQca6174RegisterLayout;
    const UInt32 intrEnable = Ath10k::socCoreOffset(layout, Ath10k::kPCIeIntrEnableAddress);
    const UInt32 intrClear = Ath10k::socCoreOffset(layout, Ath10k::kPCIeIntrClearAddress);
    const UInt32 coreCtrl = Ath10k::socCoreOffset(layout, Ath10k::kCoreCtrlAddress);
    const UInt32 clearMask = Ath10k::kPCIeIntrFirmwareMask | Ath10k::kPCIeIntrCEMaskAll;

    ok &= _regs.write32(intrEnable, 0);
    ok &= _regs.write32(intrClear, clearMask);
    UInt32 flush = 0xffffffffu;
    ok &= _regs.read32(intrEnable, flush);

    UInt32 core = 0xffffffffu;
    if (_regs.read32(coreCtrl, core)) {
        ok &= _regs.write32(coreCtrl, core & ~Ath10k::kCoreCtrlPCIeReg31Mask);
    } else {
        ok = false;
    }

    UInt32 coreAfter = 0xffffffffu;
    ok &= _regs.read32(coreCtrl, coreAfter);
    _warmResetIRQMasked = ok && flush == 0 &&
                          (coreAfter & Ath10k::kCoreCtrlPCIeReg31Mask) == 0;
    ATH10K_TRACE("Ath10kMac: warm reset IRQ mask %s intr-enable=0x%08x clear-mask=0x%08x core-ctrl=0x%08x\n",
          _warmResetIRQMasked ? "PASS" : "FAIL", flush, clearMask, coreAfter);
    return _warmResetIRQMasked;
}

bool Ath10kPCIDevice::warmResetSI0()
{
    const UInt32 offset = Ath10k::socResetControlOffset(Ath10k::kQca6174RegisterLayout);
    UInt32 val = 0xffffffffu;
    bool ok = _regs.read32(offset, val);
    if (!ok)
        return false;

    ATH10K_TRACE("Ath10kMac: warm reset SI0 begin control=0x%08x mask=0x%08x%s\n",
          val, Ath10k::kSocResetControlSI0ResetMask,
          Ath10k::kSocResetControlSI0ResetMask == 0 ? " (QCA6174 timing/no-op mask)" : "");
    ok &= _regs.write32(offset, val | Ath10k::kSocResetControlSI0ResetMask);
    UInt32 flush = 0xffffffffu;
    ok &= _regs.read32(offset, flush);
    IOSleep(Ath10k::kWarmResetDelayMs);
    ok &= _regs.read32(offset, val);
    ok &= _regs.write32(offset, val & ~Ath10k::kSocResetControlSI0ResetMask);
    ok &= _regs.read32(offset, flush);
    IOSleep(Ath10k::kWarmResetDelayMs);

    _warmResetSI0Succeeded = ok;
    ATH10K_TRACE("Ath10kMac: warm reset SI0 %s control=0x%08x\n",
          ok ? "PASS" : "FAIL", flush);
    return ok;
}

bool Ath10kPCIDevice::warmResetCPU(const char *stage)
{
    const auto &layout = Ath10k::kQca6174RegisterLayout;
    const UInt32 resetOffset = Ath10k::socResetControlOffset(layout);
    UInt32 val = 0xffffffffu;
    if (!_regs.write32(layout.fwIndicatorAddress, 0) || !_regs.read32(resetOffset, val)) {
        IOLog("Ath10kMac: warm reset CPU %s ERROR preparing FW_IND/reset-control\n",
              stage ? stage : "stage");
        return false;
    }

    const UInt32 requested = val | Ath10k::kSocResetControlCPUWarmMask;
    const bool ok = _regs.write32(resetOffset, requested);
    ATH10K_TRACE("Ath10kMac: warm reset CPU %s %s FW_IND=0 reset-control 0x%08x -> 0x%08x\n",
          stage ? stage : "stage", ok ? "asserted" : "FAIL", val, requested);
    return ok;
}

bool Ath10kPCIDevice::warmResetClearLF()
{
    const UInt32 offset = Ath10k::socLFTimerControl0Offset(Ath10k::kQca6174RegisterLayout);
    UInt32 before = 0xffffffffu;
    UInt32 after = 0xffffffffu;
    bool ok = _regs.read32(offset, before);
    if (ok)
        ok &= _regs.write32(offset, before & ~Ath10k::kSocLFTimerEnableMask);
    if (ok)
        ok &= _regs.read32(offset, after);

    _warmResetLFBefore = before;
    _warmResetLFAfter = after;
    _warmResetLFCleared = ok && (after & Ath10k::kSocLFTimerEnableMask) == 0;
    ATH10K_TRACE("Ath10kMac: warm reset LF timer %s 0x%08x -> 0x%08x enable-mask=0x%08x\n",
          _warmResetLFCleared ? "PASS" : "FAIL", before, after,
          Ath10k::kSocLFTimerEnableMask);
    return _warmResetLFCleared;
}

bool Ath10kPCIDevice::warmResetCE()
{
    const UInt32 offset = Ath10k::socResetControlOffset(Ath10k::kQca6174RegisterLayout);
    UInt32 val = 0xffffffffu;
    if (!_regs.read32(offset, val))
        return false;

    ATH10K_TRACE("Ath10kMac: warm reset CE assert control=0x%08x mask=0x%08x\n",
          val, Ath10k::kSocResetControlCEResetMask);
    if (!_regs.write32(offset, val | Ath10k::kSocResetControlCEResetMask))
        return false;
    IOSleep(Ath10k::kWarmResetDelayMs);
    if (!_regs.write32(offset, val & ~Ath10k::kSocResetControlCEResetMask))
        return false;

    _warmResetCESucceeded = true;
    ATH10K_TRACE("Ath10kMac: warm reset CE deassert requested value=0x%08x\n",
          val & ~Ath10k::kSocResetControlCEResetMask);
    return true;
}

bool Ath10kPCIDevice::verifyCopyEnginePipe(UInt32 ceId,
                                           const Ath10k::CESnapshot &snap) const
{
    if (ceId >= Ath10k::kCECount || !snap.valid)
        return false;
    const auto &cfg = Ath10k::kCEHostConfig[ceId];
    bool ok = true;
    if (!cfg.poll) {
        ok &= (snap.hostIE & Ath10k::kCEHostInterruptMask) == 0;
        ok &= (snap.miscIE & Ath10k::kCEMiscErrorMask) == 0;
    }

    if (cfg.srcEntries != 0) {
        ok &= _ceSrcRings[ceId].allocated();
        ok &= snap.srcBase == _ceSrcRings[ceId].dmaAddress32();
        ok &= snap.srcSize == cfg.srcEntries;
        ok &= (snap.ctrl1 & Ath10k::kCECtrl1DmaxMask) ==
              (cfg.srcSizeMax & Ath10k::kCECtrl1DmaxMask);
        ok &= (snap.ctrl1 & Ath10k::kCECtrl1SrcByteSwapMask) == 0;
        ok &= snap.srcWatermark == Ath10k::watermarkValue(0, cfg.srcEntries);
    }
    if (cfg.dstEntries != 0) {
        ok &= _ceDstRings[ceId].allocated();
        ok &= snap.dstBase == _ceDstRings[ceId].dmaAddress32();
        ok &= snap.dstSize == cfg.dstEntries;
        ok &= (snap.ctrl1 & Ath10k::kCECtrl1DstByteSwapMask) == 0;
        ok &= snap.dstWatermark == Ath10k::watermarkValue(0, cfg.dstEntries);
    }
    return ok;
}

bool Ath10kPCIDevice::initOneCopyEnginePipe(UInt32 ceId, const char *stage)
{
    if (ceId >= Ath10k::kCECount)
        return false;
    const auto &cfg = Ath10k::kCEHostConfig[ceId];
    if (cfg.srcEntries == 0 && cfg.dstEntries == 0) {
        ATH10K_TRACE("Ath10kMac: CE%u init %s no host ring (%s)\n",
              ceId, stage ? stage : "pass", cfg.role);
        return true;
    }

    const UInt32 base = Ath10k::ceBaseAddress(ceId);
    bool ok = true;

    if (cfg.srcEntries != 0) {
        if (!_ceSrcRings[ceId].allocated())
            return false;
        bzero(_ceSrcRings[ceId].virtualAddress(), static_cast<size_t>(_ceSrcRings[ceId].bytes()));
        OSSynchronizeIO();
        if (!_ceSrcRings[ceId].syncForDevice()) {
            ++_ceRingSyncOutFailureCount;
            IOLog("Ath10kMac: CE%u init %s ERROR source ring syncForDevice failed\n",
                  ceId, stage ? stage : "pass");
            return false;
        }
        ++_ceRingSyncOutCount;

        UInt32 ctrl1 = 0xffffffffu;
        ok &= _regs.read32(base + Ath10k::kCECtrl1, ctrl1);
        ctrl1 &= ~(Ath10k::kCECtrl1DmaxMask | Ath10k::kCECtrl1SrcByteSwapMask);
        ctrl1 |= (cfg.srcSizeMax & Ath10k::kCECtrl1DmaxMask);
        ok &= _regs.write32(base + Ath10k::kCESrcRingBaseLo, _ceSrcRings[ceId].dmaAddress32());
        ok &= _regs.write32(base + Ath10k::kCESrcRingSize, cfg.srcEntries);
        ok &= _regs.write32(base + Ath10k::kCECtrl1, ctrl1);
        ok &= _regs.write32(base + Ath10k::kCESrcWatermark,
                            Ath10k::watermarkValue(0, cfg.srcEntries));
    }

    if (cfg.dstEntries != 0) {
        if (!_ceDstRings[ceId].allocated())
            return false;
        bzero(_ceDstRings[ceId].virtualAddress(), static_cast<size_t>(_ceDstRings[ceId].bytes()));
        OSSynchronizeIO();
        if (!_ceDstRings[ceId].syncForDevice()) {
            ++_ceRingSyncOutFailureCount;
            IOLog("Ath10kMac: CE%u init %s ERROR destination ring syncForDevice failed\n",
                  ceId, stage ? stage : "pass");
            return false;
        }
        ++_ceRingSyncOutCount;

        UInt32 ctrl1 = 0xffffffffu;
        ok &= _regs.read32(base + Ath10k::kCECtrl1, ctrl1);
        ctrl1 &= ~Ath10k::kCECtrl1DstByteSwapMask;
        ok &= _regs.write32(base + Ath10k::kCEDstRingBaseLo, _ceDstRings[ceId].dmaAddress32());
        ok &= _regs.write32(base + Ath10k::kCEDstRingSize, cfg.dstEntries);
        ok &= _regs.write32(base + Ath10k::kCECtrl1, ctrl1);
        ok &= _regs.write32(base + Ath10k::kCEDstWatermark,
                            Ath10k::watermarkValue(0, cfg.dstEntries));
    }

    Ath10k::CESnapshot snap{};
    const bool readOk = Ath10k::readCESnapshot(_regs, ceId, snap);
    const bool verified = ok && readOk && verifyCopyEnginePipe(ceId, snap);
    ATH10K_TRACE("Ath10kMac: CE%u init %s readback=%s src=%08x/%u dst=%08x/%u ctrl1=%08x src-wm=%08x dst-wm=%08x idx=%u/%u,%u/%u role=%s\n",
          ceId, stage ? stage : "pass", verified ? "PASS" : "FAIL",
          snap.srcBase, snap.srcSize, snap.dstBase, snap.dstSize, snap.ctrl1,
          snap.srcWatermark, snap.dstWatermark,
          snap.srcWriteIndex, snap.currentSrcRead,
          snap.dstWriteIndex, snap.currentDstRead, cfg.role);
    return verified;
}

bool Ath10kPCIDevice::initCopyEnginePipes(const char *stage)
{
    _cePipeInitAttempted = true;
    _cePipeLastVerifiedCount = 0;
    UInt32 configured = 0;
    ATH10K_TRACE("Ath10kMac: CE init_pipes begin stage=%s transfers=OFF interrupts=MASKED diag-dmax=0x%x\n",
          stage ? stage : "pass", Ath10k::kCEDiagTransferLimit);

    for (UInt32 ceId = 0; ceId < Ath10k::kCECount; ++ceId) {
        const auto &cfg = Ath10k::kCEHostConfig[ceId];
        if (cfg.srcEntries != 0 || cfg.dstEntries != 0)
            ++configured;
        if (!initOneCopyEnginePipe(ceId, stage)) {
            IOLog("Ath10kMac: CE init_pipes FAILED stage=%s ce=%u; deinitializing programmed host rings\n",
                  stage ? stage : "pass", ceId);
            deinitCopyEnginePipes("init-failure-cleanup");
            return false;
        }
        if (cfg.srcEntries != 0 || cfg.dstEntries != 0)
            ++_cePipeLastVerifiedCount;
    }

    _cePipesProgrammed = true;
    _cePipeInitSucceeded = (_cePipeLastVerifiedCount == configured);
    if (_cePipeInitSucceeded)
        ++_cePipeInitPassCount;
    ATH10K_TRACE("Ath10kMac: CE init_pipes %s stage=%s verified=%u/%u pass-count=%u\n",
          _cePipeInitSucceeded ? "READY" : "FAILED", stage ? stage : "pass",
          _cePipeLastVerifiedCount, configured, _cePipeInitPassCount);
    return _cePipeInitSucceeded;
}

bool Ath10kPCIDevice::deinitOneCopyEnginePipe(UInt32 ceId)
{
    if (ceId >= Ath10k::kCECount)
        return false;
    const auto &cfg = Ath10k::kCEHostConfig[ceId];
    const UInt32 base = Ath10k::ceBaseAddress(ceId);
    bool ok = true;

    if (cfg.srcEntries != 0) {
        UInt32 ctrl1 = 0xffffffffu;
        ok &= _regs.read32(base + Ath10k::kCECtrl1, ctrl1);
        ctrl1 &= ~Ath10k::kCECtrl1DmaxMask;
        ok &= _regs.write32(base + Ath10k::kCESrcRingBaseLo, 0);
        ok &= _regs.write32(base + Ath10k::kCESrcRingSize, 0);
        ok &= _regs.write32(base + Ath10k::kCECtrl1, ctrl1);
        ok &= _regs.write32(base + Ath10k::kCESrcWatermark, 0);
    }
    if (cfg.dstEntries != 0) {
        ok &= _regs.write32(base + Ath10k::kCEDstRingBaseLo, 0);
        ok &= _regs.write32(base + Ath10k::kCEDstRingSize, 0);
        ok &= _regs.write32(base + Ath10k::kCEDstWatermark, 0);
    }

    if (cfg.srcEntries == 0 && cfg.dstEntries == 0)
        return ok;

    Ath10k::CESnapshot snap{};
    const bool readOk = Ath10k::readCESnapshot(_regs, ceId, snap);
    bool verified = ok && readOk;
    if (cfg.srcEntries != 0) {
        verified &= snap.srcBase == 0 && snap.srcSize == 0;
        verified &= (snap.ctrl1 & Ath10k::kCECtrl1DmaxMask) == 0;
        verified &= (snap.srcWatermark & Ath10k::kCEWatermarkHighMask) == 0;
    }
    if (cfg.dstEntries != 0) {
        verified &= snap.dstBase == 0 && snap.dstSize == 0;
        verified &= (snap.dstWatermark & Ath10k::kCEWatermarkHighMask) == 0;
    }
    return verified;
}

bool Ath10kPCIDevice::deinitCopyEnginePipes(const char *stage)
{
    UInt32 verified = 0;
    UInt32 configured = 0;
    bool allOk = true;
    ATH10K_TRACE("Ath10kMac: CE deinit_pipes begin stage=%s\n", stage ? stage : "cleanup");
    for (UInt32 ceId = 0; ceId < Ath10k::kCECount; ++ceId) {
        const auto &cfg = Ath10k::kCEHostConfig[ceId];
        if (cfg.srcEntries == 0 && cfg.dstEntries == 0)
            continue;
        ++configured;
        const bool ok = deinitOneCopyEnginePipe(ceId);
        if (ok)
            ++verified;
        else
            allOk = false;
        ATH10K_TRACE("Ath10kMac: CE%u deinit readback=%s\n", ceId, ok ? "PASS" : "FAIL");
    }
    _cePipeDeinitVerifiedCount = verified;
    _cePipeDeinitSucceeded = allOk && verified == configured;
    if (_cePipeDeinitSucceeded)
        _cePipesProgrammed = false;
    ATH10K_TRACE("Ath10kMac: CE deinit_pipes %s verified=%u/%u\n",
          _cePipeDeinitSucceeded ? "READY" : "FAILED", verified, configured);
    return _cePipeDeinitSucceeded;
}

bool Ath10kPCIDevice::targetCpuToCEAddress(UInt32 targetAddress, UInt32 &ceAddress)
{
    const UInt32 coreCtrlOffset =
        Ath10k::socCoreOffset(Ath10k::kQca6174RegisterLayout, Ath10k::kCoreCtrlAddress);
    UInt32 coreCtrl = 0xffffffffu;
    if (!_regs.read32(coreCtrlOffset, coreCtrl) || coreCtrl == 0xffffffffu)
        return false;

    _ceDiagCoreCtrl = coreCtrl;

    const UInt32 region = targetAddress & 0x000fffffu;
    const UInt32 window = (coreCtrl & 0x000007ffu) << 21;
    const UInt32 upperRegion = (targetAddress >= 0x00100000u) ? 0x00100000u : 0u;
    ceAddress = window | upperRegion | region;
    return true;
}

bool Ath10kPCIDevice::ce7DiagnosticRead(UInt32 targetAddress,
                                        void *outData,
                                        UInt32 nbytes)
{
    static constexpr UInt32 kDiagCE = 7;

    if (!outData || nbytes == 0 || nbytes > Ath10k::kCEDiagTransferLimit ||
        (targetAddress & 0x3u) != 0 || (nbytes & 0x3u) != 0) {
        IOLog("Ath10kMac: CE7 diag read ERROR invalid request target=0x%08x bytes=%u\n",
              targetAddress, nbytes);
        return false;
    }

    if (!_cePipesProgrammed ||
        !_ceSrcRings[kDiagCE].allocated() ||
        !_ceDstRings[kDiagCE].allocated() ||
        !_ceDiagBounce.allocated() ||
        _ceDiagBounce.length() < nbytes) {
        IOLog("Ath10kMac: CE7 diag read ERROR rings/bounce not ready\n");
        return false;
    }

    Ath10k::CESnapshot before{};
    if (!Ath10k::readCESnapshot(_regs, kDiagCE, before) ||
        !verifyCopyEnginePipe(kDiagCE, before)) {
        IOLog("Ath10kMac: CE7 diag read ERROR CE7 pipe readback invalid\n");
        return false;
    }

    const UInt32 srcEntries = Ath10k::kCEHostConfig[kDiagCE].srcEntries;
    const UInt32 dstEntries = Ath10k::kCEHostConfig[kDiagCE].dstEntries;
    if (before.srcWriteIndex >= srcEntries || before.currentSrcRead >= srcEntries ||
        before.dstWriteIndex >= dstEntries || before.currentDstRead >= dstEntries ||
        before.srcWriteIndex != before.currentSrcRead ||
        before.dstWriteIndex != before.currentDstRead) {
        IOLog("Ath10kMac: CE7 diag read ERROR rings not idle src=%u/%u dst=%u/%u\n",
              before.srcWriteIndex, before.currentSrcRead,
              before.dstWriteIndex, before.currentDstRead);
        return false;
    }

    UInt32 ceAddress = 0xffffffffu;
    if (!targetCpuToCEAddress(targetAddress, ceAddress)) {
        IOLog("Ath10kMac: CE7 diag read ERROR target->CE address conversion failed target=0x%08x\n",
              targetAddress);
        return false;
    }

    _ceDiagLastTargetAddress = targetAddress;
    _ceDiagLastCEAddress = ceAddress;
    _ceDiagLastCompletedBytes = 0;
    _ceDiagLastWaitUs = 0;
    _ceDiagLastSrcWriteIndex = before.srcWriteIndex;
    _ceDiagLastDstWriteIndex = before.dstWriteIndex;
    _ceDiagPreSubmitSyncSucceeded = false;
    _ceDiagCompletionSyncSucceeded = false;

    bzero(_ceDiagBounce.virtualAddress(), static_cast<size_t>(nbytes));

    auto *srcRing = reinterpret_cast<Ath10k::CEDescriptor32 *>(
        _ceSrcRings[kDiagCE].virtualAddress());
    auto *dstRing = reinterpret_cast<Ath10k::CEDescriptor32 *>(
        _ceDstRings[kDiagCE].virtualAddress());

    const UInt32 srcSlot = before.srcWriteIndex;
    const UInt32 dstSlot = before.dstWriteIndex;
    const UInt32 srcNext = Ath10k::ceRingNext(srcSlot, srcEntries);
    const UInt32 dstNext = Ath10k::ceRingNext(dstSlot, dstEntries);

    auto *srcDesc = &srcRing[srcSlot];
    auto *dstDesc = &dstRing[dstSlot];

    bzero(srcDesc, sizeof(*srcDesc));
    bzero(dstDesc, sizeof(*dstDesc));

    // Same ordering as Linux ath10k_pci_diag_read_mem(): post a host
    // destination buffer first, then submit a source descriptor whose address
    // is in Target CE address space.
    dstDesc->address = static_cast<UInt32>(_ceDiagBounce.dmaAddress());
    dstDesc->nbytes = 0;
    dstDesc->flags = 0;

    srcDesc->address = ceAddress;
    srcDesc->nbytes = static_cast<UInt16>(nbytes);
    srcDesc->flags = 0;
    OSSynchronizeIO();

    // IODMACommand may be exposing internal bounce buffers rather than the
    // CPU-visible memory above. Explicitly publish all CPU writes before the
    // CE is allowed to fetch either ring descriptor or the host bounce buffer.
    if (!_ceDiagBounce.syncForDevice()) {
        ++_ceDiagSyncFailureCount;
        IOLog("Ath10kMac: CE7 diag read ERROR bounce syncForDevice failed before submit\n");
        return false;
    }
    ++_ceDiagSyncOutCount;

    if (!_ceDstRings[kDiagCE].syncForDevice()) {
        ++_ceDiagSyncFailureCount;
        IOLog("Ath10kMac: CE7 diag read ERROR destination ring syncForDevice failed before submit\n");
        return false;
    }
    ++_ceDiagSyncOutCount;

    const UInt32 base = Ath10k::ceBaseAddress(kDiagCE);
    ATH10K_TRACE("Ath10kMac: CE7 diag READ begin target=0x%08x ce=0x%08x bytes=%u core-ctrl=0x%08x bounce-iovm=0x%08x src-slot=%u dst-slot=%u mapper=%s\n",
          targetAddress, ceAddress, nbytes, _ceDiagCoreCtrl,
          static_cast<UInt32>(_ceDiagBounce.dmaAddress()), srcSlot, dstSlot,
          _ceDiagBounce.mapperModeName());

    if (!_regs.write32(base + Ath10k::kCEDstWriteIndex, dstNext)) {
        IOLog("Ath10kMac: CE7 diag read ERROR cannot post destination index\n");
        return false;
    }
    OSSynchronizeIO();
    if (!_regs.read32(base + Ath10k::kCEDstWriteIndex, _ceDiagLastDstWriteIndex) ||
        _ceDiagLastDstWriteIndex != dstNext) {
        IOLog("Ath10kMac: CE7 diag read ERROR destination write-index readback=%u expected=%u\n",
              _ceDiagLastDstWriteIndex, dstNext);
        return false;
    }

    // Publish the source descriptor only after the destination has been posted.
    if (!_ceSrcRings[kDiagCE].syncForDevice()) {
        ++_ceDiagSyncFailureCount;
        IOLog("Ath10kMac: CE7 diag read ERROR source ring syncForDevice failed after destination post\n");
        return false;
    }
    ++_ceDiagSyncOutCount;
    _ceDiagPreSubmitSyncSucceeded = true;

    if (!_regs.write32(base + Ath10k::kCESrcWriteIndex, srcNext)) {
        IOLog("Ath10kMac: CE7 diag read ERROR cannot submit source index\n");
        return false;
    }

    // From this point a real CE7 DMA transaction has been submitted. Keep
    // this sticky until both source and destination completion are proven.
    _ceDiagTransferUncertain = true;
    OSSynchronizeIO();
    if (!_regs.read32(base + Ath10k::kCESrcWriteIndex, _ceDiagLastSrcWriteIndex)) {
        IOLog("Ath10kMac: CE7 diag read ERROR source write-index readback failed; DMA state UNCERTAIN\n");
        return false;
    }
    ATH10K_TRACE("Ath10kMac: CE7 diag submit readback dst-w=%u/%u src-w=%u/%u mapper=%s sync-out=%u\n",
          _ceDiagLastDstWriteIndex, dstNext, _ceDiagLastSrcWriteIndex, srcNext,
          _ceDiagBounce.mapperModeName(), _ceDiagSyncOutCount);
    if (_ceDiagLastSrcWriteIndex != srcNext) {
        IOLog("Ath10kMac: CE7 diag read ERROR source write-index did not latch; DMA state UNCERTAIN\n");
        return false;
    }

    bool sourceDone = false;
    bool destinationDone = false;
    UInt16 completedBytes = 0;
    UInt32 waitedUs = 0;
    UInt32 srcRead = 0xffffffffu;
    UInt32 dstRead = 0xffffffffu;

    for (;;) {
        OSSynchronizeIO();

        if (!_regs.read32(base + Ath10k::kCECurrentSrcRead, srcRead) ||
            !_regs.read32(base + Ath10k::kCECurrentDstRead, dstRead)) {
            IOLog("Ath10kMac: CE7 diag read ERROR cannot poll CE read indices\n");
            return false;
        }

        // Hardware completion metadata may live in an IODMACommand internal
        // buffer. Pull it back before examining nbytes/address.
        if (!_ceDstRings[kDiagCE].syncFromDevice()) {
            ++_ceDiagSyncFailureCount;
            IOLog("Ath10kMac: CE7 diag read ERROR destination ring syncFromDevice failed; DMA state UNCERTAIN\n");
            return false;
        }
        ++_ceDiagSyncInCount;

        completedBytes = dstDesc->nbytes;
        sourceDone = ((srcRead & (srcEntries - 1u)) == srcNext);
        destinationDone = (completedBytes != 0);

        if (sourceDone && destinationDone)
            break;

        if (waitedUs >= Ath10k::kCEDiagAccessTimeoutUs)
            break;

        IODelay(Ath10k::kCEDiagAccessWaitUs);
        waitedUs += Ath10k::kCEDiagAccessWaitUs;
    }

    _ceDiagLastWaitUs = waitedUs;
    _ceDiagLastSrcReadIndex = srcRead;
    _ceDiagLastDstReadIndex = dstRead;
    _ceDiagLastCompletedBytes = completedBytes;

    if (!sourceDone || !destinationDone) {
        IOLog("Ath10kMac: CE7 diag read TIMEOUT target=0x%08x waited=%u us src-w=%u src-r=%u expected=%u dst-w=%u dst-r=%u completed=%u sync-in=%u; DMA state UNCERTAIN\n",
              targetAddress, waitedUs, _ceDiagLastSrcWriteIndex, srcRead, srcNext,
              _ceDiagLastDstWriteIndex, dstRead, completedBytes, _ceDiagSyncInCount);
        return false;
    }

    // Both CE sides completed, so the transaction itself is no longer
    // uncertain. Payload synchronization failures below are data-integrity
    // failures, not in-flight DMA failures.
    _ceDiagTransferUncertain = false;

    if (completedBytes != nbytes ||
        dstDesc->address != static_cast<UInt32>(_ceDiagBounce.dmaAddress())) {
        IOLog("Ath10kMac: CE7 diag read ERROR completion mismatch completed=%u expected=%u dst-addr=0x%08x expected-addr=0x%08x\n",
              completedBytes, nbytes, dstDesc->address,
              static_cast<UInt32>(_ceDiagBounce.dmaAddress()));
        return false;
    }

    if (!_ceDiagBounce.syncFromDevice()) {
        ++_ceDiagSyncFailureCount;
        IOLog("Ath10kMac: CE7 diag read ERROR bounce syncFromDevice failed after completion\n");
        return false;
    }
    ++_ceDiagSyncInCount;
    _ceDiagCompletionSyncSucceeded = true;

    bcopy(_ceDiagBounce.virtualAddress(), outData, static_cast<size_t>(nbytes));

    // Mirror Linux completion bookkeeping enough for deterministic reuse.
    // Publish the cleared descriptor metadata too, otherwise a double-buffered
    // ring could retain the old completion state when the two-entry ring wraps.
    srcDesc->nbytes = 0;
    dstDesc->nbytes = 0;
    OSSynchronizeIO();
    bool cleanupSyncOk = true;
    if (!_ceSrcRings[kDiagCE].syncForDevice()) {
        ++_ceDiagSyncFailureCount;
        cleanupSyncOk = false;
    } else {
        ++_ceDiagSyncOutCount;
    }
    if (!_ceDstRings[kDiagCE].syncForDevice()) {
        ++_ceDiagSyncFailureCount;
        cleanupSyncOk = false;
    } else {
        ++_ceDiagSyncOutCount;
    }
    if (!cleanupSyncOk) {
        IOLog("Ath10kMac: CE7 diag read ERROR descriptor cleanup syncForDevice failed after safe completion\n");
        return false;
    }

    ++_ceDiagTransactionCount;

    UInt32 value = 0;
    if (nbytes >= sizeof(UInt32))
        bcopy(outData, &value, sizeof(value));

    ATH10K_TRACE("Ath10kMac: CE7 diag READ COMPLETE target=0x%08x ce=0x%08x bytes=%u value0=0x%08x waited=%u us src-r=%u dst-r=%u transaction=%u sync-out=%u sync-in=%u\n",
          targetAddress, ceAddress, completedBytes, value, waitedUs,
          srcRead, dstRead, _ceDiagTransactionCount,
          _ceDiagSyncOutCount, _ceDiagSyncInCount);
    return true;
}

bool Ath10kPCIDevice::ce7DiagnosticWrite(UInt32 targetAddress,
                                         const void *data,
                                         UInt32 nbytes)
{
    static constexpr UInt32 kDiagCE = 7;

    _ceDiagWriteAttempted = true;

    if (!data || nbytes == 0 || nbytes > Ath10k::kCEDiagTransferLimit ||
        (targetAddress & 0x3u) != 0 || (nbytes & 0x3u) != 0) {
        IOLog("Ath10kMac: CE7 diag write ERROR invalid request target=0x%08x bytes=%u\n",
              targetAddress, nbytes);
        return false;
    }

    if (!_cePipesProgrammed ||
        !_ceSrcRings[kDiagCE].allocated() ||
        !_ceDstRings[kDiagCE].allocated() ||
        !_ceDiagBounce.allocated() ||
        _ceDiagBounce.length() < nbytes) {
        IOLog("Ath10kMac: CE7 diag write ERROR rings/bounce not ready\n");
        return false;
    }

    Ath10k::CESnapshot before{};
    if (!Ath10k::readCESnapshot(_regs, kDiagCE, before) ||
        !verifyCopyEnginePipe(kDiagCE, before)) {
        IOLog("Ath10kMac: CE7 diag write ERROR CE7 pipe readback invalid\n");
        return false;
    }

    const UInt32 srcEntries = Ath10k::kCEHostConfig[kDiagCE].srcEntries;
    const UInt32 dstEntries = Ath10k::kCEHostConfig[kDiagCE].dstEntries;
    if (before.srcWriteIndex >= srcEntries || before.currentSrcRead >= srcEntries ||
        before.dstWriteIndex >= dstEntries || before.currentDstRead >= dstEntries ||
        before.srcWriteIndex != before.currentSrcRead ||
        before.dstWriteIndex != before.currentDstRead) {
        IOLog("Ath10kMac: CE7 diag write ERROR rings not idle src=%u/%u dst=%u/%u\n",
              before.srcWriteIndex, before.currentSrcRead,
              before.dstWriteIndex, before.currentDstRead);
        return false;
    }

    UInt32 ceAddress = 0xffffffffu;
    if (!targetCpuToCEAddress(targetAddress, ceAddress)) {
        IOLog("Ath10kMac: CE7 diag write ERROR target->CE address conversion failed target=0x%08x\n",
              targetAddress);
        return false;
    }

    _ceDiagLastTargetAddress = targetAddress;
    _ceDiagLastCEAddress = ceAddress;
    _ceDiagLastCompletedBytes = 0;
    _ceDiagLastWaitUs = 0;
    _ceDiagLastSrcWriteIndex = before.srcWriteIndex;
    _ceDiagLastDstWriteIndex = before.dstWriteIndex;
    _ceDiagPreSubmitSyncSucceeded = false;
    _ceDiagCompletionSyncSucceeded = false;

    bcopy(data, _ceDiagBounce.virtualAddress(), static_cast<size_t>(nbytes));
    OSSynchronizeIO();
    if (!_ceDiagBounce.syncForDevice()) {
        ++_ceDiagSyncFailureCount;
        IOLog("Ath10kMac: CE7 diag write ERROR bounce syncForDevice failed before submit\n");
        return false;
    }
    ++_ceDiagSyncOutCount;

    auto *srcRing = reinterpret_cast<Ath10k::CEDescriptor32 *>(
        _ceSrcRings[kDiagCE].virtualAddress());
    auto *dstRing = reinterpret_cast<Ath10k::CEDescriptor32 *>(
        _ceDstRings[kDiagCE].virtualAddress());

    const UInt32 srcSlot = before.srcWriteIndex;
    const UInt32 dstSlot = before.dstWriteIndex;
    const UInt32 srcNext = Ath10k::ceRingNext(srcSlot, srcEntries);
    const UInt32 dstNext = Ath10k::ceRingNext(dstSlot, dstEntries);

    auto *srcDesc = &srcRing[srcSlot];
    auto *dstDesc = &dstRing[dstSlot];
    bzero(srcDesc, sizeof(*srcDesc));
    bzero(dstDesc, sizeof(*dstDesc));

    // Linux ath10k_pci_diag_write_mem() posts a Target destination first,
    // then sends the host bounce buffer through the CE7 source ring.
    dstDesc->address = ceAddress;
    dstDesc->nbytes = 0;
    dstDesc->flags = 0;

    srcDesc->address = static_cast<UInt32>(_ceDiagBounce.dmaAddress());
    srcDesc->nbytes = static_cast<UInt16>(nbytes);
    srcDesc->flags = 0;
    OSSynchronizeIO();

    if (!_ceDstRings[kDiagCE].syncForDevice()) {
        ++_ceDiagSyncFailureCount;
        IOLog("Ath10kMac: CE7 diag write ERROR destination ring syncForDevice failed\n");
        return false;
    }
    ++_ceDiagSyncOutCount;

    const UInt32 base = Ath10k::ceBaseAddress(kDiagCE);
    UInt32 value0 = 0;
    if (nbytes >= sizeof(UInt32))
        bcopy(data, &value0, sizeof(value0));
    ATH10K_TRACE("Ath10kMac: CE7 diag WRITE begin target=0x%08x ce=0x%08x bytes=%u value0=0x%08x bounce-iovm=0x%08x src-slot=%u dst-slot=%u\n",
          targetAddress, ceAddress, nbytes, value0,
          static_cast<UInt32>(_ceDiagBounce.dmaAddress()), srcSlot, dstSlot);

    if (!_regs.write32(base + Ath10k::kCEDstWriteIndex, dstNext)) {
        IOLog("Ath10kMac: CE7 diag write ERROR cannot post destination index\n");
        return false;
    }
    OSSynchronizeIO();
    if (!_regs.read32(base + Ath10k::kCEDstWriteIndex, _ceDiagLastDstWriteIndex) ||
        _ceDiagLastDstWriteIndex != dstNext) {
        IOLog("Ath10kMac: CE7 diag write ERROR destination write-index readback=%u expected=%u\n",
              _ceDiagLastDstWriteIndex, dstNext);
        return false;
    }

    if (!_ceSrcRings[kDiagCE].syncForDevice()) {
        ++_ceDiagSyncFailureCount;
        IOLog("Ath10kMac: CE7 diag write ERROR source ring syncForDevice failed after destination post\n");
        return false;
    }
    ++_ceDiagSyncOutCount;
    _ceDiagPreSubmitSyncSucceeded = true;

    if (!_regs.write32(base + Ath10k::kCESrcWriteIndex, srcNext)) {
        IOLog("Ath10kMac: CE7 diag write ERROR cannot submit source index\n");
        return false;
    }

    _ceDiagTransferUncertain = true;
    OSSynchronizeIO();
    if (!_regs.read32(base + Ath10k::kCESrcWriteIndex, _ceDiagLastSrcWriteIndex)) {
        IOLog("Ath10kMac: CE7 diag write ERROR source write-index readback failed; DMA state UNCERTAIN\n");
        return false;
    }
    ATH10K_TRACE("Ath10kMac: CE7 diag WRITE submit readback dst-w=%u/%u src-w=%u/%u sync-out=%u\n",
          _ceDiagLastDstWriteIndex, dstNext, _ceDiagLastSrcWriteIndex, srcNext,
          _ceDiagSyncOutCount);
    if (_ceDiagLastSrcWriteIndex != srcNext) {
        IOLog("Ath10kMac: CE7 diag write ERROR source write-index did not latch; DMA state UNCERTAIN\n");
        return false;
    }

    bool sourceDone = false;
    bool destinationDone = false;
    UInt16 completedBytes = 0;
    UInt32 waitedUs = 0;
    UInt32 srcRead = 0xffffffffu;
    UInt32 dstRead = 0xffffffffu;

    for (;;) {
        OSSynchronizeIO();
        if (!_regs.read32(base + Ath10k::kCECurrentSrcRead, srcRead) ||
            !_regs.read32(base + Ath10k::kCECurrentDstRead, dstRead)) {
            IOLog("Ath10kMac: CE7 diag write ERROR cannot poll CE read indices\n");
            return false;
        }

        if (!_ceDstRings[kDiagCE].syncFromDevice()) {
            ++_ceDiagSyncFailureCount;
            IOLog("Ath10kMac: CE7 diag write ERROR destination ring syncFromDevice failed; DMA state UNCERTAIN\n");
            return false;
        }
        ++_ceDiagSyncInCount;

        completedBytes = dstDesc->nbytes;
        sourceDone = ((srcRead & (srcEntries - 1u)) == srcNext);
        destinationDone = (completedBytes != 0);
        if (sourceDone && destinationDone)
            break;
        if (waitedUs >= Ath10k::kCEDiagAccessTimeoutUs)
            break;
        IODelay(Ath10k::kCEDiagAccessWaitUs);
        waitedUs += Ath10k::kCEDiagAccessWaitUs;
    }

    _ceDiagLastWaitUs = waitedUs;
    _ceDiagLastSrcReadIndex = srcRead;
    _ceDiagLastDstReadIndex = dstRead;
    _ceDiagLastCompletedBytes = completedBytes;

    if (!sourceDone || !destinationDone) {
        IOLog("Ath10kMac: CE7 diag write TIMEOUT target=0x%08x waited=%u us src-w=%u src-r=%u expected=%u dst-w=%u dst-r=%u completed=%u; DMA state UNCERTAIN\n",
              targetAddress, waitedUs, _ceDiagLastSrcWriteIndex, srcRead, srcNext,
              _ceDiagLastDstWriteIndex, dstRead, completedBytes);
        return false;
    }

    _ceDiagTransferUncertain = false;

    if (completedBytes != nbytes || dstDesc->address != ceAddress) {
        IOLog("Ath10kMac: CE7 diag write ERROR completion mismatch completed=%u expected=%u dst-addr=0x%08x expected-target=0x%08x\n",
              completedBytes, nbytes, dstDesc->address, ceAddress);
        return false;
    }
    _ceDiagCompletionSyncSucceeded = true;

    srcDesc->nbytes = 0;
    dstDesc->nbytes = 0;
    OSSynchronizeIO();
    bool cleanupSyncOk = true;
    if (!_ceSrcRings[kDiagCE].syncForDevice()) {
        ++_ceDiagSyncFailureCount;
        cleanupSyncOk = false;
    } else {
        ++_ceDiagSyncOutCount;
    }
    if (!_ceDstRings[kDiagCE].syncForDevice()) {
        ++_ceDiagSyncFailureCount;
        cleanupSyncOk = false;
    } else {
        ++_ceDiagSyncOutCount;
    }
    if (!cleanupSyncOk) {
        IOLog("Ath10kMac: CE7 diag write ERROR descriptor cleanup syncForDevice failed after safe completion\n");
        return false;
    }

    ++_ceDiagTransactionCount;
    ++_ceDiagWriteTransactionCount;
    ATH10K_TRACE("Ath10kMac: CE7 diag WRITE COMPLETE target=0x%08x ce=0x%08x bytes=%u value0=0x%08x waited=%u us src-r=%u dst-r=%u transaction=%u write-transaction=%u\n",
          targetAddress, ceAddress, completedBytes, value0, waitedUs,
          srcRead, dstRead, _ceDiagTransactionCount, _ceDiagWriteTransactionCount);
    return true;
}

bool Ath10kPCIDevice::runCE7DiagnosticReadTest()
{
    _ceDiagReadAttempted = true;
    _ceDiagReadSucceeded = false;
    _ceDiagReadOnly = true;
    _ceDiagTransferUncertain = false;
    _ceDiagTransactionCount = 0;
    _ceDiagPreSubmitSyncSucceeded = false;
    _ceDiagCompletionSyncSucceeded = false;
    _ceDiagSyncOutCount = 0;
    _ceDiagSyncInCount = 0;
    _ceDiagSyncFailureCount = 0;
    _ceDiagPCIeStateAddress = 0xffffffffu;
    _ceDiagPipeCfgAddress = 0xffffffffu;
    _ceDiagServiceMapAddress = 0xffffffffu;
    _ceDiagConfigFlags = 0xffffffffu;
    _ceDiagBounce.release();

    if (!_warmResetSucceeded || !_cePipesProgrammed) {
        IOLog("Ath10kMac: CE7 diagnostic discovery ERROR warm reset/pipes not ready\n");
        return false;
    }

    // One reusable DMA buffer. 0.0.91 accepts a null device mapper and relies
    // on IODMACommand::kMapped plus explicit synchronize() calls so internal
    // double-buffering remains coherent with CPU-visible memory.
    if (!_dmaMapperReady) {
        IOLog("Ath10kMac: CE7 diagnostic discovery ERROR DMA mapping policy not ready\n");
        return false;
    }

    if (!_ceDiagBounce.allocate(Ath10k::kCEDiagTransferLimit, 4, _dmaMapper)) {
        IOLog("Ath10kMac: CE7 diagnostic discovery ERROR bounce DMA allocation failed\n");
        return false;
    }

    _dmaMapperUsedByDiag = _ceDiagBounce.usesExplicitMapper();
    IOLog("Ath10kMac: CE7 diagnostic READ-ONLY discovery begin host-interest=0x%08x timeout=%u us poll=%u us mapper=%s explicit-sync=yes\n",
          Ath10k::kHostInterestInterconnectAddress,
          Ath10k::kCEDiagAccessTimeoutUs, Ath10k::kCEDiagAccessWaitUs,
          _ceDiagBounce.mapperModeName());

    if (!ce7DiagnosticRead(Ath10k::kHostInterestInterconnectAddress,
                           &_ceDiagPCIeStateAddress, sizeof(UInt32))) {
        IOLog("Ath10kMac: CE7 diagnostic discovery ERROR hi_interconnect_state read failed\n");
        return false;
    }
    if (_ceDiagPCIeStateAddress == 0 || _ceDiagPCIeStateAddress == 0xffffffffu) {
        IOLog("Ath10kMac: CE7 diagnostic discovery ERROR invalid pcie_state address=0x%08x\n",
              _ceDiagPCIeStateAddress);
        return false;
    }

    if (!ce7DiagnosticRead(_ceDiagPCIeStateAddress + Ath10k::kPCIeStatePipeCfgOffset,
                           &_ceDiagPipeCfgAddress, sizeof(UInt32))) {
        IOLog("Ath10kMac: CE7 diagnostic discovery ERROR pipe_cfg_addr read failed\n");
        return false;
    }
    if (_ceDiagPipeCfgAddress == 0 || _ceDiagPipeCfgAddress == 0xffffffffu) {
        IOLog("Ath10kMac: CE7 diagnostic discovery ERROR invalid pipe_cfg_addr=0x%08x\n",
              _ceDiagPipeCfgAddress);
        return false;
    }

    if (!ce7DiagnosticRead(_ceDiagPCIeStateAddress + Ath10k::kPCIeStateServiceMapOffset,
                           &_ceDiagServiceMapAddress, sizeof(UInt32))) {
        IOLog("Ath10kMac: CE7 diagnostic discovery ERROR svc_to_pipe_map read failed\n");
        return false;
    }
    if (_ceDiagServiceMapAddress == 0 || _ceDiagServiceMapAddress == 0xffffffffu) {
        IOLog("Ath10kMac: CE7 diagnostic discovery ERROR invalid svc_to_pipe_map=0x%08x\n",
              _ceDiagServiceMapAddress);
        return false;
    }

    if (!ce7DiagnosticRead(_ceDiagPCIeStateAddress + Ath10k::kPCIeStateConfigFlagsOffset,
                           &_ceDiagConfigFlags, sizeof(UInt32))) {
        IOLog("Ath10kMac: CE7 diagnostic discovery ERROR config_flags read failed\n");
        return false;
    }

    _ceDiagReadSucceeded = true;
    ATH10K_TRACE("Ath10kMac: CE7 diagnostic READ-ONLY READY transactions=%u pcie-state=0x%08x pipe-cfg=0x%08x svc-map=0x%08x config-flags=0x%08x\n",
          _ceDiagTransactionCount, _ceDiagPCIeStateAddress,
          _ceDiagPipeCfgAddress, _ceDiagServiceMapAddress, _ceDiagConfigFlags);
    ATH10K_TRACE("Ath10kMac: CE7 discovery stage COMPLETE; persistent init_config + wake_target_cpu + BMI_GET_TARGET_INFO follow; firmware remains disabled\n");
    return true;
}

bool Ath10kPCIDevice::runCE7InitConfigPersistentTest()
{
    _ceDiagWriteAttempted = true;
    _ceDiagWriteSucceeded = false;
    _ceDiagWriteTransactionCount = 0;
    _ceDiagReadOnly = false;
    _initConfigAttempted = true;
    _initConfigSucceeded = false;
    _initConfigNoOpWriteVerified = false;
    _initConfigPipeCfgVerified = false;
    _initConfigServiceMapVerified = false;
    _initConfigConfigFlagsVerified = false;
    _initConfigEarlyAllocVerified = false;
    _initConfigOptionFlag2Verified = false;
    _initConfigRollbackAttempted = false;
    _initConfigRollbackSucceeded = false;
    _initConfigTargetDirty = false;
    _initConfigOriginalConfigFlags = 0xffffffffu;
    _initConfigAppliedConfigFlags = 0xffffffffu;
    _initConfigOriginalEarlyAlloc = 0xffffffffu;
    _initConfigAppliedEarlyAlloc = 0xffffffffu;
    _initConfigOriginalOptionFlag2 = 0xffffffffu;
    _initConfigAppliedOptionFlag2 = 0xffffffffu;
    _initConfigTargetNumBanks = Ath10k::targetIRAMBanks(_deviceId, _chipRevision);
    _initConfigPersistent = false;

    if (!_ceDiagReadSucceeded || !_cePipesProgrammed || !_ceDiagBounce.allocated() ||
        _ceDiagPCIeStateAddress == 0 || _ceDiagPCIeStateAddress == 0xffffffffu ||
        _ceDiagPipeCfgAddress == 0 || _ceDiagPipeCfgAddress == 0xffffffffu ||
        _ceDiagServiceMapAddress == 0 || _ceDiagServiceMapAddress == 0xffffffffu) {
        IOLog("Ath10kMac: init_config persistent ERROR discovery/pipes/bounce not ready\n");
        return false;
    }
    if (_initConfigTargetNumBanks == 0) {
        IOLog("Ath10kMac: init_config persistent ERROR unknown target IRAM bank count device=%04x rev=%u; semantic writes refused\n",
              _deviceId, _chipRevision);
        return false;
    }

    UInt8 pipeOriginal[Ath10k::kQCA6174TargetCEConfigBytes];
    UInt8 serviceOriginal[Ath10k::kQCA6174ServiceMapBytes];
    UInt8 verifyBuffer[Ath10k::kQCA6174ServiceMapBytes];
    bzero(pipeOriginal, sizeof(pipeOriginal));
    bzero(serviceOriginal, sizeof(serviceOriginal));
    bzero(verifyBuffer, sizeof(verifyBuffer));

    const UInt32 configFlagsAddress =
        _ceDiagPCIeStateAddress + Ath10k::kPCIeStateConfigFlagsOffset;

    ATH10K_TRACE("Ath10kMac: init_config PERSISTENT begin pipe-cfg=0x%08x bytes=%u svc-map=0x%08x bytes=%u config-flags=0x%08x early-alloc=0x%08x option-flag2=0x%08x banks=%u\n",
          _ceDiagPipeCfgAddress, Ath10k::kQCA6174TargetCEConfigBytes,
          _ceDiagServiceMapAddress, Ath10k::kQCA6174ServiceMapBytes,
          configFlagsAddress, Ath10k::kHostInterestEarlyAllocAddress,
          Ath10k::kHostInterestOptionFlag2Address, _initConfigTargetNumBanks);

    // First prove target writes with a no-op 32-bit write/readback. This does
    // not change target semantics and therefore needs no rollback.
    if (!ce7DiagnosticRead(configFlagsAddress,
                           &_initConfigOriginalConfigFlags, sizeof(UInt32))) {
        IOLog("Ath10kMac: init_config ERROR cannot snapshot config_flags\n");
        return false;
    }
    if (!ce7DiagnosticWrite(configFlagsAddress,
                            &_initConfigOriginalConfigFlags, sizeof(UInt32))) {
        IOLog("Ath10kMac: init_config ERROR no-op diagnostic write failed\n");
        return false;
    }
    UInt32 noOpReadback = 0xffffffffu;
    if (!ce7DiagnosticRead(configFlagsAddress, &noOpReadback, sizeof(UInt32)) ||
        noOpReadback != _initConfigOriginalConfigFlags) {
        IOLog("Ath10kMac: init_config ERROR no-op write readback=0x%08x expected=0x%08x\n",
              noOpReadback, _initConfigOriginalConfigFlags);
        return false;
    }
    _initConfigNoOpWriteVerified = true;
    ATH10K_TRACE("Ath10kMac: init_config no-op WRITE/READBACK PASS config-flags=0x%08x\n",
          _initConfigOriginalConfigFlags);

    // Snapshot every target region before making semantic changes.
    if (!ce7DiagnosticRead(_ceDiagPipeCfgAddress, pipeOriginal,
                           Ath10k::kQCA6174TargetCEConfigBytes) ||
        !ce7DiagnosticRead(_ceDiagServiceMapAddress, serviceOriginal,
                           Ath10k::kQCA6174ServiceMapBytes) ||
        !ce7DiagnosticRead(Ath10k::kHostInterestEarlyAllocAddress,
                           &_initConfigOriginalEarlyAlloc, sizeof(UInt32)) ||
        !ce7DiagnosticRead(Ath10k::kHostInterestOptionFlag2Address,
                           &_initConfigOriginalOptionFlag2, sizeof(UInt32))) {
        IOLog("Ath10kMac: init_config ERROR snapshot read failed before semantic writes\n");
        return false;
    }

    _initConfigAppliedConfigFlags =
        _initConfigOriginalConfigFlags & ~Ath10k::kPCIeConfigFlagEnableL1;
    _initConfigAppliedEarlyAlloc = _initConfigOriginalEarlyAlloc |
        ((Ath10k::kHIEarlyAllocMagic << Ath10k::kHIEarlyAllocMagicShift) &
         Ath10k::kHIEarlyAllocMagicMask) |
        ((_initConfigTargetNumBanks << Ath10k::kHIEarlyAllocIRAMBanksShift) &
         Ath10k::kHIEarlyAllocIRAMBanksMask);
    _initConfigAppliedOptionFlag2 =
        _initConfigOriginalOptionFlag2 | Ath10k::kHIOptionEarlyCfgDone;

    ATH10K_TRACE("Ath10kMac: init_config snapshot config-flags=0x%08x->0x%08x early-alloc=0x%08x->0x%08x option-flag2=0x%08x->0x%08x\n",
          _initConfigOriginalConfigFlags, _initConfigAppliedConfigFlags,
          _initConfigOriginalEarlyAlloc, _initConfigAppliedEarlyAlloc,
          _initConfigOriginalOptionFlag2, _initConfigAppliedOptionFlag2);

    bool applyOk = true;
    bool pipeMayBeApplied = false;
    bool serviceMayBeApplied = false;
    bool configMayBeApplied = false;
    bool earlyMayBeApplied = false;
    bool optionMayBeApplied = false;

    // Linux order: pipe_cfg, service map, config flags, early alloc, option flag2.
    if (applyOk) {
        pipeMayBeApplied = true;
        if (!ce7DiagnosticWrite(_ceDiagPipeCfgAddress,
                                Ath10k::kQCA6174TargetCEConfig,
                                Ath10k::kQCA6174TargetCEConfigBytes)) {
            IOLog("Ath10kMac: init_config APPLY ERROR pipe_cfg write failed\n");
            applyOk = false;
        } else {
            bzero(verifyBuffer, sizeof(verifyBuffer));
            _initConfigPipeCfgVerified =
                ce7DiagnosticRead(_ceDiagPipeCfgAddress, verifyBuffer,
                                  Ath10k::kQCA6174TargetCEConfigBytes) &&
                ath10kBytesEqual(verifyBuffer, Ath10k::kQCA6174TargetCEConfig,
                                 Ath10k::kQCA6174TargetCEConfigBytes);
            applyOk = _initConfigPipeCfgVerified;
            ATH10K_TRACE("Ath10kMac: init_config pipe_cfg readback=%s entries=%u bytes=%u CE5=OUT/2048\n",
                  _initConfigPipeCfgVerified ? "PASS" : "FAIL",
                  Ath10k::kQCA6174TargetCEConfigCount,
                  Ath10k::kQCA6174TargetCEConfigBytes);
        }
    }

    if (applyOk) {
        serviceMayBeApplied = true;
        if (!ce7DiagnosticWrite(_ceDiagServiceMapAddress,
                                Ath10k::kQCA6174ServiceMap,
                                Ath10k::kQCA6174ServiceMapBytes)) {
            IOLog("Ath10kMac: init_config APPLY ERROR service map write failed\n");
            applyOk = false;
        } else {
            bzero(verifyBuffer, sizeof(verifyBuffer));
            _initConfigServiceMapVerified =
                ce7DiagnosticRead(_ceDiagServiceMapAddress, verifyBuffer,
                                  Ath10k::kQCA6174ServiceMapBytes) &&
                ath10kBytesEqual(verifyBuffer, Ath10k::kQCA6174ServiceMap,
                                 Ath10k::kQCA6174ServiceMapBytes);
            applyOk = _initConfigServiceMapVerified;
            ATH10K_TRACE("Ath10kMac: init_config svc_to_pipe readback=%s entries=%u bytes=%u HTT-RX=CE1\n",
                  _initConfigServiceMapVerified ? "PASS" : "FAIL",
                  Ath10k::kQCA6174ServiceMapCount,
                  Ath10k::kQCA6174ServiceMapBytes);
        }
    }

    if (applyOk) {
        configMayBeApplied = true;
        UInt32 readback = 0xffffffffu;
        _initConfigConfigFlagsVerified =
            ce7DiagnosticWrite(configFlagsAddress, &_initConfigAppliedConfigFlags,
                               sizeof(UInt32)) &&
            ce7DiagnosticRead(configFlagsAddress, &readback, sizeof(UInt32)) &&
            readback == _initConfigAppliedConfigFlags;
        applyOk = _initConfigConfigFlagsVerified;
        ATH10K_TRACE("Ath10kMac: init_config config_flags readback=%s value=0x%08x (L1 bit cleared)\n",
              _initConfigConfigFlagsVerified ? "PASS" : "FAIL", readback);
    }

    if (applyOk) {
        earlyMayBeApplied = true;
        UInt32 readback = 0xffffffffu;
        _initConfigEarlyAllocVerified =
            ce7DiagnosticWrite(Ath10k::kHostInterestEarlyAllocAddress,
                               &_initConfigAppliedEarlyAlloc, sizeof(UInt32)) &&
            ce7DiagnosticRead(Ath10k::kHostInterestEarlyAllocAddress,
                              &readback, sizeof(UInt32)) &&
            readback == _initConfigAppliedEarlyAlloc;
        applyOk = _initConfigEarlyAllocVerified;
        ATH10K_TRACE("Ath10kMac: init_config hi_early_alloc readback=%s value=0x%08x magic=0x%04x banks=%u\n",
              _initConfigEarlyAllocVerified ? "PASS" : "FAIL", readback,
              Ath10k::kHIEarlyAllocMagic, _initConfigTargetNumBanks);
    }

    if (applyOk) {
        optionMayBeApplied = true;
        UInt32 readback = 0xffffffffu;
        _initConfigOptionFlag2Verified =
            ce7DiagnosticWrite(Ath10k::kHostInterestOptionFlag2Address,
                               &_initConfigAppliedOptionFlag2, sizeof(UInt32)) &&
            ce7DiagnosticRead(Ath10k::kHostInterestOptionFlag2Address,
                              &readback, sizeof(UInt32)) &&
            readback == _initConfigAppliedOptionFlag2;
        applyOk = _initConfigOptionFlag2Verified;
        ATH10K_TRACE("Ath10kMac: init_config hi_option_flag2 readback=%s value=0x%08x EARLY_CFG_DONE=%s\n",
              _initConfigOptionFlag2Verified ? "PASS" : "FAIL", readback,
              (readback & Ath10k::kHIOptionEarlyCfgDone) ? "set" : "clear");
    }

    const bool anySemanticWrite = pipeMayBeApplied || serviceMayBeApplied ||
        configMayBeApplied || earlyMayBeApplied || optionMayBeApplied;

    // Do not submit rollback traffic when an earlier DMA completion is unknown.
    // In that case probeTarget() will quarantine bus mastering and require reboot.
    bool rollbackOk = !anySemanticWrite;
    if (!applyOk && anySemanticWrite && !_ceDiagTransferUncertain) {
        _initConfigRollbackAttempted = true;
        rollbackOk = true;
        ATH10K_TRACE("Ath10kMac: init_config ROLLBACK begin semantic-apply=%s\n",
              applyOk ? "complete" : "partial");

        if (optionMayBeApplied && !_ceDiagTransferUncertain) {
            UInt32 readback = 0xffffffffu;
            const bool ok = ce7DiagnosticWrite(Ath10k::kHostInterestOptionFlag2Address,
                                               &_initConfigOriginalOptionFlag2,
                                               sizeof(UInt32)) &&
                            ce7DiagnosticRead(Ath10k::kHostInterestOptionFlag2Address,
                                              &readback, sizeof(UInt32)) &&
                            readback == _initConfigOriginalOptionFlag2;
            rollbackOk &= ok;
            ATH10K_TRACE("Ath10kMac: init_config rollback option_flag2=%s value=0x%08x\n",
                  ok ? "PASS" : "FAIL", readback);
        }

        if (earlyMayBeApplied && !_ceDiagTransferUncertain) {
            UInt32 readback = 0xffffffffu;
            const bool ok = ce7DiagnosticWrite(Ath10k::kHostInterestEarlyAllocAddress,
                                               &_initConfigOriginalEarlyAlloc,
                                               sizeof(UInt32)) &&
                            ce7DiagnosticRead(Ath10k::kHostInterestEarlyAllocAddress,
                                              &readback, sizeof(UInt32)) &&
                            readback == _initConfigOriginalEarlyAlloc;
            rollbackOk &= ok;
            ATH10K_TRACE("Ath10kMac: init_config rollback early_alloc=%s value=0x%08x\n",
                  ok ? "PASS" : "FAIL", readback);
        }

        if (configMayBeApplied && !_ceDiagTransferUncertain) {
            UInt32 readback = 0xffffffffu;
            const bool ok = ce7DiagnosticWrite(configFlagsAddress,
                                               &_initConfigOriginalConfigFlags,
                                               sizeof(UInt32)) &&
                            ce7DiagnosticRead(configFlagsAddress,
                                              &readback, sizeof(UInt32)) &&
                            readback == _initConfigOriginalConfigFlags;
            rollbackOk &= ok;
            ATH10K_TRACE("Ath10kMac: init_config rollback config_flags=%s value=0x%08x\n",
                  ok ? "PASS" : "FAIL", readback);
        }

        if (serviceMayBeApplied && !_ceDiagTransferUncertain) {
            bzero(verifyBuffer, sizeof(verifyBuffer));
            const bool ok = ce7DiagnosticWrite(_ceDiagServiceMapAddress,
                                               serviceOriginal,
                                               Ath10k::kQCA6174ServiceMapBytes) &&
                            ce7DiagnosticRead(_ceDiagServiceMapAddress,
                                              verifyBuffer,
                                              Ath10k::kQCA6174ServiceMapBytes) &&
                            ath10kBytesEqual(verifyBuffer, serviceOriginal,
                                             Ath10k::kQCA6174ServiceMapBytes);
            rollbackOk &= ok;
            ATH10K_TRACE("Ath10kMac: init_config rollback service_map=%s\n",
                  ok ? "PASS" : "FAIL");
        }

        if (pipeMayBeApplied && !_ceDiagTransferUncertain) {
            bzero(verifyBuffer, sizeof(verifyBuffer));
            const bool ok = ce7DiagnosticWrite(_ceDiagPipeCfgAddress,
                                               pipeOriginal,
                                               Ath10k::kQCA6174TargetCEConfigBytes) &&
                            ce7DiagnosticRead(_ceDiagPipeCfgAddress,
                                              verifyBuffer,
                                              Ath10k::kQCA6174TargetCEConfigBytes) &&
                            ath10kBytesEqual(verifyBuffer, pipeOriginal,
                                             Ath10k::kQCA6174TargetCEConfigBytes);
            rollbackOk &= ok;
            ATH10K_TRACE("Ath10kMac: init_config rollback pipe_cfg=%s\n",
                  ok ? "PASS" : "FAIL");
        }

        _initConfigRollbackSucceeded = rollbackOk && !_ceDiagTransferUncertain;
    }

    if (applyOk && !_ceDiagTransferUncertain) {
        _initConfigPersistent = true;
        _initConfigTargetDirty = false; // applied state is intentional until BMI quiesce reset
        _initConfigSucceeded = true;
        _ceDiagWriteSucceeded = _initConfigNoOpWriteVerified;
        _ceDiagConfigFlags = _initConfigAppliedConfigFlags;
        ATH10K_TRACE("Ath10kMac: init_config PERSISTENT APPLIED apply=verified rollback=not-requested target-dirty=no diag-writes=%u total-transactions=%u wake-target-cpu=NEXT\n",
              _ceDiagWriteTransactionCount, _ceDiagTransactionCount);
        return true;
    }

    _initConfigTargetDirty = anySemanticWrite && !_initConfigRollbackSucceeded;
    _initConfigSucceeded = false;
    _ceDiagWriteSucceeded = _initConfigNoOpWriteVerified && !_ceDiagTransferUncertain;
    if (_initConfigRollbackSucceeded)
        _ceDiagConfigFlags = _initConfigOriginalConfigFlags;

    IOLog("Ath10kMac: init_config PERSISTENT FAILED apply=failed rollback=%s target-dirty=%s diag-writes=%u total-transactions=%u wake-target-cpu=SKIPPED\n",
          _initConfigRollbackSucceeded ? "verified" :
              (_initConfigRollbackAttempted ? "failed" : "not-attempted"),
          _initConfigTargetDirty ? "yes" : "no",
          _ceDiagWriteTransactionCount, _ceDiagTransactionCount);

    if (_initConfigTargetDirty && !_ceDiagTransferUncertain)
        ATH10K_TRACE("Ath10kMac: init_config WARNING target semantic state not fully restored; REBOOT REQUIRED before another test\n");

    return false;
}


bool Ath10kPCIDevice::wakeTargetCPUForBMI()
{
    _wakeTargetCPUAttempted = true;
    _wakeTargetCPUSucceeded = false;
    _wakeTargetCPUCoreCtrlBefore = 0xffffffffu;
    _wakeTargetCPUCoreCtrlWritten = 0xffffffffu;
    _wakeTargetCPUCoreCtrlAfter = 0xffffffffu;

    if (!_initConfigPersistent || !_cePipesProgrammed) {
        IOLog("Ath10kMac: wake_target_cpu ERROR persistent init_config/CE pipes not ready\n");
        return false;
    }

    const UInt32 addr = Ath10k::socCoreOffset(Ath10k::kQca6174RegisterLayout,
                                               Ath10k::kCoreCtrlAddress);
    if (!_regs.read32(addr, _wakeTargetCPUCoreCtrlBefore)) {
        IOLog("Ath10kMac: wake_target_cpu ERROR cannot read CORE_CTRL\n");
        return false;
    }

    _wakeTargetCPUCoreCtrlWritten =
        _wakeTargetCPUCoreCtrlBefore | Ath10k::kCoreCtrlCPUIntrMask;
    if (!_regs.write32(addr, _wakeTargetCPUCoreCtrlWritten)) {
        IOLog("Ath10kMac: wake_target_cpu ERROR cannot write CORE_CTRL\n");
        return false;
    }

    OSSynchronizeIO();
    IODelay(100);
    _regs.read32(addr, _wakeTargetCPUCoreCtrlAfter); // informational; trigger may self-clear
    _wakeTargetCPUSucceeded = true;
    ATH10K_TRACE("Ath10kMac: wake_target_cpu PASS CORE_CTRL 0x%08x -> write 0x%08x readback=0x%08x CPU_INTR=0x%08x\n",
          _wakeTargetCPUCoreCtrlBefore, _wakeTargetCPUCoreCtrlWritten,
          _wakeTargetCPUCoreCtrlAfter, Ath10k::kCoreCtrlCPUIntrMask);
    return true;
}

bool Ath10kPCIDevice::validateBMIPipeStructure(UInt32 ceId,
                                                  const Ath10k::CESnapshot &snap) const
{
    // BMI runs after wake_target_cpu(). At that point some CE interrupt/watermark
    // state is no longer required to match the bring-up diagnostic snapshot.
    // Linux ath10k does not re-validate HOST_IE/MISC_IE/watermarks before BMI;
    // it uses the already initialized CE0/CE1 rings directly. Keep only the
    // invariants required for a safe DMA submission.
    if (ceId >= Ath10k::kCECount || !snap.valid)
        return false;

    const auto &cfg = Ath10k::kCEHostConfig[ceId];
    bool ok = true;

    if (cfg.srcEntries != 0) {
        ok &= _ceSrcRings[ceId].allocated();
        ok &= snap.srcBase == _ceSrcRings[ceId].dmaAddress32();
        ok &= snap.srcSize == cfg.srcEntries;
        ok &= (snap.ctrl1 & Ath10k::kCECtrl1DmaxMask) ==
              (cfg.srcSizeMax & Ath10k::kCECtrl1DmaxMask);
        ok &= (snap.ctrl1 & Ath10k::kCECtrl1SrcByteSwapMask) == 0;
    }

    if (cfg.dstEntries != 0) {
        ok &= _ceDstRings[ceId].allocated();
        ok &= snap.dstBase == _ceDstRings[ceId].dmaAddress32();
        ok &= snap.dstSize == cfg.dstEntries;
        ok &= (snap.ctrl1 & Ath10k::kCECtrl1DstByteSwapMask) == 0;
    }

    return ok;
}

bool Ath10kPCIDevice::bmiExchange(const void *request, UInt32 requestLength,
                                  void *response, UInt32 &responseLength)
{
    if (_bmiGetTargetInfoAttempted && !_bmiGetTargetInfoSucceeded && !_boot.attempted) {
        _bmiEvidenceEntered=true;
        _bmiEvidenceRequestLength=requestLength;
        _bmiEvidenceCondition=request && (requestLength==4 && Ath10k::htcReadLE16(static_cast<const UInt8 *>(request))==8 && static_cast<const UInt8 *>(request)[2]==0 && static_cast<const UInt8 *>(request)[3]==0);
        if(request && requestLength==4){
            const auto *bytes=static_cast<const UInt8 *>(request);
            _bmiEvidenceCommand=static_cast<UInt32>(bytes[0]) | (static_cast<UInt32>(bytes[1])<<8) |
                (static_cast<UInt32>(bytes[2])<<16) | (static_cast<UInt32>(bytes[3])<<24);
        }
    }
    const UInt32 txCE = Ath10k::kBMICENumToTarget;
    const UInt32 rxCE = Ath10k::kBMICENumToHost;

    if (!request || requestLength == 0 || requestLength > Ath10k::kBMIMaxDataSize ||
        !response || responseLength == 0 || responseLength > Ath10k::kBMIResponseBufferSize ||
        _bmiTransferUncertain || _dmaQuarantine || _boot.bmiClosed || _boot.rxOwned ||
        !_cePipesProgrammed || !_wakeTargetCPUSucceeded ||
        !_ceSrcRings[txCE].allocated() || !_ceDstRings[rxCE].allocated()) {
        IOLog("Ath10kMac: BMI exchange ERROR invalid/not-ready req=%u resp-cap=%u\n",
              requestLength, responseLength);
        return false;
    }

    if (!_bmiRequestBuffer.allocated() &&
        !_bmiRequestBuffer.allocate(Ath10k::kBMIMaxDataSize,
                                    Ath10k::kCEDescriptorRingAlign, _dmaMapper)) {
        IOLog("Ath10kMac: BMI exchange ERROR request DMA allocation failed\n");
        return false;
    }
    if (!_bmiResponseBuffer.allocated() &&
        !_bmiResponseBuffer.allocate(Ath10k::kBMIResponseBufferSize,
                                     Ath10k::kCEDescriptorRingAlign, _dmaMapper)) {
        IOLog("Ath10kMac: BMI exchange ERROR response DMA allocation failed\n");
        return false;
    }

    Ath10k::CESnapshot txBefore{};
    Ath10k::CESnapshot rxBefore{};
    _bmiPipeValidationAttempted = true;
    const bool txRead = Ath10k::readCESnapshot(_regs, txCE, txBefore);
    const bool rxRead = Ath10k::readCESnapshot(_regs, rxCE, rxBefore);
    const bool strictTx = txRead && verifyCopyEnginePipe(txCE, txBefore);
    const bool strictRx = rxRead && verifyCopyEnginePipe(rxCE, rxBefore);
    const bool structuralTx = txRead && validateBMIPipeStructure(txCE, txBefore);
    const bool structuralRx = rxRead && validateBMIPipeStructure(rxCE, rxBefore);

    _bmiPipeStrictValidationMismatch = !(strictTx && strictRx);
    _bmiPipeStructuralValidationSucceeded = structuralTx && structuralRx;

    if (!_fwReadbackInProgress)
        ATH10K_TRACE("Ath10kMac: BMI CE0 post-wake snapshot structural=%s strict=%s "
          "src=%08x/%u dst=%08x/%u ctrl1=%08x hostIE=%08x miscIE=%08x "
          "wm=%08x/%08x idx=%u/%u,%u/%u\n",
          structuralTx ? "PASS" : "FAIL", strictTx ? "PASS" : "MISMATCH",
          txBefore.srcBase, txBefore.srcSize, txBefore.dstBase, txBefore.dstSize,
          txBefore.ctrl1, txBefore.hostIE, txBefore.miscIE,
          txBefore.srcWatermark, txBefore.dstWatermark,
          txBefore.srcWriteIndex, txBefore.currentSrcRead,
          txBefore.dstWriteIndex, txBefore.currentDstRead);
    if (!_fwReadbackInProgress)
        ATH10K_TRACE("Ath10kMac: BMI CE1 post-wake snapshot structural=%s strict=%s "
          "src=%08x/%u dst=%08x/%u ctrl1=%08x hostIE=%08x miscIE=%08x "
          "wm=%08x/%08x idx=%u/%u,%u/%u\n",
          structuralRx ? "PASS" : "FAIL", strictRx ? "PASS" : "MISMATCH",
          rxBefore.srcBase, rxBefore.srcSize, rxBefore.dstBase, rxBefore.dstSize,
          rxBefore.ctrl1, rxBefore.hostIE, rxBefore.miscIE,
          rxBefore.srcWatermark, rxBefore.dstWatermark,
          rxBefore.srcWriteIndex, rxBefore.currentSrcRead,
          rxBefore.dstWriteIndex, rxBefore.currentDstRead);

    if (!structuralTx || !structuralRx) {
        IOLog("Ath10kMac: BMI exchange ERROR CE0/CE1 structural validation failed; no DMA submitted\n");
        return false;
    }
    if (_bmiPipeStrictValidationMismatch && !_fwReadbackInProgress) {
        ATH10K_TRACE("Ath10kMac: BMI CE0/CE1 NOTE strict diagnostic fields changed after target wake; "
              "continuing because DMA-critical structure is intact (Linux-parity policy)\n");
    }

    const UInt32 txEntries = Ath10k::kCEHostConfig[txCE].srcEntries;
    const UInt32 rxEntries = Ath10k::kCEHostConfig[rxCE].dstEntries;
    if (txBefore.srcWriteIndex >= txEntries || txBefore.currentSrcRead >= txEntries ||
        rxBefore.dstWriteIndex >= rxEntries || rxBefore.currentDstRead >= rxEntries ||
        txBefore.srcWriteIndex != txBefore.currentSrcRead ||
        rxBefore.dstWriteIndex != rxBefore.currentDstRead) {
        IOLog("Ath10kMac: BMI exchange ERROR CE0/CE1 not idle CE0=%u/%u CE1=%u/%u\n",
              txBefore.srcWriteIndex, txBefore.currentSrcRead,
              rxBefore.dstWriteIndex, rxBefore.currentDstRead);
        return false;
    }

    bzero(_bmiRequestBuffer.virtualAddress(), static_cast<size_t>(_bmiRequestBuffer.length()));
    bcopy(request, _bmiRequestBuffer.virtualAddress(), requestLength);
    bzero(_bmiResponseBuffer.virtualAddress(), static_cast<size_t>(_bmiResponseBuffer.length()));
    OSSynchronizeIO();
    if (!_bmiRequestBuffer.syncForDevice() || !_bmiResponseBuffer.syncForDevice()) {
        IOLog("Ath10kMac: BMI exchange ERROR request/response syncForDevice failed\n");
        return false;
    }

    auto *txRing = reinterpret_cast<Ath10k::CEDescriptor32 *>(
        _ceSrcRings[txCE].virtualAddress());
    auto *rxRing = reinterpret_cast<Ath10k::CEDescriptor32 *>(
        _ceDstRings[rxCE].virtualAddress());
    const UInt32 txSlot = txBefore.srcWriteIndex;
    const UInt32 rxSlot = rxBefore.dstWriteIndex;
    const UInt32 txNext = Ath10k::ceRingNext(txSlot, txEntries);
    const UInt32 rxNext = Ath10k::ceRingNext(rxSlot, rxEntries);
    auto *txDesc = &txRing[txSlot];
    auto *rxDesc = &rxRing[rxSlot];
    bzero(txDesc, sizeof(*txDesc));
    bzero(rxDesc, sizeof(*rxDesc));

    if(_bmiEvidenceCondition && _deviceId==Ath10k::kQCA9377Device){
        captureBMIROMEvidence(true);
        // CE7 may have posted a diagnostic DMA before failing. Never start a
        // BMI transaction if ownership of that transfer became uncertain.
        if(_ceDiagTransferUncertain || _dmaQuarantine)return false;
    }

    // Linux ath10k_pci_hif_exchange_bmi_msg() posts CE1 RX first, then CE0 TX.
    rxDesc->address = static_cast<UInt32>(_bmiResponseBuffer.dmaAddress());
    rxDesc->nbytes = 0;
    rxDesc->flags = 0;
    if (!_ceDstRings[rxCE].syncForDevice()) {
        IOLog("Ath10kMac: BMI exchange ERROR CE1 destination ring syncForDevice failed\n");
        return false;
    }

    const UInt32 rxBase = Ath10k::ceBaseAddress(rxCE);
    // A posted RX buffer is device-owned even if the following TX fails.
    _bmiTransferUncertain = true;
    if (!_regs.write32(rxBase + Ath10k::kCEDstWriteIndex, rxNext) ||
        !_regs.read32(rxBase + Ath10k::kCEDstWriteIndex, _bmiCE1DstWrite) ||
        _bmiCE1DstWrite != rxNext) {
        IOLog("Ath10kMac: BMI exchange ERROR CE1 destination post failed readback=%u expected=%u\n",
              _bmiCE1DstWrite, rxNext);
        return false;
    }

    txDesc->address = static_cast<UInt32>(_bmiRequestBuffer.dmaAddress());
    txDesc->nbytes = static_cast<UInt16>(requestLength);
    txDesc->flags = Ath10k::kQCA6174CEDescriptorMetadataAnonymous;
    OSSynchronizeIO();
    if (!_ceSrcRings[txCE].syncForDevice()) {
        IOLog("Ath10kMac: BMI exchange ERROR CE0 source ring syncForDevice failed after CE1 post\n");
        return false;
    }

    const UInt32 txBase = Ath10k::ceBaseAddress(txCE);
    if (!_fwReadbackInProgress)
        ATH10K_TRACE("Ath10kMac: BMI exchange SUBMIT CE1 dst-slot=%u next=%u resp-iovm=0x%08x; CE0 src-slot=%u next=%u req-iovm=0x%08x req-bytes=%u flags=0x%04x\n",
          rxSlot, rxNext, static_cast<UInt32>(_bmiResponseBuffer.dmaAddress()),
          txSlot, txNext, static_cast<UInt32>(_bmiRequestBuffer.dmaAddress()),
          requestLength, txDesc->flags);

    // GET_TARGET_INFO contains no credentials. Observe only; never clear W1C
    // status, retry DMA, or interpret a failed read as a valid register value.
    if(requestLength==4 && Ath10k::htcReadLE16(static_cast<const UInt8 *>(request))==8 && static_cast<const UInt8 *>(request)[2]==0 && static_cast<const UInt8 *>(request)[3]==0){
        _bmiEvidenceCaptured[0]=true;
        UInt32 *descEvidence=_bmiEvidenceDescriptor[0];
        descEvidence[0]=txDesc->address;descEvidence[1]=txDesc->nbytes;descEvidence[2]=txDesc->flags;
        descEvidence[3]=rxDesc->address;descEvidence[4]=rxDesc->nbytes;descEvidence[5]=rxDesc->flags;
        const auto *wire=static_cast<const UInt8 *>(_bmiRequestBuffer.virtualAddress());
        ATH10K_TRACE("Ath10kMac: BMI EVIDENCE pre-submit command=%02x %02x %02x %02x TX-desc addr=0x%08x bytes=%u flags=0x%04x RX-desc addr=0x%08x bytes=%u flags=0x%04x\n",
            wire[0],wire[1],wire[2],wire[3],txDesc->address,txDesc->nbytes,txDesc->flags,
            rxDesc->address,rxDesc->nbytes,rxDesc->flags);
        for(UInt32 ce=0;ce<2;++ce){
            Ath10k::CESnapshot snap{};const bool valid=Ath10k::readCESnapshot(_regs,ce,snap);
            UInt32 hostStatus=0xffffffffu;const bool statusRead=_regs.read32(Ath10k::ceBaseAddress(ce)+Ath10k::kCEHostIS,hostStatus);
            _bmiMiscStatus[0][ce]=0xffffffffu;
            _bmiMiscRead[0][ce]=_regs.read32(Ath10k::ceBaseAddress(ce)+Ath10k::kCEMiscIS,_bmiMiscStatus[0][ce]);
            ATH10K_TRACE("Ath10kMac: BMI CE-fault pre-submit CE%u MISC_IS=0x%08x read=%s\n",ce,_bmiMiscStatus[0][ce],_bmiMiscRead[0][ce]?"yes":"no");
            UInt32 *ceEvidence=_bmiEvidenceCE[0][ce];
            ceEvidence[0]=valid?1u:0u;
            ceEvidence[1]=statusRead?1u:0u;
            ceEvidence[2]=snap.srcBase;
            ceEvidence[3]=snap.srcSize;
            ceEvidence[4]=snap.dstBase;
            ceEvidence[5]=snap.dstSize;
            ceEvidence[6]=snap.ctrl1;
            ceEvidence[7]=snap.command;
            ceEvidence[8]=snap.hostIE;
            ceEvidence[9]=hostStatus;
            ceEvidence[10]=snap.miscIE;
            ceEvidence[11]=snap.srcWatermark;
            ceEvidence[12]=snap.dstWatermark;
            ceEvidence[13]=snap.srcWriteIndex;
            ceEvidence[14]=snap.currentSrcRead;
            ceEvidence[15]=snap.dstWriteIndex;
            ceEvidence[16]=snap.currentDstRead;
            ceEvidence[17]=0u;
            ATH10K_TRACE("Ath10kMac: BMI EVIDENCE pre-submit CE%u valid=%s src=0x%08x/%u dst=0x%08x/%u ctrl1=0x%08x swap-src=%u swap-dst=%u command=0x%08x hostIE=0x%08x hostIS=0x%08x status-read=%s miscIE=0x%08x wm=0x%08x/0x%08x idx=%u/%u,%u/%u\n",
                ce,valid?"yes":"no",snap.srcBase,snap.srcSize,snap.dstBase,snap.dstSize,snap.ctrl1,
                !!(snap.ctrl1&Ath10k::kCECtrl1SrcByteSwapMask),!!(snap.ctrl1&Ath10k::kCECtrl1DstByteSwapMask),
                snap.command,snap.hostIE,hostStatus,statusRead?"yes":"no",snap.miscIE,snap.srcWatermark,snap.dstWatermark,
                snap.srcWriteIndex,snap.currentSrcRead,snap.dstWriteIndex,snap.currentDstRead);
        }
    }
    if (!_regs.write32(txBase + Ath10k::kCESrcWriteIndex, txNext)) {
        IOLog("Ath10kMac: BMI exchange ERROR CE0 source submit failed\n");
        return false;
    }
    _bmiTransferUncertain = true;
    OSSynchronizeIO();
    if (!_regs.read32(txBase + Ath10k::kCESrcWriteIndex, _bmiCE0SrcWrite) ||
        _bmiCE0SrcWrite != txNext) {
        IOLog("Ath10kMac: BMI exchange ERROR CE0 source write-index did not latch; DMA state UNCERTAIN\n");
        return false;
    }

    UInt32 waitedUs = 0;
    bool txDone = false;
    bool rxDone = false;
    UInt16 completedBytes = 0;
    UInt32 srcRead = 0xffffffffu;
    UInt32 dstRead = 0xffffffffu;

    while (waitedUs <= Ath10k::kBMICommunicationTimeoutUs) {
        OSSynchronizeIO();
        if (!_regs.read32(txBase + Ath10k::kCECurrentSrcRead, srcRead) ||
            !_regs.read32(rxBase + Ath10k::kCECurrentDstRead, dstRead)) {
            IOLog("Ath10kMac: BMI exchange ERROR cannot poll CE0/CE1 read indices; DMA state UNCERTAIN\n");
            return false;
        }
        if (!_ceDstRings[rxCE].syncFromDevice()) {
            IOLog("Ath10kMac: BMI exchange ERROR CE1 destination ring syncFromDevice failed; DMA state UNCERTAIN\n");
            return false;
        }

        completedBytes = rxDesc->nbytes;
        txDone = ((srcRead & (txEntries - 1u)) == txNext);

        // Linux ath10k's completed receive path treats a non-zero nbytes in
        // the destination descriptor as the completion condition. It does not
        // require DRRI to have advanced first; in fact the CE code explicitly
        // handles ordering races between the index and descriptor update.
        // Keep DRRI as telemetry, but don't make it a false-negative gate.
        rxDone = completedBytes != 0;
        if (txDone && rxDone)
            break;

        if (waitedUs == Ath10k::kBMICommunicationTimeoutUs)
            break;
        const UInt32 step = (Ath10k::kBMICommunicationTimeoutUs - waitedUs < Ath10k::kBMIPollIntervalUs)
            ? (Ath10k::kBMICommunicationTimeoutUs - waitedUs)
            : Ath10k::kBMIPollIntervalUs;
        if (step == 0)
            break;
        if (step >= 1000 && (step % 1000u) == 0)
            IOSleep(step / 1000u);
        else
            IODelay(step);
        waitedUs += step;
    }

    _bmiWaitUs = waitedUs;
    _bmiCE0SrcRead = srcRead;
    _bmiCE1DstRead = dstRead;
    _bmiCompletedBytes = completedBytes;

    if (!txDone || !rxDone) {
        _bmiTimedOut = true;
        // GET_TARGET_INFO contains no credentials. Observe only; never clear W1C
        // status, retry DMA, or interpret a failed read as a valid register value.
        if(requestLength==4 && Ath10k::htcReadLE16(static_cast<const UInt8 *>(request))==8 && static_cast<const UInt8 *>(request)[2]==0 && static_cast<const UInt8 *>(request)[3]==0){
            _bmiEvidenceCaptured[1]=true;
            UInt32 *descEvidence=_bmiEvidenceDescriptor[1];
            descEvidence[0]=txDesc->address;descEvidence[1]=txDesc->nbytes;descEvidence[2]=txDesc->flags;
            descEvidence[3]=rxDesc->address;descEvidence[4]=rxDesc->nbytes;descEvidence[5]=rxDesc->flags;
            const auto *wire=static_cast<const UInt8 *>(_bmiRequestBuffer.virtualAddress());
            IOLog("Ath10kMac: BMI EVIDENCE timeout command=%02x %02x %02x %02x TX-desc addr=0x%08x bytes=%u flags=0x%04x RX-desc addr=0x%08x bytes=%u flags=0x%04x\n",
                wire[0],wire[1],wire[2],wire[3],txDesc->address,txDesc->nbytes,txDesc->flags,
                rxDesc->address,rxDesc->nbytes,rxDesc->flags);
            for(UInt32 ce=0;ce<2;++ce){
                Ath10k::CESnapshot snap{};const bool valid=Ath10k::readCESnapshot(_regs,ce,snap);
                UInt32 hostStatus=0xffffffffu;const bool statusRead=_regs.read32(Ath10k::ceBaseAddress(ce)+Ath10k::kCEHostIS,hostStatus);
                _bmiMiscStatus[1][ce]=0xffffffffu;
                _bmiMiscRead[1][ce]=_regs.read32(Ath10k::ceBaseAddress(ce)+Ath10k::kCEMiscIS,_bmiMiscStatus[1][ce]);
                IOLog("Ath10kMac: BMI CE-fault timeout CE%u MISC_IS=0x%08x read=%s\n",ce,_bmiMiscStatus[1][ce],_bmiMiscRead[1][ce]?"yes":"no");
                UInt32 *ceEvidence=_bmiEvidenceCE[1][ce];
                ceEvidence[0]=valid?1u:0u;
                ceEvidence[1]=statusRead?1u:0u;
                ceEvidence[2]=snap.srcBase;
                ceEvidence[3]=snap.srcSize;
                ceEvidence[4]=snap.dstBase;
                ceEvidence[5]=snap.dstSize;
                ceEvidence[6]=snap.ctrl1;
                ceEvidence[7]=snap.command;
                ceEvidence[8]=snap.hostIE;
                ceEvidence[9]=hostStatus;
                ceEvidence[10]=snap.miscIE;
                ceEvidence[11]=snap.srcWatermark;
                ceEvidence[12]=snap.dstWatermark;
                ceEvidence[13]=snap.srcWriteIndex;
                ceEvidence[14]=snap.currentSrcRead;
                ceEvidence[15]=snap.dstWriteIndex;
                ceEvidence[16]=snap.currentDstRead;
                ceEvidence[17]=0u;
                IOLog("Ath10kMac: BMI EVIDENCE timeout CE%u valid=%s src=0x%08x/%u dst=0x%08x/%u ctrl1=0x%08x swap-src=%u swap-dst=%u command=0x%08x hostIE=0x%08x hostIS=0x%08x status-read=%s miscIE=0x%08x wm=0x%08x/0x%08x idx=%u/%u,%u/%u\n",
                    ce,valid?"yes":"no",snap.srcBase,snap.srcSize,snap.dstBase,snap.dstSize,snap.ctrl1,
                    !!(snap.ctrl1&Ath10k::kCECtrl1SrcByteSwapMask),!!(snap.ctrl1&Ath10k::kCECtrl1DstByteSwapMask),
                    snap.command,snap.hostIE,hostStatus,statusRead?"yes":"no",snap.miscIE,snap.srcWatermark,snap.dstWatermark,
                    snap.srcWriteIndex,snap.currentSrcRead,snap.dstWriteIndex,snap.currentDstRead);
            }
        }

        // Read-only evidence: never retry a command with uncertain DMA ownership.
        const auto &layout=Ath10k::kQca6174RegisterLayout;
        const bool wakeRead=_regs.read32(Ath10k::socWakeOffset(layout),_bmiWakeAtTimeout);
        const bool rtcRead=_regs.read32(Ath10k::rtcStateOffset(layout),_bmiRTCAtTimeout);
        IOLog("Ath10kMac: BMI timeout wake-evidence SOC_WAKE=0x%08x read=%s RTC_STATE=0x%08x read=%s awake=%s\n",
            _bmiWakeAtTimeout,wakeRead?"yes":"no",_bmiRTCAtTimeout,rtcRead?"yes":"no",
            rtcRead && Ath10k::rtcStateValue(_bmiRTCAtTimeout)==layout.rtcStateValueOn?"yes":"no");
        IOLog("Ath10kMac: BMI exchange TIMEOUT waited=%u us CE0 src-w/r=%u/%u expected=%u CE1 dst-w/r=%u/%u posted=%u completed=%u; DMA state UNCERTAIN\n",
              waitedUs, _bmiCE0SrcWrite, srcRead, txNext,
              _bmiCE1DstWrite, dstRead, rxNext, completedBytes);
        if(_bmiEvidenceCondition) captureBMIROMEvidence();
        return false;
    }

    // Hardware consumed both descriptors. From here the transaction is no
    // longer in flight; data-integrity failures can be handled by reset cleanup.
    _bmiTransferUncertain = false;

    if (completedBytes > responseLength || completedBytes > _bmiResponseBuffer.length()) {
        IOLog("Ath10kMac: BMI exchange ERROR response too large completed=%u capacity=%u\n",
              completedBytes, responseLength);
        return false;
    }
    if (!_bmiResponseBuffer.syncFromDevice()) {
        IOLog("Ath10kMac: BMI exchange ERROR response syncFromDevice failed after completion\n");
        return false;
    }

    bcopy(_bmiResponseBuffer.virtualAddress(), response, completedBytes);
    responseLength = completedBytes;

    // Clear completion metadata before any later ring reuse.
    txDesc->nbytes = 0;
    rxDesc->nbytes = 0;
    OSSynchronizeIO();
    if (!_ceSrcRings[txCE].syncForDevice() || !_ceDstRings[rxCE].syncForDevice()) {
        IOLog("Ath10kMac: BMI exchange ERROR descriptor cleanup sync failed after safe completion\n");
        return false;
    }

    if (!_fwReadbackInProgress)
        ATH10K_TRACE("Ath10kMac: BMI exchange COMPLETE req=%u resp=%u waited=%u us CE0 src-r=%u CE1 dst-r=%u\n",
          requestLength, responseLength, waitedUs, srcRead, dstRead);
    return true;
}

// Diagnostic only: separate CE7 buffers; CE0/CE1 ownership remains uncertain.
// Read only the bounded ROM descriptor and its 12-byte metadata buffer, with
// confirmation reads. Never retry CE7 after failure, alter a BMI
// doorbell, reset a ring, or reinterpret these bytes as CPU execution proof.
void Ath10kPCIDevice::captureBMIROMEvidence(bool beforeSubmit)
{
    if(beforeSubmit){
        _bmiROMPreStatus=1;_bmiROMPreStable=false;
        if(_deviceId!=Ath10k::kQCA9377Device || !_cePipesProgrammed || _ceDiagTransferUncertain || _dmaQuarantine || _boot.attempted)return;
        Ath10k::CESnapshot snap{};UInt32 core=0;
        const auto &layout=Ath10k::kQca6174RegisterLayout;
        if(!Ath10k::readCESnapshot(_regs,0,snap) || snap.dstSize!=32 || snap.currentDstRead>=32 ||
           !_regs.read32(Ath10k::socCoreOffset(layout,Ath10k::kCoreCtrlAddress),core) || core==0xffffffffu)return;
        const UInt32 window=(core&0x7ffu)<<21;
        auto cpuAddress=[window](UInt32 address,UInt32 bytes,UInt32 &cpu)->bool {
            if(address>=0x00400000u && address<0x00500000u)cpu=address;
            else if((address&0xfff00000u)==(window|0x00100000u))cpu=0x00400000u|(address&0x000fffffu);
            else return false;
            return !(cpu&3u) && bytes<=0x00500000u-cpu;
        };
        UInt32 ring=0;if(!cpuAddress(snap.dstBase,32u*8u,ring)){_bmiROMPreStatus=2;return;}
        _bmiROMPreDescriptorCPU=ring+snap.currentDstRead*8u;
        UInt8 desc[8]{},confirm[8]{},words[12]{},again[12]{};
        _bmiROMPreStatus=3;if(!ce7DiagnosticRead(_bmiROMPreDescriptorCPU,desc,sizeof(desc)))return;
        const UInt32 address=Ath10k::connLE32(desc);
        if(!cpuAddress(address,sizeof(words),_bmiROMPreBufferCPU)){_bmiROMPreStatus=2;return;}
        if(!ce7DiagnosticRead(_bmiROMPreBufferCPU,words,sizeof(words)) ||
           !ce7DiagnosticRead(_bmiROMPreDescriptorCPU,confirm,sizeof(confirm)) ||
           !ce7DiagnosticRead(_bmiROMPreBufferCPU,again,sizeof(again)))return;
        Ath10k::CESnapshot after{};
        _bmiROMPreStable=!memcmp(desc,confirm,sizeof(desc)) && !memcmp(words,again,sizeof(words)) &&
            Ath10k::readCESnapshot(_regs,0,after) && after.dstBase==snap.dstBase && after.dstSize==32 && after.currentDstRead==snap.currentDstRead;
        for(UInt32 i=0;i<3;++i)_bmiROMPreWords[i]=Ath10k::connLE32(words+4*i);
        _bmiROMPreStatus=_bmiROMPreStable?4:5;
        ATH10K_TRACE("Ath10kMac: BMI ROM pre-submit status=%u desc-cpu=0x%08x buffer-cpu=0x%08x words=%08x/%08x/%08x stable=%s; no BMI submitted yet\n",
            _bmiROMPreStatus,_bmiROMPreDescriptorCPU,_bmiROMPreBufferCPU,_bmiROMPreWords[0],_bmiROMPreWords[1],_bmiROMPreWords[2],_bmiROMPreStable?"yes":"no");
        return;
    }
    _bmiROMStatus=1; // skipped unless every eligibility check passes
    if(_deviceId!=Ath10k::kQCA9377Device || !_bmiTimedOut ||
       !_bmiEvidenceCaptured[0] || !_bmiEvidenceCaptured[1] ||
       !_cePipesProgrammed || _ceDiagTransferUncertain || _dmaQuarantine || _boot.attempted)
        return;
    const UInt32 *pre=_bmiEvidenceCE[0][0];
    const UInt32 *now=_bmiEvidenceCE[1][0];
    if(!pre[0] || !now[0] || now[5]!=32 || pre[5]!=32 || now[4]!=pre[4] ||
       pre[16]>=32 || now[16]>=32 || now[16]!=((pre[16]+1u)&31u) ||
       now[13]!=now[14] || _bmiEvidenceDescriptor[1][4]!=0)
        return;
    const auto &layout=Ath10k::kQca6174RegisterLayout;
    _bmiROMCoreRead=_regs.read32(Ath10k::socCoreOffset(layout,Ath10k::kCoreCtrlAddress),_bmiROMCore);
    _bmiROMFWRead=_regs.read32(layout.fwIndicatorAddress,_bmiROMFW);
    if(!_bmiROMCoreRead || _bmiROMCore==0xffffffffu){_bmiROMStatus=2;return;}
    const UInt32 window=(_bmiROMCore & 0x7ffu)<<21;
    // Only the directly accessible DRAM alias documented by Linux hw.c.
    // No CPU_ADDR_MSB programming or arbitrary target pointer traversal.
    auto cpuAddress=[window](UInt32 address, UInt32 bytes, UInt32 &cpu)->bool {
        if(address>=0x00400000u && address<0x00500000u) cpu=address;
        else if((address & 0xfff00000u)==(window|0x00100000u))
            cpu=0x00400000u|(address & 0x000fffffu);
        else return false;
        return (cpu&3u)==0 && bytes<=0x00500000u-cpu;
    };
    UInt32 ringCPU=0;
    if(!cpuAddress(now[4],32u*8u,ringCPU)){_bmiROMStatus=3;return;}
    _bmiROMSlot=(now[16]+31u)&31u;
    _bmiROMDescriptorCPU=ringCPU+_bmiROMSlot*8u;
    UInt8 descriptor[8]{},confirm[8]{},word[12]{},wordConfirm[12]{};
    ++_bmiROMReadCount;
    if(!ce7DiagnosticRead(_bmiROMDescriptorCPU,descriptor,sizeof(descriptor))){_bmiROMStatus=4;return;}
    auto le32=[](const UInt8 *p)->UInt32 {
        return static_cast<UInt32>(p[0])|(static_cast<UInt32>(p[1])<<8)|
               (static_cast<UInt32>(p[2])<<16)|(static_cast<UInt32>(p[3])<<24);
    };
    _bmiROMDescriptorAddress=le32(descriptor);
    _bmiROMDescriptorBytes=Ath10k::htcReadLE16(descriptor+4);
    _bmiROMDescriptorFlags=Ath10k::htcReadLE16(descriptor+6);
    if(_bmiROMDescriptorBytes!=4 || !cpuAddress(_bmiROMDescriptorAddress,sizeof(word),_bmiROMBufferCPU)){
        _bmiROMStatus=5;return;
    }
    ++_bmiROMReadCount;
    if(!ce7DiagnosticRead(_bmiROMBufferCPU,word,sizeof(word))){_bmiROMStatus=6;return;}
    _bmiROMWord=le32(word);_bmiROMVersion=le32(word+4);_bmiROMType=le32(word+8);
    ++_bmiROMReadCount;
    if(!ce7DiagnosticRead(_bmiROMDescriptorCPU,confirm,sizeof(confirm))){_bmiROMStatus=7;return;}
    ++_bmiROMReadCount;
    if(!ce7DiagnosticRead(_bmiROMBufferCPU,wordConfirm,sizeof(wordConfirm))){_bmiROMStatus=7;return;}
    Ath10k::CESnapshot after{};
    _bmiROMStable=memcmp(word,wordConfirm,sizeof(word))==0 && Ath10k::readCESnapshot(_regs,0,after) &&
        after.dstBase==now[4] && after.dstSize==now[5] &&
        after.currentDstRead==now[16] && after.currentSrcRead==now[14] &&
        memcmp(descriptor,confirm,sizeof(descriptor))==0;
    _bmiROMResponseCandidate=_bmiROMStable && _bmiROMWord==12 && _bmiROMVersion==0x05020001 && _bmiROMType==8;
    const UInt32 *pre1=_bmiEvidenceCE[0][1],*now1=_bmiEvidenceCE[1][1];
    UInt32 sourceCPU=0;
    if(_bmiROMStable && pre1[0] && now1[0] && pre1[2]==now1[2] && pre1[3]==32 && now1[3]==32 &&
       pre1[13]==now1[13] && pre1[14]==now1[14] && now1[13]<32 && now1[13]==now1[14] &&
       cpuAddress(now1[2],32*8,sourceCPU)){
        _bmiROMCE1DescriptorCPU=sourceCPU+now1[13]*8;
        UInt8 source[8]{},sourceConfirm[8]{};
        ++_bmiROMReadCount;
        if(!ce7DiagnosticRead(_bmiROMCE1DescriptorCPU,source,sizeof(source))){_bmiROMStatus=9;return;}
        _bmiROMCE1Address=le32(source);_bmiROMCE1Bytes=Ath10k::htcReadLE16(source+4);_bmiROMCE1Flags=Ath10k::htcReadLE16(source+6);
        ++_bmiROMReadCount;
        if(!ce7DiagnosticRead(_bmiROMCE1DescriptorCPU,sourceConfirm,sizeof(sourceConfirm))){_bmiROMStatus=9;return;}
        Ath10k::CESnapshot after1{};
        _bmiROMCE1Stable=memcmp(source,sourceConfirm,sizeof(source))==0 && Ath10k::readCESnapshot(_regs,1,after1) &&
            after1.srcBase==now1[2] && after1.srcSize==32 && after1.srcWriteIndex==now1[13] && after1.currentSrcRead==now1[14];
    }
    _bmiROMStatus=_bmiROMStable?8u:7u;
    const bool matched=_bmiROMPreStable && _bmiROMPreDescriptorCPU==_bmiROMDescriptorCPU && _bmiROMPreBufferCPU==_bmiROMBufferCPU;
    ATH10K_TRACE("Ath10kMac: BMI ROM before/after comparable=%s changed=%s; response candidate remains diagnostic only\n",
        matched?"yes":"no",matched && (_bmiROMPreWords[0]!=_bmiROMWord || _bmiROMPreWords[1]!=_bmiROMVersion || _bmiROMPreWords[2]!=_bmiROMType)?"yes":"no");
    IOLog("Ath10kMac: BMI ROM response candidate=%s length=%u version=0x%08x type=%u CE1-slot-cpu=0x%08x addr=0x%08x bytes=%u flags=0x%04x stable=%s; BMI remains failed\n",
        _bmiROMResponseCandidate?"yes":"no",_bmiROMWord,_bmiROMVersion,_bmiROMType,_bmiROMCE1DescriptorCPU,
        _bmiROMCE1Address,_bmiROMCE1Bytes,_bmiROMCE1Flags,_bmiROMCE1Stable?"yes":"no");
    ATH10K_TRACE("Ath10kMac: BMI ROM evidence status=%u desc-cpu=0x%08x slot=%u buffer-cpu=0x%08x bytes=%u word=0x%08x stable=%s; diagnostic read only\n",
        _bmiROMStatus,_bmiROMDescriptorCPU,_bmiROMSlot,_bmiROMBufferCPU,
        _bmiROMDescriptorBytes,_bmiROMWord,_bmiROMStable?"yes":"no");
}

bool Ath10kPCIDevice::runBMIGetTargetInfoTest()
{
    _bmiAttempted = true;
    _bmiSucceeded = false;
    _bmiGetTargetInfoAttempted = true;
    _bmiGetTargetInfoSucceeded = false;
    _bmiPipeValidationAttempted = false;
    _bmiPipeStructuralValidationSucceeded = false;
    _bmiPipeStrictValidationMismatch = false;
    _bmiTransferUncertain = false;
    _bmiTimedOut = false;
    _bmiRequestLength = sizeof(UInt32);
    _bmiResponseLength = 0;
    _bmiTargetInfoLength = 0xffffffffu;
    _bmiTargetVersion = 0xffffffffu;
    _bmiTargetType = 0xffffffffu;
    _bmiWaitUs = 0;
    _bmiCompletedBytes = 0;
    _bmiWakeAtTimeout = _bmiRTCAtTimeout = 0xffffffffu;

    _bmiROMStatus=0;_bmiROMCore=_bmiROMFW=_bmiROMWord=0xffffffffu;
    _bmiROMVersion=_bmiROMType=0xffffffffu;_bmiROMResponseCandidate=_bmiROMCE1Stable=false;
    _bmiROMCE1DescriptorCPU=_bmiROMCE1Address=_bmiROMCE1Bytes=_bmiROMCE1Flags=0;
    _bmiROMCoreRead=_bmiROMFWRead=_bmiROMStable=false;
    _bmiROMDescriptorCPU=_bmiROMBufferCPU=_bmiROMDescriptorAddress=0;
    _bmiROMDescriptorBytes=_bmiROMDescriptorFlags=_bmiROMSlot=_bmiROMReadCount=0;
    _bmiEvidenceEntered=_bmiEvidenceCondition=false;
    _bmiEvidenceRequestLength=0;_bmiEvidenceCommand=0xffffffffu;
    bzero(_bmiEvidenceCaptured,sizeof(_bmiEvidenceCaptured));
    bzero(_bmiEvidenceDescriptor,sizeof(_bmiEvidenceDescriptor));
    bzero(_bmiEvidenceCE,sizeof(_bmiEvidenceCE));
    bzero(_bmiMiscStatus,sizeof(_bmiMiscStatus));bzero(_bmiMiscRead,sizeof(_bmiMiscRead));
    _bmiROMPreStatus=_bmiROMPreDescriptorCPU=_bmiROMPreBufferCPU=0;_bmiROMPreStable=false;
    bzero(_bmiROMPreWords,sizeof(_bmiROMPreWords));

    UInt32 command = Ath10k::kBMIGetTargetInfo;
    Ath10k::BMIGetTargetInfoResponse response{};
    UInt32 responseLength = sizeof(response);

    IOLog("Ath10kMac: BMI_GET_TARGET_INFO begin command=%u req-bytes=%u resp-cap=%u CE0=H2T CE1=T2H timeout=%u us\n",
          command, (UInt32)sizeof(command), responseLength,
          Ath10k::kBMICommunicationTimeoutUs);

    if (!bmiExchange(&command, sizeof(command), &response, responseLength)) {
        IOLog("Ath10kMac: BMI_GET_TARGET_INFO ERROR transport failed\n");
        return false;
    }

    _bmiResponseLength = responseLength;
    if (responseLength < sizeof(response)) {
        IOLog("Ath10kMac: BMI_GET_TARGET_INFO ERROR short response=%u expected-at-least=%lu\n",
              responseLength, (unsigned long)sizeof(response));
        return false;
    }

    _bmiTargetInfoLength = response.length;
    _bmiTargetVersion = response.version;
    _bmiTargetType = response.type;
    if(response.length!=sizeof(response) || response.type!=8 || !response.version || response.version==0xffffffffu ||
       (payload().available && !payloadMatchesTarget())){
        IOLog("Ath10kMac: BMI_GET_TARGET_INFO rejected ROM identity len=%u version=0x%08x type=%u; board/OTP/firmware remain blocked\n",
              response.length,response.version,response.type);
        return false;
    }
    _bmiGetTargetInfoSucceeded = true;
    _bmiSucceeded = true;
    ATH10K_TRACE("Ath10kMac: BMI_GET_TARGET_INFO COMPLETE response-bytes=%u target-len=0x%08x version=0x%08x type=0x%08x\n",
          responseLength, _bmiTargetInfoLength, _bmiTargetVersion, _bmiTargetType);
    return true;
}

bool Ath10kPCIDevice::bmiReadSOCRegister(UInt32 address,UInt32 &value)
{
    if(!_bmiGetTargetInfoSucceeded || _boot.bmiClosed || (address&3))return false;
    const Ath10k::BMIReadSOCRequest request{Ath10k::kBMIReadSOCRegister,address};
    UInt32 bytes=sizeof(value);value=0;
    return bmiExchange(&request,sizeof(request),&value,bytes) && bytes==sizeof(value);
}
bool Ath10kPCIDevice::bmiWriteSOCRegister(UInt32 address,UInt32 value)
{
    if(!_bmiGetTargetInfoSucceeded || !_bmiBoardPrereqSucceeded || _boot.bmiClosed || (address&3))return false;
    const Ath10k::BMIWriteSOCRequest request{Ath10k::kBMIWriteSOCRegister,address,value};
    return bmiSendOnly(&request,sizeof(request));
}
bool Ath10kPCIDevice::bmiWritePLLWord(UInt32 address,UInt32 value)
{
    // Linux qca6174_regs: three fixed RAM words, written before board upload.
    // bmiWriteMemory deliberately remains restricted to board RAM/init flag.
    const bool profile=_selectedHw && _selectedHw->targetCpuFrequencyHz==176000000 &&
        ((_selectedHw->deviceId==Ath10k::kQCA6174Device && _selectedHw->chipRevision==Ath10k::kQCA6174Hw32ChipRev) ||
         (_selectedHw->deviceId==Ath10k::kQCA9377Device && _selectedHw->chipRevision==Ath10k::kQCA9377Hw11ChipRev));
    if(!profile || !_bmiGetTargetInfoSucceeded || !_bmiBoardPrereqSucceeded ||
       !payloadMatchesTarget() || _boot.bmiClosed || _bmiTransferUncertain || _dmaQuarantine ||
       !_pll.attempted || _pll.complete)return false;
    const bool parameter=(_pll.step==5 && address==0x404028 && value==1) ||
        (_pll.step==12 && address==0x404020 && value==1) ||
        (_pll.step==13 && address==0x404024 && value==_selectedHw->targetCpuFrequencyHz);
    if(!parameter)return false;
    Ath10k::BMIWriteMemoryRequest request{};UInt32 consumed=0;
    const UInt32 bytes=Ath10k::buildBMIWriteRequest(request,address,
        reinterpret_cast<const UInt8 *>(&value),sizeof(value),consumed);
    if(bytes!=16 || consumed!=sizeof(value) || !bmiSendOnly(&request,bytes))return false;
    ++_bmiWriteMemoryTransactionCount;
    ATH10K_TRACE("Ath10kMac: PLL BMI_WRITE_MEMORY complete addr=0x%08x value=0x%08x step=%u\n",address,value,_pll.step);
    return true;
}
bool Ath10kPCIDevice::configureTargetPLL()
{
    if(!_selectedHw || !_bmiGetTargetInfoSucceeded || !_bmiBoardPrereqSucceeded)return false;
    const bool ok=Ath10k::configurePLL(_pll,_selectedHw->targetCpuFrequencyHz,
        [this](UInt32 a,UInt32 &v){return bmiReadSOCRegister(a,v);},
        [this](UInt32 a,UInt32 v){return bmiWriteSOCRegister(a,v);},
        [this](UInt32 a,UInt32 v){return bmiWritePLLWord(a,v);},
        [](UInt32 us){IODelay(us);},[]()->UInt64{uint64_t t=0,n=0;clock_get_uptime(&t);absolutetime_to_nanoseconds(t,&n);return n/1000;});
    ATH10K_TRACE("Ath10kMac: PLL %s profile=%s ref=%u ref-hz=%u step=%u polls=%u\n",
        _pll.status,_selectedHw->targetRevision,_pll.reference,_pll.referenceHz,_pll.step,_pll.polls);
    return ok;
}
bool Ath10kPCIDevice::bmiReadMemory(UInt32 targetAddress, void *outData, UInt32 length)
{
    _bmiReadMemorySucceeded = false;
    if (!outData || !Ath10k::bmiMemoryRangeValid(targetAddress, length) ||
        !_bmiGetTargetInfoSucceeded || _bmiTransferUncertain || _dmaQuarantine) {
        IOLog("Ath10kMac: BMI_READ_MEMORY ERROR invalid/not-ready addr=0x%08x len=%u\n",
              targetAddress, length);
        return false;
    }

    _bmiReadMemoryAttempted = true;
    auto *out = reinterpret_cast<UInt8 *>(outData);
    UInt32 remaining = length;
    UInt32 address = targetAddress;

    while (remaining != 0) {
        const UInt32 chunk = remaining > Ath10k::kBMIMaxDataSize
            ? Ath10k::kBMIMaxDataSize : remaining;
        Ath10k::BMIReadMemoryRequest request{};
        request.id = Ath10k::kBMIReadMemory;
        request.address = address;
        request.length = chunk;

        UInt8 response[Ath10k::kBMIMaxDataSize]{};
        UInt32 responseLength = chunk;

        if (!_fwReadbackInProgress)
            ATH10K_TRACE("Ath10kMac: BMI_READ_MEMORY begin addr=0x%08x bytes=%u transaction=%u\n",
              address, chunk, _bmiReadMemoryTransactionCount + 1u);
        if (!bmiExchange(&request, sizeof(request), response, responseLength)) {
            IOLog("Ath10kMac: BMI_READ_MEMORY ERROR transport addr=0x%08x bytes=%u\n",
                  address, chunk);
            return false;
        }
        if (responseLength != chunk) {
            IOLog("Ath10kMac: BMI_READ_MEMORY ERROR short/long response addr=0x%08x got=%u expected=%u\n",
                  address, responseLength, chunk);
            return false;
        }

        bcopy(response, out, chunk);
        ++_bmiReadMemoryTransactionCount;
        if (!_fwReadbackInProgress)
            ATH10K_TRACE("Ath10kMac: BMI_READ_MEMORY COMPLETE addr=0x%08x bytes=%u transaction=%u\n",
              address, chunk, _bmiReadMemoryTransactionCount);

        address += chunk;
        out += chunk;
        remaining -= chunk;
    }

    _bmiReadMemorySucceeded = true;
    return true;
}

bool Ath10kPCIDevice::bmiSendOnly(const void *request, UInt32 requestLength)
{
    // WRITE_MEMORY/LZ/DONE have no BMI response. A dedicated CE1 buffer is
    // already posted only for DONE, to catch the firmware's first HTC packet.
    const UInt32 ce = Ath10k::kBMICENumToTarget;
    if (!request || requestLength == 0 || requestLength > Ath10k::kBMIMaxDataSize ||
        !_bmiGetTargetInfoSucceeded || !_bmiBoardPrereqSucceeded ||
        !_cePipesProgrammed || !_wakeTargetCPUSucceeded ||
        _bmiTransferUncertain || _dmaQuarantine || _boot.bmiClosed ||
        !_bmiRequestBuffer.allocated() || !_ceSrcRings[ce].allocated())
        return false;
    if (requestLength < sizeof(UInt32))
        return false;
    UInt32 command = 0;
    bcopy(request, &command, sizeof(command));
    const bool isDone = command == Ath10k::kBMIDone;
    if ((_boot.rxOwned && !isDone) ||
        (isDone && (requestLength != sizeof(UInt32) || !_boot.attempted ||
                    !_boot.preDoneHIVerified || !_boot.rxPosted || !_boot.rxOwned ||
                    _boot.doneAttempted)))
        return false;

    Ath10k::CESnapshot before{};
    const UInt32 entries = Ath10k::kCEHostConfig[ce].srcEntries;
    if (!Ath10k::readCESnapshot(_regs, ce, before) ||
        !validateBMIPipeStructure(ce, before) ||
        before.srcWriteIndex >= entries || before.currentSrcRead >= entries ||
        before.srcWriteIndex != before.currentSrcRead) {
        IOLog("Ath10kMac: BMI TX-only ERROR CE0 structure/idle validation failed\n");
        return false;
    }
    bzero(_bmiRequestBuffer.virtualAddress(), static_cast<size_t>(_bmiRequestBuffer.length()));
    bcopy(request, _bmiRequestBuffer.virtualAddress(), requestLength);
    if (!_bmiRequestBuffer.syncForDevice())
        return false;
    auto *ring = reinterpret_cast<Ath10k::CEDescriptor32 *>(_ceSrcRings[ce].virtualAddress());
    auto *desc = &ring[before.srcWriteIndex];
    bzero(desc, sizeof(*desc));
    desc->address = static_cast<UInt32>(_bmiRequestBuffer.dmaAddress());
    desc->nbytes = static_cast<UInt16>(requestLength);
    desc->flags = Ath10k::kQCA6174CEDescriptorMetadataAnonymous;
    OSSynchronizeIO();
    if (!_ceSrcRings[ce].syncForDevice())
        return false;

    const UInt32 next = Ath10k::ceRingNext(before.srcWriteIndex, entries);
    const UInt32 base = Ath10k::ceBaseAddress(ce);
    if (isDone) {
        // Close BMI before attempting the doorbell. Even an MMIO failure or
        // a timeout must never permit DONE (or any other BMI command) again.
        _boot.bmiClosed = true;
        _boot.doneAttempted = true;
    }
    // Conservatively retain DMA ownership before attempting the doorbell.
    _bmiTransferUncertain = true;
    if (!_regs.write32(base + Ath10k::kCESrcWriteIndex, next) ||
        !_regs.read32(base + Ath10k::kCESrcWriteIndex, _bmiCE0SrcWrite) ||
        _bmiCE0SrcWrite != next)
        return false;

    UInt32 waited = 0;
    for (;;) {
        OSSynchronizeIO();
        if (!_regs.read32(base + Ath10k::kCECurrentSrcRead, _bmiCE0SrcRead))
            return false;
        if (_bmiCE0SrcRead >= entries)
            return false;
        if (_bmiCE0SrcRead == next)
            break;
        if (waited >= Ath10k::kBMICommunicationTimeoutUs) {
            _bmiTimedOut = true;
            _bmiWaitUs = waited;
            IOLog("Ath10kMac: BMI TX-only TIMEOUT src=%u/%u; reset required\n",
                  _bmiCE0SrcRead, next);
            return false;
        }
        IOSleep(1);
        waited += 1000;
    }
    _bmiTransferUncertain = false;
    _bmiWaitUs = waited;
    if (isDone)
        _boot.doneSent = true;
    desc->nbytes = 0;
    OSSynchronizeIO();
    if (!_ceSrcRings[ce].syncForDevice())
        return false;
    // CE consumption proves transport completion; the caller must read back
    // target RAM to prove that the ROM applied the write.
    return true;
}

bool Ath10kPCIDevice::bmiWriteMemory(UInt32 targetAddress, const void *data, UInt32 length)
{
    if (!data || !_bmiBoardUploadAttempted || !_bmiBoardPrereqSucceeded ||
        (targetAddress & 3u) || !Ath10k::bmiMemoryRangeValid(targetAddress, length))
        return false;
    // This milestone only permits the selected board RAM and its initialized
    // flag. Check the padded interval as well, not only the input byte count.
    const UInt64 end = static_cast<UInt64>(targetAddress) + ((static_cast<UInt64>(length) + 3u) & ~3ull);
    const bool boardRange = targetAddress >= _bmiBoardDataAddress &&
        end <= static_cast<UInt64>(_bmiBoardDataAddress) + payload().kBoardSize;
    const bool initFlag = targetAddress == Ath10k::kHostInterestBoardDataInitAddress && length == 4;
    if (!boardRange && !initFlag)
        return false;
    const auto *bytes = reinterpret_cast<const UInt8 *>(data);
    UInt32 offset = 0;
    while (offset < length) {
        Ath10k::BMIWriteMemoryRequest request{};
        UInt32 consumed = 0;
        const UInt32 wireSize = Ath10k::buildBMIWriteRequest(
            request, targetAddress + offset, bytes + offset, length - offset, consumed);
        if (!wireSize || !bmiSendOnly(&request, wireSize)) {
            IOLog("Ath10kMac: BMI_WRITE_MEMORY ERROR addr=0x%08x offset=%u\n", targetAddress, offset);
            return false;
        }
        ++_bmiWriteMemoryTransactionCount;
        if (boardRange)
            _bmiBoardBytesWritten += consumed;
        offset += consumed;
    }
    ATH10K_TRACE("Ath10kMac: BMI_WRITE_MEMORY COMPLETE addr=0x%08x bytes=%u\n", targetAddress, length);
    return true;
}

bool Ath10kPCIDevice::runBMIBoardUploadTest()
{
    _bmiBoardUploadStatus = "profile-mismatch";
    // Payload selection requires an allowlisted PCI subsystem, chip and ROM.
    // The 9377 API1 fallback is explicit; never substitute the 6174 board.
    if (!payloadMatchesTarget()) {
        IOLog("Ath10kMac: board upload BLOCKED: payload/profile mismatch\n");
        return false;
    }
    _bmiBoardUploadStatus = "prerequisite-mismatch";
    if (!_bmiBoardPrereqSucceeded || !payload().boardAddressValid(_bmiBoardDataAddress) ||
        _bmiBoardDataInitialized != 0 || _bmiBoardExtDataAddress != 0 ||
        _bmiBoardExtDataConfig != 0 || _bmiCalDataAddress != 0) {
        IOLog("Ath10kMac: board upload BLOCKED: prerequisite/RAM layout outside selected profile\n");
        return false;
    }
    _bmiBoardUploadAttempted = true;
    _bmiBoardUploadStatus = "write-failed";
    ATH10K_TRACE("Ath10kMac: board upload BEGIN key=%s bytes=%u sha256=%s\n",
          payload().kBoardKey, payload().kBoardSize, payload().kBoardSHA256);
    ATH10K_TRACE("Ath10kMac: board selection=%s profile-device=%04x\n", payload().kBoardSelection, payload().device);
    if (!bmiWriteMemory(_bmiBoardDataAddress, payload().kBoard, payload().kBoardSize))
        return false;

    _bmiBoardUploadStatus = "readback-failed";
    for (UInt32 offset = 0; offset < payload().kBoardSize;) {
        UInt8 buffer[Ath10k::kBMIMaxDataSize]{};
        UInt32 count = static_cast<UInt32>(payload().kBoardSize) - offset;
        if (count > sizeof(buffer))
            count = sizeof(buffer);
        if (!bmiReadMemory(_bmiBoardDataAddress + offset, buffer, count))
            return false;
        if (memcmp(buffer, payload().kBoard + offset, count) != 0) {
            ATH10K_TRACE("Ath10kMac: board readback MISMATCH offset=%u bytes=%u\n", offset, count);
            return false;
        }
        offset += count;
        _bmiBoardBytesVerified = offset;
    }
    _bmiBoardReadbackSucceeded = true;
    _bmiBoardUploadStatus = "init-flag-failed";
    const UInt32 initialized = 1;
    UInt32 observed = 0;
    if (!bmiWriteMemory(Ath10k::kHostInterestBoardDataInitAddress, &initialized, sizeof(initialized)) ||
        !bmiReadMemory(Ath10k::kHostInterestBoardDataInitAddress, &observed, sizeof(observed)) ||
        observed != 1)
        return false;
    _bmiBoardInitVerified = true;
    _bmiBoardUploadSucceeded = true;
    _bmiBoardUploadStatus = "verified-before-reset";
    ATH10K_TRACE("Ath10kMac: board upload VERIFIED bytes=%u init=1 writes=%u; OTP stage may now follow\n",
          _bmiBoardBytesVerified, _bmiWriteMemoryTransactionCount);
    return true;
}

bool Ath10kPCIDevice::runBMIBoardPrerequisiteProbe()
{
    _bmiBoardPrereqAttempted = true;
    _bmiBoardPrereqSucceeded = false;
    _bmiReadMemoryAttempted = false;
    _bmiReadMemorySucceeded = false;
    _bmiReadMemoryTransactionCount = 0;
    _bmiInterconnectMatchesDiag = false;
    _bmiEarlyCfgDoneSeen = false;
    _bmiBoardDataPointerValid = false;
    _bmiBoardExtDataValid = false;
    _bmiDesiredCPUSpeed = 0xffffffffu;
    _bmiBoardDataAddress = 0xffffffffu;
    _bmiBoardDataInitialized = 0xffffffffu;
    _bmiBoardExtDataAddress = 0xffffffffu;
    _bmiBoardExtDataConfig = 0xffffffffu;
    _bmiBoardExtDataSize = 0;
    _bmiCalDataAddress = 0xffffffffu;
    _bmiInterconnectState = 0xffffffffu;
    _bmiOptionFlag2 = 0xffffffffu;
    for (UInt32 &word : _bmiBoardFingerprint)
        word = 0xffffffffu;

    ATH10K_TRACE("Ath10kMac: BMI board/cal prerequisite probe begin policy=READ-ONLY no OTP/board/firmware writes\n");

    struct Read32Item {
        UInt32 address;
        UInt32 *value;
        const char *name;
    };

    Read32Item items[] = {
        {Ath10k::kHostInterestDesiredCPUSpeedAddress, &_bmiDesiredCPUSpeed, "hi_desired_cpu_speed_hz"},
        {Ath10k::kHostInterestBoardDataAddress, &_bmiBoardDataAddress, "hi_board_data"},
        {Ath10k::kHostInterestBoardDataInitAddress, &_bmiBoardDataInitialized, "hi_board_data_initialized"},
        {Ath10k::kHostInterestBoardExtDataAddress, &_bmiBoardExtDataAddress, "hi_board_ext_data"},
        {Ath10k::kHostInterestBoardExtConfigAddress, &_bmiBoardExtDataConfig, "hi_board_ext_data_config"},
        {Ath10k::kHostInterestCalDataAddress, &_bmiCalDataAddress, "hi_cal_data"},
        {Ath10k::kHostInterestInterconnectAddress, &_bmiInterconnectState, "hi_interconnect_state"},
        {Ath10k::kHostInterestOptionFlag2Address, &_bmiOptionFlag2, "hi_option_flag2"},
    };

    for (const auto &item : items) {
        if (!bmiReadMemory(item.address, item.value, sizeof(UInt32))) {
            IOLog("Ath10kMac: BMI board/cal prerequisite ERROR cannot read %s @0x%08x\n",
                  item.name, item.address);
            return false;
        }
        ATH10K_TRACE("Ath10kMac: BMI Host Interest %s @0x%08x = 0x%08x\n",
              item.name, item.address, *item.value);
    }

    _bmiBoardExtDataValid =
        (_bmiBoardExtDataConfig & Ath10k::kHostInterestBoardExtDataValidMask) != 0;
    _bmiBoardExtDataSize =
        (_bmiBoardExtDataConfig & Ath10k::kHostInterestBoardExtDataSizeMask) >>
        Ath10k::kHostInterestBoardExtDataSizeShift;
    _bmiInterconnectMatchesDiag =
        _ceDiagPCIeStateAddress != 0xffffffffu &&
        _bmiInterconnectState == _ceDiagPCIeStateAddress;
    _bmiEarlyCfgDoneSeen = (_bmiOptionFlag2 & Ath10k::kHIOptionEarlyCfgDone) != 0;
    _bmiBoardDataPointerValid =
        _bmiBoardDataAddress != 0 && _bmiBoardDataAddress != 0xffffffffu &&
        (_bmiBoardDataAddress & 0x3u) == 0;

    ATH10K_TRACE("Ath10kMac: BMI prerequisites parsed cpu=%u board=0x%08x initialized=%u board-ext=0x%08x ext-config=0x%08x ext-valid=%s ext-size=%u cal=0x%08x\n",
          _bmiDesiredCPUSpeed, _bmiBoardDataAddress, _bmiBoardDataInitialized,
          _bmiBoardExtDataAddress, _bmiBoardExtDataConfig,
          _bmiBoardExtDataValid ? "yes" : "no", _bmiBoardExtDataSize,
          _bmiCalDataAddress);
    ATH10K_TRACE("Ath10kMac: BMI prerequisite cross-check interconnect=0x%08x diag-pcie-state=0x%08x match=%s option-flag2=0x%08x EARLY_CFG_DONE=%s board-pointer=%s\n",
          _bmiInterconnectState, _ceDiagPCIeStateAddress,
          _bmiInterconnectMatchesDiag ? "yes" : "no", _bmiOptionFlag2,
          _bmiEarlyCfgDoneSeen ? "yes" : "no",
          _bmiBoardDataPointerValid ? "valid" : "invalid");

    if (_bmiBoardDataPointerValid) {
        if (!bmiReadMemory(_bmiBoardDataAddress, _bmiBoardFingerprint,
                           sizeof(_bmiBoardFingerprint))) {
            IOLog("Ath10kMac: BMI board/cal prerequisite ERROR board target-area fingerprint read failed\n");
            return false;
        }
        ATH10K_TRACE("Ath10kMac: BMI board target-area fingerprint addr=0x%08x words=%08x %08x %08x %08x\n",
              _bmiBoardDataAddress,
              _bmiBoardFingerprint[0], _bmiBoardFingerprint[1],
              _bmiBoardFingerprint[2], _bmiBoardFingerprint[3]);
    }

    ATH10K_TRACE("Ath10kMac: board candidate bus=pci,vendor=%04x,device=%04x,subsystem-vendor=%04x,subsystem-device=%04x\n",
          _vendorId, _deviceId, _subsystemVendorId, _subsystemId);
    if (_selectedHw) {
        ATH10K_TRACE("Ath10kMac: firmware prerequisites profile=%s dir=%s cal-len=%u target-cpu=%u diag-ce=%s board-api2=board-2.bin board-fallback=board.bin\n",
              _selectedHw->targetRevision, _selectedHw->firmwareDirectory,
              _selectedHw->calibrationDataLength, _selectedHw->targetCpuFrequencyHz,
              _selectedHw->firmwareDiagCeDownload ? "yes" : "no");
    }

    const bool ready = _bmiReadMemorySucceeded && _bmiBoardDataPointerValid &&
                       _bmiInterconnectMatchesDiag && _bmiEarlyCfgDoneSeen;
    _bmiBoardPrereqSucceeded = ready;
    ATH10K_TRACE("Ath10kMac: BMI board/cal prerequisite probe %s reads=%u board=0x%08x board-init=%u interconnect-match=%s early-cfg=%s\n",
          ready ? "READY" : "INCOMPLETE", _bmiReadMemoryTransactionCount,
          _bmiBoardDataAddress, _bmiBoardDataInitialized,
          _bmiInterconnectMatchesDiag ? "yes" : "no",
          _bmiEarlyCfgDoneSeen ? "yes" : "no");
    return ready;
}

bool Ath10kPCIDevice::bmiLZStreamStart(UInt32 address)
{
    if (!_otpAttempted || !_bmiBoardUploadSucceeded || !_bmiBoardInitVerified ||
        _otpStreamClosed || _otpExecuteAttempted)
        return false;
    if (address == Ath10k::kQCA6174OTPLoadAddress) {
        if (_otpStreamStarted)
            return false;
    } else if (address == 0) {
        if (!_otpStreamStarted || _otpWireBytesSent != ((payload().kOTPSize + 3u) & ~3u))
            return false;
    } else {
        return false;
    }
    const Ath10k::BMILZStreamStartRequest request{Ath10k::kBMILZStreamStart, address};
    if (!bmiSendOnly(&request, sizeof(request)))
        return false;
    if (address == 0)
        _otpStreamClosed = true;
    else
        _otpStreamStarted = true;
    ATH10K_TRACE("Ath10kMac: OTP LZ_STREAM_START COMPLETE address=0x%08x flush=%s\n",
          address, address == 0 ? "yes" : "no");
    return true;
}

bool Ath10kPCIDevice::bmiLZData(const UInt8 *data, UInt32 length)
{
    const UInt32 paddedSize = (payload().kOTPSize + 3u) & ~3u;
    if (!data || length == 0 || (length & 3u) || !_otpStreamStarted ||
        _otpStreamClosed || _otpExecuteAttempted || _otpWireBytesSent > paddedSize ||
        length > paddedSize - _otpWireBytesSent)
        return false;
    UInt32 offset = 0;
    while (offset < length) {
        Ath10k::BMILZDataRequest request{};
        UInt32 consumed = 0;
        const UInt32 wireSize = Ath10k::buildBMILZDataRequest(request, data + offset,
                                                          length - offset, consumed);
        if (!wireSize || !bmiSendOnly(&request, wireSize)) {
            IOLog("Ath10kMac: OTP LZ_DATA FAILED sent=%u command=%u\n",
                  _otpWireBytesSent, _otpLZDataCommands + 1);
            return false;
        }
        offset += consumed;
        _otpWireBytesSent += consumed;
        ++_otpLZDataCommands;
    }
    ATH10K_TRACE("Ath10kMac: OTP LZ_DATA COMPLETE wire-bytes=%u commands=%u\n",
          _otpWireBytesSent, _otpLZDataCommands);
    return true;
}

bool Ath10kPCIDevice::runBMIOtpTest()
{
    if (_otpAttempted)
        return false; // No retry or second execution within the same bring-up.
    _otpStatus = "prerequisite-mismatch";
    if (!_bmiBoardUploadSucceeded || !_bmiBoardReadbackSucceeded || !_bmiBoardInitVerified ||
        !_bmiBoardPrereqSucceeded || _bmiTransferUncertain || _dmaQuarantine ||
        !payloadMatchesTarget() ||
        payload().kOTPSize == 0) {
        IOLog("Ath10kMac: OTP BLOCKED: verified board/profile required\n");
        return false;
    }
    _otpAttempted = true;
    _otpStatus = "lz-start-failed";
    ATH10K_TRACE("Ath10kMac: OTP BEGIN bytes=%u sha256=%s load=0x%08x execute-param=0x%08x ignore-result-feature=%s\n",
          payload().kOTPSize, payload().kOTPSHA256,
          Ath10k::kQCA6174OTPLoadAddress, Ath10k::kQCA6174OTPExecuteParam,
          payload().kOTPIgnoreResult ? "yes" : "no");
    if (!bmiLZStreamStart(Ath10k::kQCA6174OTPLoadAddress))
        return false;

    // Match Linux ath10k_bmi_fast_download: aligned head, a separate zero-
    // padded trailer, then START(0) to finish the stream and flush caches.
    const UInt32 head = payload().kOTPSize & ~3u;
    const UInt32 tail = payload().kOTPSize - head;
    _otpStatus = "lz-data-failed";
    if (head && !bmiLZData(payload().kOTP, head))
        return false;
    if (tail) {
        UInt8 trailer[4]{};
        bcopy(payload().kOTP + head, trailer, tail);
        if (!bmiLZData(trailer, sizeof(trailer)))
            return false;
    }
    _otpStatus = "lz-flush-failed";
    if (!bmiLZStreamStart(0))
        return false;
    _otpDownloadSucceeded = true;

    const Ath10k::BMIExecuteRequest request{Ath10k::kBMIExecute,
        Ath10k::kQCA6174OTPLoadAddress, Ath10k::kQCA6174OTPExecuteParam};
    UInt32 result = 0xffffffffu;
    UInt32 responseBytes = sizeof(result);
    _otpExecuteAttempted = true;
    _otpStatus = "execute-transport-failed";
    ATH10K_TRACE("Ath10kMac: OTP BMI_EXECUTE BEGIN addr=0x%08x param=0x%08x\n", request.address, request.parameter);
    if (!bmiExchange(&request, sizeof(request), &result, responseBytes))
        return false;
    _otpExecuteResponseBytes = responseBytes;
    if (responseBytes != sizeof(result)) {
        _otpStatus = "execute-response-size-invalid";
        IOLog("Ath10kMac: OTP BMI_EXECUTE invalid response bytes=%u\n", responseBytes);
        return false;
    }
    _otpExecuteSucceeded = true; // Transport/ABI completion, not calibration proof.
    _otpResult = result;
    _otpResultAccepted = Ath10k::otpResultAccepted(result, payload().kOTPIgnoreResult);
    _otpResultIgnored = result != 0 && payload().kOTPIgnoreResult;
    _otpStatus = !_otpResultAccepted ? "otp-result-rejected" :
        (_otpResultIgnored ? "returned-nonzero-accepted-by-firmware" : "returned-zero");
    ATH10K_TRACE("Ath10kMac: OTP BMI_EXECUTE COMPLETE result=0x%08x accepted=%s nonzero-ignored=%s status=%s\n",
          result, _otpResultAccepted ? "yes" : "no",
          _otpResultIgnored ? "yes" : "no", _otpStatus);
    return _otpResultAccepted;
}

bool Ath10kPCIDevice::bmiFirmwareLZStart(UInt32 address)
{
    if (!_fwAttempted || !_otpDownloadSucceeded || !_otpExecuteSucceeded ||
        !_otpResultAccepted || _fwStreamClosed)
        return false;
    if (address == Ath10k::kQCA6174FirmwareLoadAddress) {
        if (_fwStreamStarted)
            return false;
    } else if (address == 0) {
        if (!_fwStreamStarted || _fwWireBytesSent != ((payload().kFirmwareSize + 3u) & ~3u))
            return false;
    } else {
        return false;
    }
    const Ath10k::BMILZStreamStartRequest request{Ath10k::kBMILZStreamStart, address};
    if (!bmiSendOnly(&request, sizeof(request)))
        return false;
    if (address == 0)
        _fwStreamClosed = true;
    else
        _fwStreamStarted = true;
    ATH10K_TRACE("Ath10kMac: WLAN LZ_STREAM_START COMPLETE address=0x%08x flush=%s\n",
          address, address == 0 ? "yes" : "no");
    return true;
}

bool Ath10kPCIDevice::bmiFirmwareLZData(const UInt8 *data, UInt32 length)
{
    const UInt32 paddedSize = (payload().kFirmwareSize + 3u) & ~3u;
    if (!data || length == 0 || (length & 3u) || !_fwStreamStarted ||
        _fwStreamClosed || _fwWireBytesSent > paddedSize ||
        length > paddedSize - _fwWireBytesSent)
        return false;
    UInt32 offset = 0;
    while (offset < length) {
        Ath10k::BMILZDataRequest request{};
        UInt32 consumed = 0;
        const UInt32 wireSize = Ath10k::buildBMILZDataRequest(request, data + offset,
                                                           length - offset, consumed);
        if (!wireSize || !bmiSendOnly(&request, wireSize)) {
            IOLog("Ath10kMac: WLAN LZ_DATA FAILED sent=%u command=%u\n",
                  _fwWireBytesSent, _fwLZDataCommands + 1);
            return false;
        }
        offset += consumed;
        _fwWireBytesSent += consumed;
        ++_fwLZDataCommands;
        if ((_fwLZDataCommands % 256u) == 0)
            ATH10K_TRACE("Ath10kMac: WLAN LZ progress wire-bytes=%u/%u commands=%u\n",
                  _fwWireBytesSent, paddedSize, _fwLZDataCommands);
    }
    ATH10K_TRACE("Ath10kMac: WLAN LZ_DATA COMPLETE wire-bytes=%u commands=%u\n",
          _fwWireBytesSent, _fwLZDataCommands);
    return true;
}

bool Ath10kPCIDevice::verifyFirmwareSegments()
{
    _fwReadbackAttempted = true;
    for (UInt32 index = 0; index < payload().kFirmwareSegmentCount; ++index) {
        const auto &segment = payload().kFirmwareSegments[index];
        _fwFailedSegment = index;
        if (!segment.length || (segment.address & 3u) ||
            segment.offset > payload().kFirmwareSize ||
            segment.length > payload().kFirmwareSize - segment.offset ||
            static_cast<UInt64>(segment.address) + ((static_cast<UInt64>(segment.length) + 3u) & ~3ull) > 0x100000000ull)
            return false;
        ATH10K_TRACE("Ath10kMac: WLAN readback BEGIN segment=%u addr=0x%08x bytes=%u\n",
              index, segment.address, segment.length);
        for (UInt32 offset = 0; offset < segment.length;) {
            UInt8 buffer[Ath10k::kBMIMaxDataSize]{};
            UInt32 count = segment.length - offset;
            if (count > sizeof(buffer))
                count = sizeof(buffer);
            // Read whole words; compare only the actual segment bytes. The
            // final SGMT segment has an odd length, and its RAM padding is
            // unspecified. Never compare the padding with a guessed value.
            const UInt32 readSize = (count + 3u) & ~3u;
            if (!bmiReadMemory(segment.address + offset, buffer, readSize))
                return false;
            const UInt8 *expected = payload().kFirmware + segment.offset + offset;
            if (memcmp(buffer, expected, count) != 0) {
                UInt32 differing = 0;
                while (differing < count && buffer[differing] == expected[differing])
                    ++differing;
                _fwMismatchAddress = segment.address + offset + differing;
                ATH10K_TRACE("Ath10kMac: WLAN readback MISMATCH segment=%u addr=0x%08x expected=%02x actual=%02x\n",
                      index, _fwMismatchAddress, expected[differing], buffer[differing]);
                return false;
            }
            offset += count;
            _fwReadbackBytes += count;
        }
        ++_fwVerifiedSegments;
        ATH10K_TRACE("Ath10kMac: WLAN readback VERIFIED segment=%u bytes=%u total=%u\n",
              index, segment.length, _fwReadbackBytes);
    }
    _fwFailedSegment = 0xffffffffu;
    _fwReadbackSucceeded = _fwVerifiedSegments == payload().kFirmwareSegmentCount &&
        _fwReadbackBytes == payload().kFirmwareSegmentDataBytes;
    return _fwReadbackSucceeded;
}

bool Ath10kPCIDevice::runBMIFirmwareDownloadTest()
{
    if (_fwAttempted)
        return false; // No retry of a partially downloaded image.
    _fwStatus = "prerequisite-mismatch";
    if (!_bmiBoardUploadSucceeded || !_bmiBoardReadbackSucceeded || !_bmiBoardInitVerified ||
        !_otpDownloadSucceeded || !_otpStreamClosed || !_otpExecuteSucceeded || !_otpResultAccepted ||
        _bmiTransferUncertain || _ceDiagTransferUncertain || _dmaQuarantine ||
        !payloadMatchesTarget()) {
        IOLog("Ath10kMac: WLAN download BLOCKED: board/OTP/profile prerequisites\n");
        return false;
    }
    _fwAttempted = true;
    _fwStatus = "lz-start-failed";
    ATH10K_TRACE("Ath10kMac: WLAN BEGIN version=%s bytes=%u sha256=%s path=BMI-LZ otp-result=0x%08x otp-ignored=%s\n",
          payload().kFirmwareVersion, payload().kFirmwareSize,
          payload().kFirmwareSHA256, _otpResult, _otpResultIgnored ? "yes" : "no");
    // Linux's core.c also supports BMI LZ on hw3.2 (the fallback used by
    // ath10k_download_fw). Reuse the transport validated by the OTP milestone.
    if (!bmiFirmwareLZStart(Ath10k::kQCA6174FirmwareLoadAddress))
        return false;
    const UInt32 head = payload().kFirmwareSize & ~3u;
    const UInt32 tail = payload().kFirmwareSize - head;
    _fwStatus = "lz-data-failed";
    if (head && !bmiFirmwareLZData(payload().kFirmware, head))
        return false;
    if (tail) {
        UInt8 trailer[4]{};
        bcopy(payload().kFirmware + head, trailer, tail);
        if (!bmiFirmwareLZData(trailer, sizeof(trailer)))
            return false;
    }
    _fwStatus = "lz-flush-failed";
    if (!bmiFirmwareLZStart(0))
        return false;
    _fwDownloadSucceeded = true;

    _fwStatus = "post-download-bmi-failed";
    const UInt32 command = Ath10k::kBMIGetTargetInfo;
    Ath10k::BMIGetTargetInfoResponse response{};
    UInt32 responseLength = sizeof(response);
    if (!bmiExchange(&command, sizeof(command), &response, responseLength))
        return false;
    _fwPostTargetVersion = response.version;
    _fwPostTargetType = response.type;
    _fwPostBMIProbeSucceeded = responseLength == sizeof(response) &&
        response.length == sizeof(response) && response.version == _bmiTargetVersion &&
        response.type == _bmiTargetType;
    if (!_fwPostBMIProbeSucceeded)
        return false;

    _fwStatus = "readback-failed";
    _fwReadbackInProgress = true;
    const bool readbackOK = verifyFirmwareSegments();
    _fwReadbackInProgress = false;
    if (!readbackOK)
        return false;
    _fwStatus = "download-verified-before-reset";
    ATH10K_TRACE("Ath10kMac: WLAN DOWNLOAD VERIFIED image=%u segment-data=%u segments=%u entry=0x%08x; BMI_DONE not sent\n",
          payload().kFirmwareSize, _fwReadbackBytes, _fwVerifiedSegments,
          payload().kFirmwareEntryAddress);
    return true;
}

bool Ath10kPCIDevice::verifyBootHIWord(UInt32 address, UInt32 value)
{
    if (!_boot.configAttempted || _boot.bmiClosed || _boot.rxOwned ||
        !Ath10k::isBootHIWord(address))
        return false;
    _boot.lastHIAddress = address;
    _boot.lastHIExpected = value;
    _boot.lastHIObserved = 0xffffffffu;
    if (!bmiReadMemory(address, &_boot.lastHIObserved, sizeof(UInt32)) ||
        _boot.lastHIObserved != value) {
        IOLog("Ath10kMac: BOOT HI VERIFY FAILED addr=0x%08x expected=0x%08x observed=0x%08x\n",
              address, value, _boot.lastHIObserved);
        return false;
    }
    ++_boot.hiVerified;
    return true;
}

bool Ath10kPCIDevice::verifyBootHIOptionsAfterDownload()
{
    if (!_boot.attempted || !_boot.configSucceeded || !_fwReadbackSucceeded ||
        _boot.bmiClosed || _boot.rxOwned)
        return false;
    _boot.optionFinalRead = false;
    _boot.optionFinalVerified = false;
    _boot.optionBMICreditLimitAdded = false;
    _boot.optionAfterDownload = 0xffffffffu;
    _boot.optionChangedBits = 0;
    _boot.lastHIAddress = Ath10k::kBootHIOption;
    _boot.lastHIExpected = _boot.optionApplied;
    _boot.lastHIObserved = 0xffffffffu;
    if (!bmiReadMemory(Ath10k::kBootHIOption, &_boot.lastHIObserved, sizeof(UInt32))) {
        IOLog("Ath10kMac: BOOT HI OPTION READ FAILED addr=0x%08x\n", Ath10k::kBootHIOption);
        return false;
    }
    _boot.optionFinalRead = true;
    _boot.optionAfterDownload = _boot.lastHIObserved;
    _boot.optionChangedBits = _boot.optionApplied ^ _boot.optionAfterDownload;
    if (!Ath10k::bootHIOptionMatchesAfterDownload(_boot.optionApplied, _boot.optionAfterDownload)) {
        IOLog("Ath10kMac: BOOT HI OPTION VERIFY FAILED applied=0x%08x observed=0x%08x changed=0x%08x allowed-add=0x%08x\n",
              _boot.optionApplied, _boot.optionAfterDownload, _boot.optionChangedBits,
              Ath10k::kBootHIOptionBMICreditLimit);
        return false;
    }
    _boot.optionFinalVerified = true;
    _boot.optionBMICreditLimitAdded = _boot.optionChangedBits == Ath10k::kBootHIOptionBMICreditLimit;
    ++_boot.hiVerified;
    ATH10K_TRACE("Ath10kMac: BOOT HI OPTION VERIFIED applied=0x%08x observed=0x%08x changed=0x%08x bmi-credit-limit-added=%s; target value preserved\n",
          _boot.optionApplied, _boot.optionAfterDownload, _boot.optionChangedBits,
          _boot.optionBMICreditLimitAdded ? "yes" : "no");
    return true;
}

bool Ath10kPCIDevice::writeBootHIWord(UInt32 address, UInt32 value)
{
    // Keep the board writer's range restriction intact. Boot configuration has
    // its own explicit word whitelist and cannot be used once CE1 is armed.
    if (!_boot.configAttempted || _boot.bmiClosed || _boot.rxOwned ||
        !_bmiBoardPrereqSucceeded || !Ath10k::isBootHIWord(address))
        return false;
    _boot.lastHIAddress = address;
    _boot.lastHIExpected = value;
    _boot.lastHIObserved = 0xffffffffu;
    Ath10k::BMIWriteMemoryRequest request{};
    UInt32 consumed = 0;
    const UInt32 wireBytes = Ath10k::buildBMIWriteRequest(
        request, address, reinterpret_cast<const UInt8 *>(&value), sizeof(value), consumed);
    if (wireBytes != 16 || consumed != 4 || !bmiSendOnly(&request, wireBytes))
        return false;
    ++_boot.hiWrites;
    ++_bmiWriteMemoryTransactionCount;
    if (!verifyBootHIWord(address, value))
        return false;
    ATH10K_TRACE("Ath10kMac: BOOT HI VERIFIED addr=0x%08x value=0x%08x\n", address, value);
    return true;
}

bool Ath10kPCIDevice::configureBootHostInterest()
{
    if (_boot.configAttempted)
        return false;
    _boot.status = "host-interest-prerequisite-mismatch";
    if (!_bmiGetTargetInfoSucceeded || !_bmiBoardPrereqSucceeded ||
        _bmiTransferUncertain || _ceDiagTransferUncertain || _dmaQuarantine ||
        !payloadMatchesTarget() ||
        !payload().boardAddressValid(_bmiBoardDataAddress) || _bmiBoardUploadAttempted)
        return false;
    _boot.configAttempted = true;
    _boot.status = "host-interest-config-failed";
    // Same order as ath10k_init_configure_target(), before board/OTP loading.
    if (!writeBootHIWord(Ath10k::kBootHIApp, Ath10k::kBootHTCProtocolVersion) ||
        !bmiReadMemory(Ath10k::kBootHIOption, &_boot.optionBefore, sizeof(UInt32)) ||
        _boot.optionBefore == 0xffffffffu)
        return false;
    _boot.optionApplied = _boot.optionBefore | Ath10k::kBootHIOptionBits;
    if (!writeBootHIWord(Ath10k::kBootHIOption, _boot.optionApplied) ||
        !writeBootHIWord(Ath10k::kBootHIHostBE, 0) ||
        !writeBootHIWord(Ath10k::kBootHIFWSwap, 0) ||
        !writeBootHIWord(Ath10k::kBootHIPCIId, _deviceId))
        return false;
    _boot.configSucceeded = true;
    _boot.status = "host-interest-configured";
    ATH10K_TRACE("Ath10kMac: BOOT HI CONFIGURED protocol=%u option=0x%08x->0x%08x pci-id=0x%04x\n",
          Ath10k::kBootHTCProtocolVersion, _boot.optionBefore, _boot.optionApplied, _deviceId);
    return true;
}

bool Ath10kPCIDevice::configureBootFirmwareDownload()
{
    if (!_boot.configSucceeded || _boot.downloadConfigAttempted ||
        !_otpExecuteSucceeded || !_otpResultAccepted || _fwAttempted ||
        _boot.bmiClosed || _bmiTransferUncertain || _dmaQuarantine)
        return false;
    _boot.downloadConfigAttempted = true;
    _boot.status = "skip-clock-config-failed";
    // Linux applies this only when firmware feature bit 9 is set. It is NOT
    // present in the shipped WLAN.RM.4.4.1-00309- container.
    if (payload().kSupportsSkipClockInit && !writeBootHIWord(Ath10k::kBootHISkipClock, 1))
        return false;
    _boot.downloadConfigSucceeded = true;
    _boot.status = "download-configured";
    return true;
}

bool Ath10kPCIDevice::finishBootHostInterest()
{
    if (!_boot.attempted || !_boot.configSucceeded || !_boot.downloadConfigSucceeded ||
        !_fwReadbackSucceeded || _boot.bmiClosed || _boot.rxOwned)
        return false;
    // ath10k_init_uart() disables UART output after firmware download.
    if (!writeBootHIWord(Ath10k::kBootHISerial, 0))
        return false;
    _boot.uartDisabled = true;
    if (!verifyBootHIWord(Ath10k::kBootHIApp, Ath10k::kBootHTCProtocolVersion) ||
        !verifyBootHIOptionsAfterDownload() ||
        !verifyBootHIWord(Ath10k::kBootHIHostBE, 0) ||
        !verifyBootHIWord(Ath10k::kBootHIFWSwap, 0) ||
        !verifyBootHIWord(Ath10k::kBootHIPCIId, _deviceId) ||
        (payload().kSupportsSkipClockInit && !verifyBootHIWord(Ath10k::kBootHISkipClock, 1)))
        return false;
    UInt32 board = 0, initialized = 0;
    if (!bmiReadMemory(Ath10k::kHostInterestBoardDataAddress, &board, sizeof(board)) ||
        !bmiReadMemory(Ath10k::kHostInterestBoardDataInitAddress, &initialized, sizeof(initialized)) ||
        board != _bmiBoardDataAddress || initialized != 1)
        return false;
    _boot.preDoneHIVerified = true;
    ATH10K_TRACE("Ath10kMac: BOOT HI FINAL VERIFIED writes=%u checks=%u UART=off board=0x%08x initialized=%u\n",
          _boot.hiWrites, _boot.hiVerified, board, initialized);
    return true;
}

bool Ath10kPCIDevice::prepareHTCReadyReceive()
{
    const UInt32 ce = Ath10k::kBMICENumToHost;
    const UInt32 entries = Ath10k::kCEHostConfig[ce].dstEntries;
    if (!_boot.attempted || !_boot.preDoneHIVerified || !_boot.uartDisabled ||
        _boot.rxOwned || _boot.rxPosted || _boot.bmiClosed ||
        _bmiTransferUncertain || _dmaQuarantine || !_cePipesProgrammed)
        return false;
    Ath10k::CESnapshot before{};
    if (!Ath10k::readCESnapshot(_regs, ce, before) ||
        !validateBMIPipeStructure(ce, before) ||
        (before.ctrl1 & Ath10k::kCECtrl1DmaxMask) != Ath10k::kHTCReadyBufferBytes ||
        before.dstWriteIndex >= entries || before.currentDstRead >= entries ||
        before.dstWriteIndex != before.currentDstRead)
        return false;
    if (!_regs.read32(Ath10k::kQca6174RegisterLayout.fwIndicatorAddress, _boot.fwIndicator) ||
        _boot.fwIndicator == 0xffffffffu)
        return false;
    if (_boot.fwIndicator & Ath10k::kFwIndicatorEventPending) {
        _boot.firmwareCrash = true;
        return false;
    }
    if (!_htcReadyBuffer.allocated() &&
        !_htcReadyBuffer.allocate(Ath10k::kHTCReadyBufferBytes,
                                 Ath10k::kCEDescriptorRingAlign, _dmaMapper))
        return false;
    if (_htcReadyBuffer.length() != Ath10k::kHTCReadyBufferBytes)
        return false;
    bzero(_htcReadyBuffer.virtualAddress(), static_cast<size_t>(_htcReadyBuffer.length()));
    if (!_htcReadyBuffer.syncForDevice())
        return false;
    _boot.rxSlot = before.dstWriteIndex;
    _boot.rxNext = Ath10k::ceRingNext(_boot.rxSlot, entries);
    _boot.rxRead = before.currentDstRead;
    auto *ring = reinterpret_cast<Ath10k::CEDescriptor32 *>(_ceDstRings[ce].virtualAddress());
    auto *desc = &ring[_boot.rxSlot];
    bzero(desc, sizeof(*desc));
    desc->address = static_cast<UInt32>(_htcReadyBuffer.dmaAddress());
    OSSynchronizeIO();
    if (!_ceDstRings[ce].syncForDevice())
        return false;
    // Separate from BMI ownership: CE1 is now reserved for firmware HTC,
    // while CE0 still has exactly one remaining command (BMI_DONE).
    _boot.rxOwned = true;
    const UInt32 doorbell = Ath10k::ceBaseAddress(ce) + Ath10k::kCEDstWriteIndex;
    if (!_regs.write32(doorbell, _boot.rxNext) ||
        !_regs.read32(doorbell, _boot.rxWrite) || _boot.rxWrite != _boot.rxNext)
        return false;
    _boot.rxPosted = true;
    ATH10K_TRACE("Ath10kMac: HTC READY RX ARMED CE1 slot=%u next=%u bytes=%u iovm=0x%08x fw-indicator=0x%08x\n",
          _boot.rxSlot, _boot.rxNext, Ath10k::kHTCReadyBufferBytes,
          static_cast<UInt32>(_htcReadyBuffer.dmaAddress()), _boot.fwIndicator);
    return true;
}

bool Ath10kPCIDevice::waitForHTCReady()
{
    const UInt32 ce = Ath10k::kBMICENumToHost;
    const UInt32 entries = Ath10k::kCEHostConfig[ce].dstEntries;
    if (!_boot.doneSent || !_boot.bmiClosed || !_boot.rxPosted || !_boot.rxOwned ||
        _boot.rxCompleted || _boot.rxSlot >= entries || !_htcReadyBuffer.allocated())
        return false;
    auto *ring = reinterpret_cast<Ath10k::CEDescriptor32 *>(_ceDstRings[ce].virtualAddress());
    const auto *desc = &ring[_boot.rxSlot];
    for (;;) {
        _boot.status = "htc-mmio-failed";
        OSSynchronizeIO();
        if (!_regs.read32(Ath10k::ceBaseAddress(ce) + Ath10k::kCECurrentDstRead, _boot.rxRead) ||
            _boot.rxRead >= entries ||
            (_boot.rxRead != _boot.rxSlot && _boot.rxRead != _boot.rxNext) ||
            !_regs.read32(Ath10k::kQca6174RegisterLayout.fwIndicatorAddress, _boot.fwIndicator) ||
            _boot.fwIndicator == 0xffffffffu)
            return false;
        if (_boot.fwIndicator & Ath10k::kFwIndicatorEventPending) {
            _boot.firmwareCrash = true;
            _boot.status = "firmware-crash-indicated";
            return false;
        }
        _boot.status = "htc-ring-sync-failed";
        if (!_ceDstRings[ce].syncFromDevice())
            return false;
        _boot.rxBytes = desc->nbytes;
        if (_boot.rxBytes != 0)
            break; // Descriptor completion is authoritative, even if DRRI lags.
        if (_boot.waitMs >= Ath10k::kHTCReadyTimeoutMs) {
            _boot.timedOut = true;
            _boot.status = "htc-ready-timeout";
            IOLog("Ath10kMac: HTC READY TIMEOUT waited=%u ms CE1 w/r=%u/%u slot=%u bytes=%u fw-indicator=0x%08x\n",
                  _boot.waitMs, _boot.rxWrite, _boot.rxRead, _boot.rxSlot,
                  _boot.rxBytes, _boot.fwIndicator);
            return false;
        }
        IOSleep(Ath10k::kHTCReadyPollMs);
        _boot.waitMs += Ath10k::kHTCReadyPollMs;
    }
    _boot.rxCompleted = true;
    _boot.status = "htc-descriptor-invalid";
    if (desc->address != static_cast<UInt32>(_htcReadyBuffer.dmaAddress()) ||
        _boot.rxBytes > _htcReadyBuffer.length())
        return false;
    _boot.status = "htc-buffer-sync-failed";
    if (!_htcReadyBuffer.syncFromDevice())
        return false;
    const auto *bytes = reinterpret_cast<const UInt8 *>(_htcReadyBuffer.virtualAddress());
    _boot.prefixBytes = _boot.rxBytes < sizeof(_boot.prefix) ? _boot.rxBytes : sizeof(_boot.prefix);
    bcopy(bytes, _boot.prefix, _boot.prefixBytes);
    char hex[sizeof(_boot.prefix) * 3 + 1]{};
    static const char digits[] = "0123456789abcdef";
    for (UInt32 i = 0; i < _boot.prefixBytes; ++i) {
        hex[i * 3] = digits[bytes[i] >> 4];
        hex[i * 3 + 1] = digits[bytes[i] & 15];
        hex[i * 3 + 2] = ' ';
    }
    ATH10K_TRACE("Ath10kMac: HTC READY RX bytes=%u waited=%u ms CE1 w/r=%u/%u prefix=%s\n",
          _boot.rxBytes, _boot.waitMs, _boot.rxWrite, _boot.rxRead, hex);
    const char *error = Ath10k::parseHTCReady(bytes, _boot.rxBytes, _boot.ready);
    if (error) {
        _boot.parseError = error;
        _boot.status = "htc-ready-invalid";
        IOLog("Ath10kMac: HTC READY INVALID reason=%s\n", error);
        return false;
    }
    _boot.readyValidated = true;
    _boot.status = "htc-ready-before-reset";
    ATH10K_TRACE("Ath10kMac: HTC READY VERIFIED credits=%u credit-size=%u endpoints=%u extended=%s version=%u bundle-max=%u alt-credit=%u\n",
          _boot.ready.credits, _boot.ready.creditSize, _boot.ready.maxEndpoints,
          _boot.ready.extended ? "yes" : "no", _boot.ready.version,
          _boot.ready.maxBundleMessages, _boot.ready.altDataCreditSize);
    // Leave the completed descriptor and mapping untouched until reset.
    return true;
}

bool Ath10kPCIDevice::runFirmwareBootTest()
{
    if (_boot.attempted)
        return false;
    _boot.status = "boot-prerequisite-mismatch";
    if (!_boot.configSucceeded || !_boot.downloadConfigSucceeded ||
        !_bmiBoardUploadSucceeded || !_bmiBoardReadbackSucceeded || !_bmiBoardInitVerified ||
        !_otpExecuteSucceeded || !_otpResultAccepted || !_fwDownloadSucceeded ||
        !_fwStreamClosed || !_fwPostBMIProbeSucceeded || !_fwReadbackSucceeded ||
        _fwVerifiedSegments != payload().kFirmwareSegmentCount ||
        _fwReadbackBytes != payload().kFirmwareSegmentDataBytes ||
        _bmiTransferUncertain || _ceDiagTransferUncertain || _dmaQuarantine ||
        _boot.bmiClosed || _boot.rxOwned)
        return false;
    _boot.attempted = true;
    _boot.status = "final-host-interest-failed";
    if (!finishBootHostInterest())
        return false;
    _boot.status = "htc-rx-prepare-failed";
    if (!prepareHTCReadyReceive())
        return false;
    _boot.status = "bmi-done-failed";
    const UInt32 command = Ath10k::kBMIDone;
    IOLog("Ath10kMac: BMI_DONE SUBMIT once; CE1 armed for HTC READY; further BMI commands will be blocked\n");
    if (!bmiSendOnly(&command, sizeof(command)))
        return false;
    ATH10K_TRACE("Ath10kMac: BMI_DONE CONSUMED by CE0; waiting up to %u ms for HTC READY\n",
          Ath10k::kHTCReadyTimeoutMs);
    return waitForHTCReady();
}

bool Ath10kPCIDevice::pulseColdResetForQuiesce()
{
    const UInt32 offset = Ath10k::globalResetOffset(Ath10k::kQca6174RegisterLayout);
    UInt32 before = 0xffffffffu;
    if (!_regs.read32(offset, before)) {
        IOLog("Ath10kMac: BMI quiesce reset ERROR cannot read SOC_GLOBAL_RESET\n");
        return false;
    }
    const UInt32 asserted = before | Ath10k::kSocGlobalResetMask;
    if (!_regs.write32(offset, asserted)) {
        IOLog("Ath10kMac: BMI quiesce reset ERROR assert failed\n");
        return false;
    }
    IOSleep(Ath10k::kColdResetDelayMs);
    if (!_regs.write32(offset, asserted & ~Ath10k::kSocGlobalResetMask)) {
        IOLog("Ath10kMac: BMI quiesce reset ERROR deassert failed\n");
        return false;
    }
    IOSleep(Ath10k::kColdResetDelayMs);
    UInt32 after = 0xffffffffu;
    const bool readOk = _regs.read32(offset, after);
    const bool verified = readOk && ((after & Ath10k::kSocGlobalResetMask) == 0);
    ATH10K_TRACE("Ath10kMac: BMI quiesce cold-reset pulse %s before=0x%08x after=0x%08x\n",
          verified ? "PASS" : "FAIL", before, after);
    return verified;
}

bool Ath10kPCIDevice::quiesceTargetAfterBMI()
{
    _bmiQuiesceAttempted = true;
    _bmiQuiesceSucceeded = false;

    if (!_pci) {
        _dmaQuarantine = true;
        return false;
    }

    // Stop any new device DMA before resetting/deprogramming CE rings.
    _pci->setBusMasterEnable(false);
    OSSynchronizeIO();
    IODelay(1000);
    const UInt16 command = _pci->configRead16(kIOPCIConfigCommand);
    ATH10K_TRACE("Ath10kMac: BMI quiesce PCI bus mastering disabled command=0x%04x\n", command);

    if (command == 0xffffu || (command & 0x0004u) != 0) {
        _dmaQuarantine = true;
        IOLog("Ath10kMac: BMI quiesce ERROR bus mastering disable not verified; retaining DMA mappings\n");
        return false;
    }

    const bool resetOk = pulseColdResetForQuiesce();
    bool cleanupWakeAsserted = false;
    const bool wakeOk = wakeTarget(cleanupWakeAsserted);
    bool deinitOk = !_cePipesProgrammed;
    if (resetOk && wakeOk && _cePipesProgrammed)
        deinitOk = deinitCopyEnginePipes("post-BMI-quiesce-reset");
    releaseTargetWake(cleanupWakeAsserted);

    // Even if the BMI transaction timed out, bus mastering is already disabled
    // and a successful reset/deinit proves the DMA mappings are safe to release.
    if (resetOk && wakeOk && deinitOk) {
        _bmiTransferUncertain = false;
        _htcControlExchangeUncertain = false;
        _boot.rxOwned = false;
        _wmi.rxOwned = false;
        _rxSetup.txOwned=false;_rxSetup.ringOwned=false;
        _connection.txOwned=false;_connection.wmiOwned=false;
        _wmiInit.txOwned = false;
        _wmiInit.hostMemoryOwned = false;
        _wmiEcho.txOwned = false;
        _wmiVdevCreate.txOwned = false;
        for (auto &radio:_wmiRadio) radio.txOwned=false;
        _wmiScanChannels.txOwned = false;
        _wmiScan.txOwned = false;
        _bmiQuiesceSucceeded = true;
        // Keep _initConfigPersistent=true as historical telemetry: it was
        // intentionally kept applied through wake_target_cpu and BMI.
        ATH10K_TRACE("Ath10kMac: BMI quiesce COMPLETE reset=yes wake=%s ce-deinit=yes DMA-safe=yes\n",
              wakeOk ? "yes" : "no");
        return true;
    }

    _dmaQuarantine = true;
    IOLog("Ath10kMac: BMI quiesce FAILED reset=%s wake=%s ce-deinit=%s; PCI bus mastering remains disabled; REBOOT REQUIRED\n",
          resetOk ? "yes" : "no", wakeOk ? "yes" : "no", deinitOk ? "yes" : "no");
    return false;
}

bool Ath10kPCIDevice::warmResetTarget()
{
    _warmResetAttempted = true;
    _warmResetSucceeded = false;
    _warmResetIRQMasked = false;
    _warmResetSI0Succeeded = false;
    _warmResetCPU1Succeeded = false;
    _warmResetCPU2Succeeded = false;
    _warmResetCESucceeded = false;
    _warmResetLFCleared = false;
    _warmResetFirstTargetInitSucceeded = false;
    _warmResetSecondTargetInitSucceeded = false;
    _cePipeInitPassCount = 0;

    const UInt32 resetOffset = Ath10k::socResetControlOffset(Ath10k::kQca6174RegisterLayout);
    _regs.read32(resetOffset, _warmResetControlBefore);
    ATH10K_TRACE("Ath10kMac: QCA6174/QCA9377 warm reset begin control=0x%08x (Linux sequence; transfers=OFF)\n",
          _warmResetControlBefore);

    if (!maskTargetIRQsForWarmReset())
        return false;
    if (!warmResetSI0())
        return false;

    _warmResetCPU1Succeeded = warmResetCPU("pass1");
    if (!_warmResetCPU1Succeeded)
        return false;
    if (!initCopyEnginePipes("warm-pass1"))
        return false;

    _warmResetFirstTargetInitSucceeded = waitForTargetInit();
    _warmResetFirstTargetInitMs = _targetInitWaitMs;
    _warmResetFirstFWIndicator = _fwIndicatorAfter;
    ATH10K_TRACE("Ath10kMac: warm reset first target-init %s indicator=0x%08x elapsed=%u ms (Linux ignores this return)\n",
          _warmResetFirstTargetInitSucceeded ? "READY" : "FAILED",
          _warmResetFirstFWIndicator, _warmResetFirstTargetInitMs);
    if (!warmResetClearLF()) {
        deinitCopyEnginePipes("warm-lf-failure");
        return false;
    }
    if (!warmResetCE()) {
        deinitCopyEnginePipes("warm-ce-reset-failure");
        return false;
    }

    _warmResetCPU2Succeeded = warmResetCPU("pass2");
    if (!_warmResetCPU2Succeeded) {
        deinitCopyEnginePipes("warm-cpu2-failure");
        return false;
    }
    if (!initCopyEnginePipes("warm-pass2"))
        return false;

    _warmResetSecondTargetInitSucceeded = waitForTargetInit();
    _warmResetSecondTargetInitMs = _targetInitWaitMs;
    _warmResetSecondFWIndicator = _fwIndicatorAfter;
    _regs.read32(resetOffset, _warmResetControlAfter);
    const UInt32 chipOffset = Ath10k::chipIdOffset(Ath10k::kQca6174RegisterLayout);
    _regs.read32(chipOffset, _postWarmChipId);
    _postWarmChipRevision = Ath10k::chipRevision(_postWarmChipId);
    const bool chipStable = _selectedHw && _postWarmChipId != 0xffffffffu &&
                            _postWarmChipRevision == _selectedHw->chipRevision;

    _warmResetSucceeded = _warmResetSecondTargetInitSucceeded && chipStable &&
                          _cePipeInitPassCount >= 2;
    ATH10K_TRACE("Ath10kMac: QCA6174/QCA9377 warm reset %s second-target-init=%s indicator=0x%08x elapsed=%u ms post-warm-chip=0x%08x rev=%u control=0x%08x ce-init-passes=%u\n",
          _warmResetSucceeded ? "COMPLETE" : "FAILED",
          _warmResetSecondTargetInitSucceeded ? "READY" : "FAILED",
          _warmResetSecondFWIndicator, _warmResetSecondTargetInitMs,
          _postWarmChipId, _postWarmChipRevision, _warmResetControlAfter,
          _cePipeInitPassCount);
    return _warmResetSucceeded;
}

bool Ath10kPCIDevice::validateCE0PostResetProgramming()
{
    _ceRingProgramAttempted = true;
    _ceRingProgramSucceeded = false;
    _ceRingRestoreSucceeded = false;
    _ceRingProgramVerifiedCount = 0;
    _ceRingRestoreVerifiedCount = 0;

    if (!_ceRingAllocationSucceeded || !_ceSrcRings[0].allocated()) {
        IOLog("Ath10kMac: CE0 post-reset test ERROR source ring unavailable\n");
        return false;
    }

    Ath10k::CESnapshot baseline{};
    if (!Ath10k::readCESnapshot(_regs, 0, baseline)) {
        IOLog("Ath10kMac: CE0 post-reset test ERROR cannot snapshot baseline\n");
        return false;
    }
    _ceBaselineSnapshots[0] = baseline;

    ATH10K_TRACE("Ath10kMac: CE0 post-reset ring test begin src=16 iovm=0x%08x transfers=OFF interrupts=MASKED\n",
          _ceSrcRings[0].dmaAddress32());

    const bool bothOk = programAndVerifyOneCE(0);
    _ceRingProgramSucceeded = (_ceRingProgramVerifiedCount == 1);
    _ceRingRestoreSucceeded = (_ceRingRestoreVerifiedCount == 1);

    ATH10K_TRACE("Ath10kMac: CE0 post-reset ring test %s programmed=%u/1 restored=%u/1 hardware left deprogrammed\n",
          bothOk ? "READY" : "FAILED",
          _ceRingProgramVerifiedCount,
          _ceRingRestoreVerifiedCount);
    return bothOk;
}

bool Ath10kPCIDevice::probeTarget()
{
    clearTargetProbeState();
    _targetProbeAttempted = true;

    ATH10K_TRACE("Ath10kMac: target probe begin (Host Interest + board/OTP/WLAN + BMI_DONE + HTC READY + persistent CE/IRQ runtime)\n");
    ATH10K_TRACE("Ath10kMac: register layout RTC_SOC=0x%08x SOC_CORE=0x%08x PCIE_LOCAL=0x%08x FW_IND=0x%08x CHIP_ID=+0x%08x\n",
          Ath10k::kQca6174RegisterLayout.rtcSocBaseAddress,
          Ath10k::kQca6174RegisterLayout.socCoreBaseAddress,
          Ath10k::kQca6174RegisterLayout.pcieLocalBaseAddress,
          Ath10k::kQca6174RegisterLayout.fwIndicatorAddress,
          Ath10k::kQca6174RegisterLayout.socChipIdAddress);

    if (!preparePCIeBringupParity()) {
        IOLog("Ath10kMac: target probe stopped: PCIe/IRQ bring-up parity failed\n");
        restorePCIeBringupParity();
        return false;
    }

    bool initialWakeAsserted = false;
    _targetWakeSucceeded = wakeTarget(initialWakeAsserted);
    if (!_targetWakeSucceeded) {
        releaseTargetWake(initialWakeAsserted);
        restorePCIeBringupParity();
        return false;
    }

    if (!inspectCopyEngines()) {
        releaseTargetWake(initialWakeAsserted);
        restorePCIeBringupParity();
        IOLog("Ath10kMac: target probe stopped: pre-reset CE inspection failed\n");
        return false;
    }

    if (!coldResetTarget()) {
        releaseTargetWake(initialWakeAsserted);
        restorePCIeBringupParity();
        return false;
    }

    if (_hasMSI && _msiCapOffset != 0)
        _msiControlPostReset = _pci->configRead16(_msiCapOffset + kMSIMessageControlOffset);
    logMSIState("post-cold-reset");

    bool postColdWakeAsserted = false;
    _postResetWakeSucceeded = wakeTarget(postColdWakeAsserted);
    if (!_postResetWakeSucceeded) {
        releaseTargetWake(initialWakeAsserted || postColdWakeAsserted);
        restorePCIeBringupParity();
        return false;
    }

    const UInt32 globalResetOffset = Ath10k::globalResetOffset(Ath10k::kQca6174RegisterLayout);
    if (!_regs.read32(globalResetOffset, _socGlobalResetAfter) ||
        _socGlobalResetAfter == 0xffffffffu ||
        (_socGlobalResetAfter & Ath10k::kSocGlobalResetMask) != 0) {
        _coldResetSucceeded = false;
        IOLog("Ath10kMac: cold reset readback FAIL SOC_GLOBAL_RESET=0x%08x\n",
              _socGlobalResetAfter);
        releaseTargetWake(initialWakeAsserted || postColdWakeAsserted);
        restorePCIeBringupParity();
        return false;
    }
    ATH10K_TRACE("Ath10kMac: cold reset readback PASS SOC_GLOBAL_RESET=0x%08x\n",
          _socGlobalResetAfter);

    _coldTargetInitSucceeded = waitForTargetInit();
    _coldTargetInitMs = _targetInitWaitMs;
    _coldFWIndicator = _fwIndicatorAfter;
    if (_hasMSI && _msiCapOffset != 0)
        _msiControlAfterWait = _pci->configRead16(_msiCapOffset + kMSIMessageControlOffset);
    logMSIState("after-cold-target-init");

    if (!_coldTargetInitSucceeded) {
        IOLog("Ath10kMac: target probe stopped: cold target-init failed; warm reset skipped\n");
        releaseTargetWake(initialWakeAsserted || postColdWakeAsserted);
        restorePCIeBringupParity();
        return false;
    }

    if (!readPostResetChipId()) {
        IOLog("Ath10kMac: target probe stopped: unsupported post-cold chip-id; warm reset skipped\n");
        releaseTargetWake(initialWakeAsserted || postColdWakeAsserted);
        restorePCIeBringupParity();
        return false;
    }

    // This milestone uses the QCA6174/QCA9377 qca6174 register/reset path only.
    if (_deviceId != Ath10k::kQCA6174Device && _deviceId != Ath10k::kQCA9377Device) {
        releaseTargetWake(initialWakeAsserted || postColdWakeAsserted);
        restorePCIeBringupParity();
        return false;
    }

    const bool warmOk = warmResetTarget();
    bool finalCEOk = false;
    bool diagReadOk = false;
    bool initConfigOk = false;
    bool wakeCpuOk = false;
    bool bmiOk = false;
    bool prereqOk = false;
    bool bootConfigOk = false;
    bool bootDownloadConfigOk = false;
    bool bootOk = false;
    bool boardOk = false;
    bool otpOk = false;
    bool firmwareOk = false;
    bool cleanupOk = true;
    bool quiesceOk = false;
    bool runtimeOk = false;
    bool htcOk = false;

    if (warmOk) {
        // Reset can clear SOC_WAKE. Reassert before touching the ROM/CE pipes.
        bool postWarmWakeAsserted=false;
        const bool wakeHeld= wakeTarget(postWarmWakeAsserted);
        postColdWakeAsserted = postColdWakeAsserted || postWarmWakeAsserted;
        // Linux hif_power_up initializes host pipes after chip_reset returns.
        // The ROM may have replaced rings during the final target-init wait.
        finalCEOk = wakeHeld && initCopyEnginePipes("post-chip-reset") && inspectCopyEngines();
        if (finalCEOk) {
            UInt32 verified = 0;
            UInt32 configured = 0;
            for (UInt32 ceId = 0; ceId < Ath10k::kCECount; ++ceId) {
                const auto &cfg = Ath10k::kCEHostConfig[ceId];
                if (cfg.srcEntries == 0 && cfg.dstEntries == 0)
                    continue;
                ++configured;
                if (verifyCopyEnginePipe(ceId, _ceSnapshots[ceId]))
                    ++verified;
            }
            _cePipeLastVerifiedCount = verified;
            finalCEOk = verified == configured;
            ATH10K_TRACE("Ath10kMac: post-warm CE validation %s verified=%u/%u\n",
                  finalCEOk ? "PASS" : "FAIL", verified, configured);
        }

        if (finalCEOk) {
            diagReadOk = runCE7DiagnosticReadTest();
            if (diagReadOk)
                initConfigOk = runCE7InitConfigPersistentTest();
            if (initConfigOk)
                wakeCpuOk = wakeTargetCPUForBMI();
            if (wakeCpuOk) {
                bmiOk = runBMIGetTargetInfoTest();
                if (bmiOk)
                    prereqOk = runBMIBoardPrerequisiteProbe();
                if (prereqOk && configureTargetPLL())
                    bootConfigOk = configureBootHostInterest();
                if (bootConfigOk)
                    boardOk = runBMIBoardUploadTest();
                if (boardOk)
                    otpOk = runBMIOtpTest();
                if (otpOk)
                    bootDownloadConfigOk = configureBootFirmwareDownload();
                if (bootDownloadConfigOk)
                    firmwareOk = runBMIFirmwareDownloadTest();
                if (firmwareOk)
                    bootOk = runFirmwareBootTest();
            }
        }
    }

    // CE7 failures before CPU wake keep the earlier quarantine behavior. Once
    // wake_target_cpu has been issued, a verified HTC READY now transitions to
    // the Phase 3 runtime. Any other path still quiesces/resets before DMA
    // cleanup because the Target ROM can use CE0/CE1.
    if (_ceDiagTransferUncertain) {
        cleanupOk = false;
        if (_pci)
            _pci->setBusMasterEnable(false);
        _dmaQuarantine = true;
        IOLog("Ath10kMac: CE7 diagnostic QUARANTINE: submitted DMA completion uncertain; PCI bus mastering disabled, CE rings intentionally left programmed; REBOOT REQUIRED\n");
    } else if (wakeCpuOk || _wakeTargetCPUAttempted) {
        if (bootOk) {
            runtimeOk = startRuntimeCEIRQ();
            if (!runtimeOk) {
                stopRuntimeCEIRQ();
                _runtimeStatus = "runtime-start-failed";
            } else {
                htcOk = runHTCServiceSetup();
                if (!htcOk) {
                    IOLog("Ath10kMac: Phase 3 step 2 HTC service setup FAILED status=%s; stopping runtime before safe reset\n",
                          _htcStatus);
                    stopRuntimeCEIRQ();
                    runtimeOk = false;
                } else {
                    const bool wmiOk = runWMIBootstrap();
                    ATH10K_TRACE("Ath10kMac: Phase 3 bootstrap %s status=%s; WMI_INIT follows only on success\n",
                          wmiOk ? "PASS" : "FAILED", _wmi.status);
                    if (wmiOk) {
                        const bool initOk=runWMIInit();
                        runtimeOk = initOk && prepareWMIRuntime();
                        ATH10K_TRACE("Ath10kMac: WMI INIT %s status=%s persistent-receiver=%s\n",
                              initOk?"PASS":"FAILED",_wmiInit.status,runtimeOk?"prepared":"failed");
                    } else runtimeOk = false;
                    if (!runtimeOk) stopRuntimeCEIRQ();
                }
            }
        }
        if (runtimeOk) {
            cleanupOk = true;
            _runtimeWakeAsserted = initialWakeAsserted || postColdWakeAsserted;
            ATH10K_TRACE("Ath10kMac: Phase 3 step 4 transition complete; keeping target awake with CE1/CE2 DMA owned\n");
        } else {
            quiesceOk = quiesceTargetAfterBMI();
            cleanupOk = quiesceOk;
            releaseTargetWake(initialWakeAsserted || postColdWakeAsserted);
            restorePCIeBringupParity();
        }
    } else {
        // init_config failure occurs before CPU wake. The persistent helper
        // rolls back any partial semantic writes just like 0.0.13.
        if (_cePipesProgrammed)
            cleanupOk = deinitCopyEnginePipes("pre-BMI-failure-cleanup");
        releaseTargetWake(initialWakeAsserted || postColdWakeAsserted);
        restorePCIeBringupParity();
    }

    const bool complete = warmOk && finalCEOk && diagReadOk && initConfigOk &&
                          wakeCpuOk && bmiOk && prereqOk && bootConfigOk && boardOk && otpOk &&
                          bootDownloadConfigOk && firmwareOk && bootOk && cleanupOk &&
                          (runtimeOk || quiesceOk) &&
                          !_initConfigTargetDirty && !_dmaQuarantine;
    const char *lifetime = runtimeOk ? "runtime-active" :
                           (quiesceOk ? "safe-reset" : "failed/not-run");
    ATH10K_TRACE("Ath10kMac: phase2 BMI+PREREQ %s cold-init=%s chip=%s warm=%s final-ce=%s diag-read=%s diag-write=%s init-config=%s wake-cpu=%s bmi-target-info=%s bmi-read-memory=%s board-prereq=%s target-version=0x%08x target-type=0x%08x board=0x%08x cal=0x%08x lifetime=%s ce7-transactions=%u ce7-writes=%u bmi-reads=%u parity-restored=%s\n",
          complete ? "COMPLETE" : "PARTIAL",
          _coldTargetInitSucceeded ? "ready" : "failed",
          _selectedHw ? _selectedHw->targetRevision : "unresolved",
          warmOk ? "complete" : "failed",
          finalCEOk ? "verified" : "not-verified",
          diagReadOk ? "ready" : "failed",
          _ceDiagWriteSucceeded ? "ready" : "failed",
          initConfigOk ? "persistent" : "failed",
          wakeCpuOk ? "ready" : "failed",
          bmiOk ? "ready" : "failed",
          _bmiReadMemorySucceeded ? "ready" : "failed/not-run",
          prereqOk ? "ready" : "incomplete",
          _bmiTargetVersion, _bmiTargetType,
          _bmiBoardDataAddress, _bmiCalDataAddress,
          lifetime,
          _ceDiagTransactionCount, _ceDiagWriteTransactionCount,
          _bmiReadMemoryTransactionCount,
          _pcieParityRestored ? "yes" : "no");

    ATH10K_TRACE("Ath10kMac: phase2 BOARD %s status=%s written=%u verified=%u bmi-writes-total=%u lifetime=%s\n",
          boardOk ? "COMPLETE" : "PARTIAL", _bmiBoardUploadStatus,
          _bmiBoardBytesWritten, _bmiBoardBytesVerified, _bmiWriteMemoryTransactionCount,
          lifetime);

    ATH10K_TRACE("Ath10kMac: phase2 OTP %s status=%s download=%s wire-bytes=%u lz-data-commands=%u execute=%s result=0x%08x accepted=%s lifetime=%s\n",
          otpOk ? "COMPLETE" : "PARTIAL", _otpStatus,
          _otpDownloadSucceeded ? "yes" : "no", _otpWireBytesSent, _otpLZDataCommands,
          _otpExecuteSucceeded ? "yes" : "no", _otpResult,
          _otpResultAccepted ? "yes" : "no", lifetime);

    ATH10K_TRACE("Ath10kMac: phase2 WLAN %s status=%s wire-bytes=%u data-commands=%u readback=%u segments=%u post-bmi=%s lifetime=%s\n",
          firmwareOk ? "COMPLETE" : "PARTIAL", _fwStatus, _fwWireBytesSent,
          _fwLZDataCommands, _fwReadbackBytes, _fwVerifiedSegments,
          _fwPostBMIProbeSucceeded ? "yes" : "no", lifetime);

    ATH10K_TRACE("Ath10kMac: phase2 BOOT %s status=%s host-interest=%s done-attempted=%s done-consumed=%s htc-ready=%s rx-bytes=%u waited=%u ms parse=%s fw-indicator=0x%08x lifetime=%s\n",
          complete ? "COMPLETE" : "PARTIAL", _boot.status,
          _boot.preDoneHIVerified ? "verified" : "incomplete",
          _boot.doneAttempted ? "yes" : "no", _boot.doneSent ? "yes" : "no",
          _boot.readyValidated ? "yes" : "no", _boot.rxBytes, _boot.waitMs,
          _boot.parseError, _boot.fwIndicator, lifetime);

    if (_dmaQuarantine || _ceDiagTransferUncertain || _bmiTransferUncertain ||
        (_boot.rxOwned && !runtimeOk))
        IOLog("Ath10kMac: NOTE DMA quarantine active: do not reload/unload repeatedly; reboot before another invasive test\n");
    else if (_initConfigTargetDirty)
        IOLog("Ath10kMac: NOTE init_config rollback incomplete before CPU wake; reboot before another invasive test\n");
    else if (_wmi.attempted)
        ATH10K_TRACE("Ath10kMac: WMI bootstrap status=%s service-ready=%s safe-reset=%s; inspect WMIRuntimeActive for live receiver\n",
              _wmi.status, _wmi.serviceReady ? "yes" : "no", quiesceOk ? "yes" : "no");
    else if (runtimeOk)
        ATH10K_TRACE("Ath10kMac: NOTE 0.0.91 Phase 3 step 2 active: HTC reserved control, WMI_CONTROL and HTT_DATA endpoints connected; SETUP_COMPLETE deferred until CE2/WMI RX; data TX disabled\n");
    else if (_htcAttempted && !htcOk)
        IOLog("Ath10kMac: NOTE 0.0.91 HTC service setup incomplete status=%s; target was safely reset=%s\n",
              _htcStatus, quiesceOk ? "yes" : "no");
    else if (complete)
        ATH10K_TRACE("Ath10kMac: NOTE 0.0.91 received valid HTC READY then safe reset. Firmware is no longer running; HTC service setup/WMI/HTT and Wi-Fi interface are not active\n");
    else
        IOLog("Ath10kMac: NOTE 0.0.91 milestone incomplete; inspect Host Interest, board, OTP, WLAN, BOOT/HTC, CE/IRQ runtime and quiesce telemetry\n");

    return complete;
}

void Ath10kPCIDevice::clearCEState()
{
    _ceInspectionAttempted = false;
    _ceInspectionSucceeded = false;
    _ceValidCount = 0;
    _ceWrapperInterruptSummary = 0xffffffffu;
    for (UInt32 i = 0; i < Ath10k::kCECount; ++i)
        _ceSnapshots[i] = Ath10k::CESnapshot{};
}

bool Ath10kPCIDevice::inspectCopyEngines()
{
    clearCEState();
    _ceInspectionAttempted = true;

    const auto &layout = Ath10k::kQca6174RegisterLayout;
    ATH10K_TRACE("Ath10kMac: CE foundation begin count=%u wrapper=0x%08x stride=0x%03x desc-size=%u align=%u\n",
          Ath10k::kCECount, layout.ceWrapperBaseAddress, Ath10k::kCEBaseStride,
          Ath10k::kCEDescriptorSize, Ath10k::kCEDescriptorRingAlign);

    // Wrapper interrupt summary is read-only diagnostic state. 0x0000 is the
    // summary register for the qcax CE wrapper.
    if (!_regs.read32(layout.ceWrapperBaseAddress, _ceWrapperInterruptSummary)) {
        IOLog("Ath10kMac: CE wrapper ERROR cannot read interrupt summary\n");
        return false;
    }
    ATH10K_TRACE("Ath10kMac: CE wrapper interrupt-summary raw=0x%08x\n",
          _ceWrapperInterruptSummary);

    for (UInt32 i = 0; i < Ath10k::kCECount; ++i) {
        auto &snap = _ceSnapshots[i];
        if (!Ath10k::readCESnapshot(_regs, i, snap)) {
            IOLog("Ath10kMac: CE%u read ERROR base=0x%08x\n",
                  i, Ath10k::ceBaseAddress(i));
            continue;
        }

        ++_ceValidCount;
        ATH10K_TRACE("Ath10kMac: CE%u base=0x%08x src-base=%08x src-size=%u dst-base=%08x dst-size=%u\n",
              i, snap.base, snap.srcBase, snap.srcSize, snap.dstBase, snap.dstSize);
        ATH10K_TRACE("Ath10kMac: CE%u idx src-w=%u src-r=%u dst-w=%u dst-r=%u ctrl1=%08x src-wm=%08x dst-wm=%08x cmd=%08x host-ie=%08x misc-ie=%08x\n",
              i, snap.srcWriteIndex, snap.currentSrcRead, snap.dstWriteIndex,
              snap.currentDstRead, snap.ctrl1, snap.srcWatermark, snap.dstWatermark,
              snap.command, snap.hostIE, snap.miscIE);
    }

    _ceInspectionSucceeded = (_ceValidCount == Ath10k::kCECount);
    ATH10K_TRACE("Ath10kMac: CE foundation %s valid=%u/%u (pre-transaction snapshot)\n",
          _ceInspectionSucceeded ? "READY" : "PARTIAL",
          _ceValidCount, Ath10k::kCECount);
    return _ceInspectionSucceeded;
}

bool Ath10kPCIDevice::allocateCERings()
{
    _ceRingAllocationAttempted = true;
    _ceRingAllocationSucceeded = false;
    _ceRingAllocatedCount = 0;
    _ceRingAllocatedBytes = 0;
    _ceRingAllocationFailureCE = 0xffffffffu;
    _ceRingAllocationFailureDirection = "none";
    _ceRingAllocationFailureStage = "not-run";
    _ceRingAllocationFailureStatus = kIOReturnSuccess;
    _ceRingFailureRequestedBytes = 0;
    _ceRingFailureDMAAddress = 0;
    _ceRingFailureSegmentLength = 0;
    _ceRingFailurePhysicalAddress = 0;
    _ceRingFailureAlignment = 0;
    releaseCERings();

    if (!_dmaMapperReady) {
        IOLog("Ath10kMac: CE ring allocation ERROR DMA mapping policy not ready\n");
        return false;
    }

    _ceRingSyncOutCount = 0;
    _ceRingSyncOutFailureCount = 0;
    ATH10K_TRACE("Ath10kMac: CE ring allocation begin descriptor-size=%u align=%u address-bits=32 mapper=%s explicit-sync=yes\n",
          Ath10k::kCEDescriptorSize, Ath10k::kCEDescriptorRingAlign,
          _dmaMapper ? "device-bound" : "default/null");

    for (UInt32 ceId = 0; ceId < Ath10k::kCECount; ++ceId) {
        const auto &cfg = Ath10k::kCEHostConfig[ceId];

        if (cfg.srcEntries != 0) {
            if (!_ceSrcRings[ceId].allocate(ceId, Ath10kCERingDirection::Source,
                                           cfg.srcEntries, _dmaMapper)) {
                _ceRingAllocationFailureCE = ceId;
                _ceRingAllocationFailureDirection = "src";
                _ceRingAllocationFailureStage = _ceSrcRings[ceId].allocationFailure();
                _ceRingAllocationFailureStatus = _ceSrcRings[ceId].allocationFailureStatus();
                _ceRingFailureRequestedBytes = _ceSrcRings[ceId].allocationFailureRequestedBytes();
                _ceRingFailureDMAAddress = _ceSrcRings[ceId].allocationFailureDMAAddress();
                _ceRingFailureSegmentLength = _ceSrcRings[ceId].allocationFailureSegmentLength();
                _ceRingFailurePhysicalAddress = _ceSrcRings[ceId].allocationFailurePhysicalAddress();
                _ceRingFailureAlignment = _ceSrcRings[ceId].allocationFailureAlignment();
                IOLog("Ath10kMac: CE%u src ring allocation ERROR entries=%u\n",
                      ceId, cfg.srcEntries);
                releaseCERings();
                return false;
            }
            ++_ceRingAllocatedCount;
            _ceRingAllocatedBytes += _ceSrcRings[ceId].bytes();
            ATH10K_TRACE("Ath10kMac: CE%u src ring allocated entries=%u bytes=0x%llx physical=0x%llx iovm=0x%08x role=%s\n",
                  ceId, cfg.srcEntries,
                  (unsigned long long)_ceSrcRings[ceId].bytes(),
                  (unsigned long long)_ceSrcRings[ceId].physicalAddress(),
                  _ceSrcRings[ceId].dmaAddress32(), cfg.role);
        }

        if (cfg.dstEntries != 0) {
            if (!_ceDstRings[ceId].allocate(ceId, Ath10kCERingDirection::Destination,
                                           cfg.dstEntries, _dmaMapper)) {
                _ceRingAllocationFailureCE = ceId;
                _ceRingAllocationFailureDirection = "dst";
                _ceRingAllocationFailureStage = _ceDstRings[ceId].allocationFailure();
                _ceRingAllocationFailureStatus = _ceDstRings[ceId].allocationFailureStatus();
                _ceRingFailureRequestedBytes = _ceDstRings[ceId].allocationFailureRequestedBytes();
                _ceRingFailureDMAAddress = _ceDstRings[ceId].allocationFailureDMAAddress();
                _ceRingFailureSegmentLength = _ceDstRings[ceId].allocationFailureSegmentLength();
                _ceRingFailurePhysicalAddress = _ceDstRings[ceId].allocationFailurePhysicalAddress();
                _ceRingFailureAlignment = _ceDstRings[ceId].allocationFailureAlignment();
                IOLog("Ath10kMac: CE%u dst ring allocation ERROR entries=%u\n",
                      ceId, cfg.dstEntries);
                releaseCERings();
                return false;
            }
            ++_ceRingAllocatedCount;
            _ceRingAllocatedBytes += _ceDstRings[ceId].bytes();
            ATH10K_TRACE("Ath10kMac: CE%u dst ring allocated entries=%u bytes=0x%llx physical=0x%llx iovm=0x%08x role=%s\n",
                  ceId, cfg.dstEntries,
                  (unsigned long long)_ceDstRings[ceId].bytes(),
                  (unsigned long long)_ceDstRings[ceId].physicalAddress(),
                  _ceDstRings[ceId].dmaAddress32(), cfg.role);
        }
    }

    _ceRingAllocationSucceeded = true;
    _dmaMapperUsedByRings = (_dmaMapper != nullptr);
    ATH10K_TRACE("Ath10kMac: CE ring allocation READY rings=%u total-bytes=0x%llx mapper=%s initial-sync=done (all descriptors zeroed)\n",
          _ceRingAllocatedCount, (unsigned long long)_ceRingAllocatedBytes,
          _dmaMapper ? "device-bound" : "default/null");
    return true;
}

void Ath10kPCIDevice::releaseCERings()
{
    for (UInt32 ceId = 0; ceId < Ath10k::kCECount; ++ceId) {
        _ceSrcRings[ceId].release();
        _ceDstRings[ceId].release();
    }
}

bool Ath10kPCIDevice::verifyOneCEProgrammed(UInt32 ceId,
                                            const Ath10k::CESnapshot &snapshot) const
{
    if (ceId >= Ath10k::kCECount || !snapshot.valid)
        return false;

    const auto &cfg = Ath10k::kCEHostConfig[ceId];
    bool ok = snapshot.hostIE == 0 && snapshot.miscIE == 0;

    if (cfg.srcEntries != 0) {
        ok &= _ceSrcRings[ceId].allocated();
        ok &= snapshot.srcBase == _ceSrcRings[ceId].dmaAddress32();
        ok &= snapshot.srcSize == cfg.srcEntries;
        ok &= snapshot.srcWriteIndex == 0;
    }

    if (cfg.dstEntries != 0) {
        ok &= _ceDstRings[ceId].allocated();
        ok &= snapshot.dstBase == _ceDstRings[ceId].dmaAddress32();
        ok &= snapshot.dstSize == cfg.dstEntries;
        ok &= snapshot.dstWriteIndex == 0;
    }

    return ok;
}

bool Ath10kPCIDevice::verifyOneCERestored(UInt32 ceId,
                                          const Ath10k::CESnapshot &baseline,
                                          const Ath10k::CESnapshot &restored) const
{
    if (ceId >= Ath10k::kCECount || !baseline.valid || !restored.valid)
        return false;

    const auto &cfg = Ath10k::kCEHostConfig[ceId];
    bool ok = restored.hostIE == baseline.hostIE &&
              restored.miscIE == baseline.miscIE;

    if (cfg.srcEntries != 0) {
        ok &= restored.srcBase == baseline.srcBase;
        ok &= restored.srcSize == baseline.srcSize;
        ok &= restored.srcWriteIndex == baseline.srcWriteIndex;
    }

    if (cfg.dstEntries != 0) {
        ok &= restored.dstBase == baseline.dstBase;
        ok &= restored.dstSize == baseline.dstSize;
        ok &= restored.dstWriteIndex == baseline.dstWriteIndex;
    }

    return ok;
}

bool Ath10kPCIDevice::restoreOneCE(UInt32 ceId,
                                   const Ath10k::CESnapshot &baseline)
{
    if (ceId >= Ath10k::kCECount || !baseline.valid)
        return false;

    const UInt32 base = Ath10k::ceBaseAddress(ceId);
    const auto &cfg = Ath10k::kCEHostConfig[ceId];
    bool ok = true;

    // Remove producer visibility first, then remove the ring mapping. Current
    // read indices are hardware-owned and are never written by the diagnostic helper.
    if (cfg.srcEntries != 0) {
        ok &= _regs.write32(base + Ath10k::kCESrcRingSize, 0);
        ok &= _regs.write32(base + Ath10k::kCESrcWriteIndex, 0);
        ok &= _regs.write32(base + Ath10k::kCESrcRingBaseLo, baseline.srcBase);
        ok &= _regs.write32(base + Ath10k::kCESrcRingSize, baseline.srcSize);
        ok &= _regs.write32(base + Ath10k::kCESrcWriteIndex, baseline.srcWriteIndex);
    }
    if (cfg.dstEntries != 0) {
        ok &= _regs.write32(base + Ath10k::kCEDstRingSize, 0);
        ok &= _regs.write32(base + Ath10k::kCEDstWriteIndex, 0);
        ok &= _regs.write32(base + Ath10k::kCEDstRingBaseLo, baseline.dstBase);
        ok &= _regs.write32(base + Ath10k::kCEDstRingSize, baseline.dstSize);
        ok &= _regs.write32(base + Ath10k::kCEDstWriteIndex, baseline.dstWriteIndex);
    }

    ok &= _regs.write32(base + Ath10k::kCEHostIE, baseline.hostIE);
    ok &= _regs.write32(base + Ath10k::kCEMiscIE, baseline.miscIE);
    return ok;
}

bool Ath10kPCIDevice::programAndVerifyOneCE(UInt32 ceId)
{
    if (ceId >= Ath10k::kCECount)
        return false;

    const auto &cfg = Ath10k::kCEHostConfig[ceId];
    if (cfg.srcEntries == 0 && cfg.dstEntries == 0)
        return true;

    const auto baseline = _ceBaselineSnapshots[ceId];
    if (!baseline.valid)
        return false;

    const UInt32 base = Ath10k::ceBaseAddress(ceId);
    bool writesOk = true;

    // Keep all CE host/misc interrupt sources masked throughout the test.
    writesOk &= _regs.write32(base + Ath10k::kCEHostIE, 0);
    writesOk &= _regs.write32(base + Ath10k::kCEMiscIE, 0);

    if (cfg.srcEntries != 0) {
        writesOk &= _regs.write32(base + Ath10k::kCESrcRingSize, 0);
        writesOk &= _regs.write32(base + Ath10k::kCESrcWriteIndex, 0);
        writesOk &= _regs.write32(base + Ath10k::kCESrcRingBaseLo,
                                  _ceSrcRings[ceId].dmaAddress32());
        writesOk &= _regs.write32(base + Ath10k::kCESrcRingSize, cfg.srcEntries);
    }

    if (cfg.dstEntries != 0) {
        writesOk &= _regs.write32(base + Ath10k::kCEDstRingSize, 0);
        writesOk &= _regs.write32(base + Ath10k::kCEDstWriteIndex, 0);
        writesOk &= _regs.write32(base + Ath10k::kCEDstRingBaseLo,
                                  _ceDstRings[ceId].dmaAddress32());
        writesOk &= _regs.write32(base + Ath10k::kCEDstRingSize, cfg.dstEntries);
    }

    Ath10k::CESnapshot programmed{};
    const bool snapshotOk = Ath10k::readCESnapshot(_regs, ceId, programmed);
    const bool programmedOk = writesOk && snapshotOk &&
                              verifyOneCEProgrammed(ceId, programmed);

    ATH10K_TRACE("Ath10kMac: CE%u ring program readback=%s src=%08x/%u dst=%08x/%u src-w=%u dst-w=%u irq=%08x/%08x\n",
          ceId, programmedOk ? "PASS" : "FAIL",
          programmed.srcBase, programmed.srcSize,
          programmed.dstBase, programmed.dstSize,
          programmed.srcWriteIndex, programmed.dstWriteIndex,
          programmed.hostIE, programmed.miscIE);

    // Always restore the exact pre-test register state before moving on.
    const bool restoreWritesOk = restoreOneCE(ceId, baseline);
    Ath10k::CESnapshot restored{};
    const bool restoredSnapshotOk = Ath10k::readCESnapshot(_regs, ceId, restored);
    const bool restoredOk = restoreWritesOk && restoredSnapshotOk &&
                            verifyOneCERestored(ceId, baseline, restored);

    ATH10K_TRACE("Ath10kMac: CE%u ring restore readback=%s src=%08x/%u dst=%08x/%u src-w=%u dst-w=%u irq=%08x/%08x\n",
          ceId, restoredOk ? "PASS" : "FAIL",
          restored.srcBase, restored.srcSize,
          restored.dstBase, restored.dstSize,
          restored.srcWriteIndex, restored.dstWriteIndex,
          restored.hostIE, restored.miscIE);

    if (programmedOk)
        ++_ceRingProgramVerifiedCount;
    if (restoredOk)
        ++_ceRingRestoreVerifiedCount;

    return programmedOk && restoredOk;
}

bool Ath10kPCIDevice::runDMATest()
{
    _dmaTestAttempted = true;
    _dmaTestSucceeded = false;
    _dmaTestBuffer.release();

    static constexpr UInt64 kTestSize = 4096;
    static constexpr UInt64 kTestAlignment = Ath10k::kCEDescriptorRingAlign;

    ATH10K_TRACE("Ath10kMac: DMA foundation allocate size=%llu align=%llu device-address-bits=32 mapper=%s explicit-sync=yes\n",
          (unsigned long long)kTestSize,
          (unsigned long long)kTestAlignment,
          _dmaMapper ? "device-bound" : "default/null");

    if (!_dmaMapperReady ||
        !_dmaTestBuffer.allocate(kTestSize, kTestAlignment, _dmaMapper)) {
        IOLog("Ath10kMac: DMA foundation ERROR allocation/prepare failed\n");
        return false;
    }

    _dmaTestSucceeded = true;
    _dmaMapperUsedByTest = _dmaTestBuffer.usesExplicitMapper();
    ATH10K_TRACE("Ath10kMac: DMA foundation READY virtual=%p physical=0x%llx iovm=0x%llx segment=0x%llx length=0x%llx align=%llu\n",
          _dmaTestBuffer.virtualAddress(),
          (unsigned long long)_dmaTestBuffer.physicalAddress(),
          (unsigned long long)_dmaTestBuffer.dmaAddress(),
          (unsigned long long)_dmaTestBuffer.segmentLength(),
          (unsigned long long)_dmaTestBuffer.length(),
          (unsigned long long)_dmaTestBuffer.alignment());
    ATH10K_TRACE("Ath10kMac: DMA regression buffer remains host-only; CE rings use separate mapped buffers\n");
    return true;
}

const char *Ath10kPCIDevice::chipName() const
{
    return Ath10k::chipNameForDevice(_deviceId);
}

void Ath10kPCIDevice::publishDiagnosticProperties()
{
    publishWMIRuntimeProperties();
    setProperty("Vendor", "Qualcomm Atheros");
    setProperty("Model", chipName());
    setProperty("Ath10kChip", chipName());
    setProperty("Ath10kStage", _networkInterface
                ? "STA network interface: scan, open/WPA2 association and Ethernet traffic"
                : _wmiLive.attempted
                ? "WMI/HTT runtime and passive scan; network interface pending"
                : _wmiInit.attempted
                ? "Phase 3: bounded WMI INIT and credit return; reset after test"
                : _wmi.attempted
                ? "Phase 3: bounded CE2/WMI bootstrap; reset after test"
                : _htcServiceSetupSucceeded
                ? "Phase 3 step 2: endpoints connected; HTC start deferred"
                : (_runtimeActive
                   ? "Phase 3 step 1: persistent CE1 RX + PCIe/MSI IRQ after HTC READY"
                   : "Phase 2 boot: Host Interest + BMI_DONE + HTC READY"));
    setProperty("Ath10kBoardUploadAttempted", _bmiBoardUploadAttempted);
    setProperty("Ath10kBoardUploadSucceeded", _bmiBoardUploadSucceeded);
    setProperty("Ath10kBoardReadbackSucceeded", _bmiBoardReadbackSucceeded);
    setProperty("Ath10kBoardInitVerifiedBeforeReset", _bmiBoardInitVerified);
    setProperty("Ath10kBoardUploadStatus", _bmiBoardUploadStatus);
    setProperty("Ath10kBoardBytesWritten", (UInt64)_bmiBoardBytesWritten, 32);
    setProperty("Ath10kBoardBytesVerified", (UInt64)_bmiBoardBytesVerified, 32);
    setProperty("Ath10kBMIWriteMemoryTransactionCount", (UInt64)_bmiWriteMemoryTransactionCount, 32);
    setProperty("Ath10kPreparedBoardKey", payload().kBoardKey);
    setProperty("Ath10kPreparedBoardSelection", payload().kBoardSelection);
    setProperty("Ath10kPayloadProfileAvailable", payload().available);
    setProperty("Ath10kPreparedBoardSHA256", payload().kBoardSHA256);
    setProperty("Ath10kPreparedFirmwareVersion", payload().kFirmwareVersion);
    setProperty("Ath10kPreparedFirmwareBytes", (UInt64)payload().kFirmwareSize, 32);
    setProperty("Ath10kPreparedOTPBytes", (UInt64)payload().kOTPSize, 32);
    setProperty("Ath10kFirmwareDownloadAttempted", _fwAttempted);
    setProperty("Ath10kFirmwareDownloadPath", "BMI-LZ");
    setProperty("Ath10kFirmwareStatus", _fwStatus);
    setProperty("Ath10kFirmwareLZStreamStarted", _fwStreamStarted);
    setProperty("Ath10kFirmwareLZStreamClosed", _fwStreamClosed);
    setProperty("Ath10kFirmwareDownloadSucceeded", _fwDownloadSucceeded);
    setProperty("Ath10kFirmwarePostBMIProbeSucceeded", _fwPostBMIProbeSucceeded);
    setProperty("Ath10kFirmwareReadbackAttempted", _fwReadbackAttempted);
    setProperty("Ath10kFirmwareReadbackSucceeded", _fwReadbackSucceeded);
    setProperty("Ath10kFirmwareWireBytesSent", (UInt64)_fwWireBytesSent, 32);
    setProperty("Ath10kFirmwareLZDataCommands", (UInt64)_fwLZDataCommands, 32);
    setProperty("Ath10kFirmwareReadbackBytes", (UInt64)_fwReadbackBytes, 32);
    setProperty("Ath10kFirmwareVerifiedSegments", (UInt64)_fwVerifiedSegments, 32);
    setProperty("Ath10kFirmwareFailedSegment", (UInt64)_fwFailedSegment, 32);
    setProperty("Ath10kFirmwareMismatchAddress", (UInt64)_fwMismatchAddress, 32);
    setProperty("Ath10kFirmwarePostTargetVersion", (UInt64)_fwPostTargetVersion, 32);
    setProperty("Ath10kFirmwarePostTargetType", (UInt64)_fwPostTargetType, 32);
    setProperty("Ath10kFirmwareEntryAddress", (UInt64)payload().kFirmwareEntryAddress, 32);
    setProperty("Ath10kPreparedFirmwareSHA256", payload().kFirmwareSHA256);
    setProperty("Ath10kFirmwareMilestoneSucceeded", _fwDownloadSucceeded && _fwPostBMIProbeSucceeded &&
                _fwReadbackSucceeded && (_bmiQuiesceSucceeded || _runtimeActive) && !_dmaQuarantine);
    setProperty("Ath10kFirmwareBootAttempted", _boot.attempted);
    setProperty("Ath10kFirmwareBootStatus", _boot.status);
    setProperty("Ath10kFirmwareBootSucceededBeforeReset", _boot.readyValidated);
    setProperty("Ath10kFirmwareBootMilestoneSucceeded", _boot.doneSent && _boot.readyValidated &&
                (_bmiQuiesceSucceeded || _runtimeActive) && !_dmaQuarantine);
    setProperty("Ath10kBootHostInterestAttempted", _boot.configAttempted);
    setProperty("Ath10kBootHostInterestConfigured", _boot.configSucceeded);
    setProperty("Ath10kBootHostInterestVerifiedBeforeDone", _boot.preDoneHIVerified);
    setProperty("Ath10kBootHostInterestWrites", (UInt64)_boot.hiWrites, 32);
    setProperty("Ath10kBootHostInterestChecks", (UInt64)_boot.hiVerified, 32);
    setProperty("Ath10kBootOptionFlagBefore", (UInt64)_boot.optionBefore, 32);
    setProperty("Ath10kBootOptionFlagApplied", (UInt64)_boot.optionApplied, 32);
    setProperty("Ath10kBootOptionFlagFinalRead", _boot.optionFinalRead);
    setProperty("Ath10kBootOptionFlagFinalVerified", _boot.optionFinalVerified);
    setProperty("Ath10kBootOptionFlagAfterDownload", (UInt64)_boot.optionAfterDownload, 32);
    setProperty("Ath10kBootOptionFlagChangedBits", (UInt64)_boot.optionChangedBits, 32);
    setProperty("Ath10kBootBMICreditLimitAdded", _boot.optionBMICreditLimitAdded);
    setProperty("Ath10kBootLastHIAddress", (UInt64)_boot.lastHIAddress, 32);
    setProperty("Ath10kBootLastHIExpected", (UInt64)_boot.lastHIExpected, 32);
    setProperty("Ath10kBootLastHIObserved", (UInt64)_boot.lastHIObserved, 32);
    setProperty("Ath10kBootUARTDisabled", _boot.uartDisabled);
    setProperty("Ath10kBootSkipClockInitFeature", payload().kSupportsSkipClockInit);
    setProperty("Ath10kFirmwareMFPFeatureDeclared", payload().kSupportsMFP);
    setProperty("Ath10kBootDownloadConfigSucceeded", _boot.downloadConfigSucceeded);
    setProperty("Ath10kBMIDoneAttempted", _boot.doneAttempted);
    setProperty("Ath10kBMIClosed", _boot.bmiClosed);
    setProperty("Ath10kHTCReadyRxPosted", _boot.rxPosted);
    setProperty("Ath10kHTCReadyDMAOwned", _boot.rxOwned);
    setProperty("Ath10kHTCReadyRxCompleted", _boot.rxCompleted);
    setProperty("Ath10kHTCReadyValidated", _boot.readyValidated);
    setProperty("Ath10kHTCReadyTimedOut", _boot.timedOut);
    setProperty("Ath10kHTCReadyParseError", _boot.parseError);
    setProperty("Ath10kHTCReadyBytes", (UInt64)_boot.rxBytes, 32);
    setProperty("Ath10kHTCReadyWaitMs", (UInt64)_boot.waitMs, 32);
    setProperty("Ath10kHTCReadyCESlot", (UInt64)_boot.rxSlot, 32);
    setProperty("Ath10kHTCReadyCEWrite", (UInt64)_boot.rxWrite, 32);
    setProperty("Ath10kHTCReadyCERead", (UInt64)_boot.rxRead, 32);
    setProperty("Ath10kHTCReadyCredits", (UInt64)_boot.ready.credits, 32);
    setProperty("Ath10kHTCReadyCreditSize", (UInt64)_boot.ready.creditSize, 32);
    setProperty("Ath10kHTCReadyMaxEndpoints", (UInt64)_boot.ready.maxEndpoints, 32);
    setProperty("Ath10kHTCReadyExtended", _boot.ready.extended);
    setProperty("Ath10kHTCReadyVersion", (UInt64)_boot.ready.version, 32);
    setProperty("Ath10kHTCReadyMaxBundleMessages", (UInt64)_boot.ready.maxBundleMessages, 32);
    setProperty("Ath10kHTCReadyAltDataCreditSize", (UInt64)_boot.ready.altDataCreditSize, 32);
    setProperty("Ath10kBootFWIndicator", (UInt64)_boot.fwIndicator, 32);
    setProperty("Ath10kBootFirmwareCrashIndicated", _boot.firmwareCrash);
    if (_boot.prefixBytes)
        setProperty("Ath10kHTCReadyPrefix", _boot.prefix, _boot.prefixBytes);
    setProperty("Ath10kOTPAttempted", _otpAttempted);
    setProperty("Ath10kOTPStatus", _otpStatus);
    setProperty("Ath10kOTPLZStreamStarted", _otpStreamStarted);
    setProperty("Ath10kOTPLZStreamClosed", _otpStreamClosed);
    setProperty("Ath10kOTPDownloadSucceeded", _otpDownloadSucceeded);
    setProperty("Ath10kOTPExecutionAttempted", _otpExecuteAttempted);
    setProperty("Ath10kOTPExecuteTransportSucceeded", _otpExecuteSucceeded);
    setProperty("Ath10kOTPResultAccepted", _otpResultAccepted);
    setProperty("Ath10kOTPResultIgnored", _otpResultIgnored);
    setProperty("Ath10kOTPResultZero", _otpExecuteSucceeded && _otpResult == 0);
    setProperty("Ath10kOTPResult", (UInt64)_otpResult, 32);
    setProperty("Ath10kOTPExecuteResponseBytes", (UInt64)_otpExecuteResponseBytes, 32);
    setProperty("Ath10kOTPWireBytesSent", (UInt64)_otpWireBytesSent, 32);
    setProperty("Ath10kOTPLZDataCommands", (UInt64)_otpLZDataCommands, 32);
    setProperty("Ath10kOTPLoadAddress", (UInt64)Ath10k::kQCA6174OTPLoadAddress, 32);
    setProperty("Ath10kOTPExecuteParameter", (UInt64)Ath10k::kQCA6174OTPExecuteParam, 32);
    setProperty("Ath10kOTPIgnoreResultFeature", payload().kOTPIgnoreResult);
    setProperty("Ath10kPreparedOTPSHA256", payload().kOTPSHA256);
    setProperty("Ath10kOTPMilestoneSucceeded", _otpDownloadSucceeded && _otpExecuteSucceeded &&
                _otpResultAccepted && (_bmiQuiesceSucceeded || _runtimeActive) && !_dmaQuarantine);
    setProperty("Ath10kBMIDoneSent", _boot.doneSent);
    setProperty("Ath10kDriverVersion", "0.0.98");
    setProperty("Ath10kHTCAttempted", _htcAttempted);
    setProperty("Ath10kWMIInitStatus",_wmiInit.status);
    setProperty("Ath10kWMIInitSubmitted",_wmiInit.submitted);
    setProperty("Ath10kWMIInitTxConsumed",_wmiInit.txConsumed);
    setProperty("Ath10kWMIReadyBeforeReset",_wmiInit.ready);
    setProperty("Ath10kWMICreditReturned",_wmiInit.creditReturned);
    setProperty("Ath10kWMIInitTxDMAOwned",_wmiInit.txOwned);
    setProperty("Ath10kWMIInitSucceeded",_wmiInit.completed && (_bmiQuiesceSucceeded || _runtimeActive) && !_dmaQuarantine);
    setProperty("Ath10kWMIReadyStatusCode",(UInt64)_wmiInit.statusCode,32);
    setProperty("Ath10kWMIInitWaitMs",(UInt64)_wmiInit.waitMs,32);
    setProperty("Ath10kWMIDiagnosticCount",(UInt64)_wmiInit.diagCount,32);
    setProperty("Ath10kWMIDiagnosticBytes",(UInt64)_wmiInit.diagBytes,32);
    setProperty("Ath10kWMIDiagSupportedCount",(UInt64)_wmiInit.diagSupportedCount,32);
    setProperty("Ath10kWMIDiagSupportedEntries",(UInt64)_wmiInit.diagSupportedEntries,32);
    setProperty("Ath10kWMIPackageIDCount",(UInt64)_wmiInit.pkgIdCount,32);
    setProperty("Ath10kWMIPackageID",(UInt64)_wmiInit.pkgId,32);
    setProperty("Ath10kWMIInitLastEventID",(UInt64)_wmiInit.lastEventId,32);
    setProperty("Ath10kWMIInitRxCount",(UInt64)_wmiInit.rxCount,32);
    if (_wmiInit.ready) setProperty("Ath10kWMIMACAddress",_wmiInit.mac,sizeof(_wmiInit.mac));
    setProperty("Ath10kWMIBootstrapStatus", _wmi.status);
    setProperty("Ath10kWMIBootstrapAttempted", _wmi.attempted);
    setProperty("Ath10kWMIServiceReadyBeforeReset", _wmi.serviceReady);
    setProperty("Ath10kWMIBootstrapSucceeded", _wmi.serviceReady && (_bmiQuiesceSucceeded || _runtimeActive) && !_dmaQuarantine);
    setProperty("Ath10kWMIRxDMAOwned", _wmi.rxOwned);
    setProperty("Ath10kWMIRxPosted", _wmi.rxPosted);
    setProperty("Ath10kWMIServiceAvailableCount", (UInt64)_wmi.availableCount, 32);
    setProperty("Ath10kWMIServiceAvailableBits", (UInt64)_wmi.availableBits, 32);
    setProperty("Ath10kWMIRxCount", (UInt64)_wmi.rxCount, 32);
    setProperty("Ath10kWMIRxRepostCount", (UInt64)_wmi.repostCount, 32);
    if (_wmi.availableCount)
        setProperty("Ath10kWMIServiceAvailableBitmap", _wmi.availableBitmap, sizeof(_wmi.availableBitmap));
    setProperty("Ath10kWMIRxBytes", (UInt64)_wmi.rxBytes, 32);
    setProperty("Ath10kWMIWaitMs", (UInt64)_wmi.waitMs, 32);
    if (_wmi.serviceReady && _wmiRxBuffer.allocated())
        setProperty("Ath10kWMIServiceReadyPacket", _wmi.servicePacket, _wmi.servicePacketBytes);
    setProperty("Ath10kWMIEventID", (UInt64)_wmi.eventId, 32);
    setProperty("Ath10kWMIFirmwareBuild", (UInt64)_wmi.fwBuild, 32);
    setProperty("Ath10kWMIABI0", (UInt64)_wmi.abi0, 32);
    setProperty("Ath10kWMIRFChains", (UInt64)_wmi.rfChains, 32);
    setProperty("Ath10kWMIInitRequestedChainMask",_wmiInit.requestedChainMask,32);
    setProperty("Ath10kWMIMemoryRequests", (UInt64)_wmi.memoryRequests, 32);
    setProperty("Ath10kWMICreditsInitialized", _wmi.creditsInitialized);
    setProperty("Ath10kWMICreditsAvailableBeforeReset", (UInt64)_wmi.credits.available, 32);
    setProperty("Ath10kWMICreditsOutstandingBeforeReset", (UInt64)_wmi.credits.outstanding, 32);
    setProperty("Ath10kHTCStatus", _htcStatus);
    setProperty("Ath10kHTCSetupCompleteDeferred", _htcServiceSetupSucceeded && !_htcSetupCompleteSent);
    setProperty("Ath10kHTCSetupCompleteSent", _htcSetupCompleteSent);
    setProperty("Ath10kHTCReservedControlConnected", _htcReservedControlConnected);
    setProperty("Ath10kHTCServiceSetupSucceeded", _htcServiceSetupSucceeded);
    setProperty("Ath10kHTCControlExchangeUncertain", _htcControlExchangeUncertain);
    setProperty("Ath10kHTCCreditFlowDeferred", _htcCreditFlowDeferred);
    setProperty("Ath10kHTCControlTxCount", (UInt64)_htcControlTxCount, 32);
    setProperty("Ath10kHTCControlRxCount", (UInt64)_htcControlRxCount, 32);
    setProperty("Ath10kHTCControlLastTxBytes", (UInt64)_htcControlLastTxBytes, 32);
    setProperty("Ath10kHTCControlLastRxBytes", (UInt64)_htcControlLastRxBytes, 32);
    setProperty("Ath10kHTCControlLastTxSlot", (UInt64)_htcControlLastTxSlot, 32);
    setProperty("Ath10kHTCControlLastTxNext", (UInt64)_htcControlLastTxNext, 32);
    setProperty("Ath10kHTCControlLastRxSlot", (UInt64)_htcControlLastRxSlot, 32);
    setProperty("Ath10kHTCControlLastRxNext", (UInt64)_htcControlLastRxNext, 32);
    setProperty("Ath10kHTCControlLastWaitMs", (UInt64)_htcControlLastWaitMs, 32);
    setProperty("Ath10kHTCReservedControlEndpoint", (UInt64)_htcReservedControl.endpoint, 8);
    setProperty("Ath10kHTCReservedControlMaxMessageSize", (UInt64)_htcReservedControl.maxMessageSize, 16);
    setProperty("Ath10kHTCReservedControlUplinkPipe", (UInt64)_htcReservedControl.uplinkPipe, 8);
    setProperty("Ath10kHTCReservedControlDownlinkPipe", (UInt64)_htcReservedControl.downlinkPipe, 8);
    setProperty("Ath10kHTCWMIControlConnected", _htcWMIControl.connected);
    setProperty("Ath10kHTCWMIControlEndpoint", (UInt64)_htcWMIControl.endpoint, 8);
    setProperty("Ath10kHTCWMIControlMaxMessageSize", (UInt64)_htcWMIControl.maxMessageSize, 16);
    setProperty("Ath10kHTCWMIControlUplinkPipe", (UInt64)_htcWMIControl.uplinkPipe, 8);
    setProperty("Ath10kHTCWMIControlDownlinkPipe", (UInt64)_htcWMIControl.downlinkPipe, 8);
    setProperty("Ath10kHTCWMIControlCreditFlowEnabled", _htcWMIControl.creditFlowEnabled);
    setProperty("Ath10kHTCHTTDataConnected", _htcHTTData.connected);
    setProperty("Ath10kHTCHTTDataEndpoint", (UInt64)_htcHTTData.endpoint, 8);
    setProperty("Ath10kHTCHTTDataMaxMessageSize", (UInt64)_htcHTTData.maxMessageSize, 16);
    setProperty("Ath10kHTCHTTDataUplinkPipe", (UInt64)_htcHTTData.uplinkPipe, 8);
    setProperty("Ath10kHTCHTTDataDownlinkPipe", (UInt64)_htcHTTData.downlinkPipe, 8);
    setProperty("Ath10kHTCHTTDataCreditFlowEnabled", _htcHTTData.creditFlowEnabled);
    if (_htcControlBuffer.allocated())
        setProperty("Ath10kHTCControlIOVM", (UInt64)_htcControlBuffer.dmaAddress(), 64);
    setProperty("Ath10kRuntimeAttempted", _runtimeAttempted);
    setProperty("Ath10kRuntimeActive", _runtimeActive);
    setProperty("Ath10kRuntimeStatus", _runtimeStatus);
    setProperty("Ath10kRuntimeCE1Armed", _runtimeCE1Armed);
    setProperty("Ath10kRuntimeInterruptsEnabled", _runtimeInterruptsEnabled);
    setProperty("Ath10kRuntimeWakeAsserted", _runtimeWakeAsserted);
    setProperty("Ath10kRuntimeRxSlot", (UInt64)_runtimeRxSlot, 32);
    setProperty("Ath10kRuntimeRxNext", (UInt64)_runtimeRxNext, 32);
    setProperty("Ath10kRuntimeRxPostedCount", (UInt64)_runtimeRxPostedCount, 32);
    setProperty("Ath10kRuntimeRxCompletedCount", (UInt64)_runtimeRxCompletedCount, 32);
    setProperty("Ath10kRuntimeRxReplenishedCount", (UInt64)_runtimeRxReplenishedCount, 32);
    setProperty("Ath10kRuntimeLastCE1Read", (UInt64)_runtimeLastCE1Read, 32);
    setProperty("Ath10kRuntimeLastCE1Write", (UInt64)_runtimeLastCE1Write, 32);
    setProperty("Ath10kRuntimeLastRxBytes", (UInt64)_runtimeLastRxBytes, 32);
    setProperty("Ath10kRuntimeLastHostIE", (UInt64)_runtimeLastHostIE, 32);
    setProperty("Ath10kRuntimeLastMiscIE", (UInt64)_runtimeLastMiscIE, 32);
    setProperty("Ath10kRuntimeLastPCIeIntrEnable", (UInt64)_runtimeLastPCIeIntrEnable, 32);
    setProperty("Ath10kRuntimeLastCoreCtrl", (UInt64)_runtimeLastCoreCtrl, 32);
    setProperty("Ath10kLiveMMIO", true);
    setProperty("Ath10kDMADeviceMapperAttempted", _dmaMapperAttempted);
    setProperty("Ath10kDMADeviceMapperReady", _dmaDeviceMapperAvailable);
    setProperty("Ath10kDMADeviceMapperUsedByRings", _dmaMapperUsedByRings);
    setProperty("Ath10kDMADeviceMapperUsedByDiag", _dmaMapperUsedByDiag);
    setProperty("Ath10kDMADeviceMapperUsedByTest", _dmaMapperUsedByTest);
    setProperty("Ath10kDMADeviceMapperAvailable", _dmaDeviceMapperAvailable);
    setProperty("Ath10kDMAMappingPolicyReady", _dmaMapperReady);
    setProperty("Ath10kCERingSyncOutCount", (UInt64)_ceRingSyncOutCount, 32);
    setProperty("Ath10kCERingSyncOutFailureCount", (UInt64)_ceRingSyncOutFailureCount, 32);
    setProperty("Ath10kCE7DiagPreSubmitSyncSucceeded", _ceDiagPreSubmitSyncSucceeded);
    setProperty("Ath10kCE7DiagCompletionSyncSucceeded", _ceDiagCompletionSyncSucceeded);
    setProperty("Ath10kCE7DiagSyncOutCount", (UInt64)_ceDiagSyncOutCount, 32);
    setProperty("Ath10kCE7DiagSyncInCount", (UInt64)_ceDiagSyncInCount, 32);
    setProperty("Ath10kCE7DiagSyncFailureCount", (UInt64)_ceDiagSyncFailureCount, 32);
    setProperty("Ath10kPCIeParityAttempted", _pcieParityAttempted);
    setProperty("Ath10kPCIeParityPrepared", _pcieParityPrepared);
    setProperty("Ath10kPCIeParityRestored", _pcieParityRestored);
    setProperty("Ath10kASPMDisableAttempted", _aspmDisableAttempted);
    setProperty("Ath10kASPMDisabled", _aspmDisabled);
    setProperty("Ath10kASPMRestoreSucceeded", _aspmRestoreSucceeded);
    setProperty("Ath10kPCIeLinkControlBefore", (UInt64)_pcieLinkControlBefore, 16);
    setProperty("Ath10kPCIeLinkControlDuring", (UInt64)_pcieLinkControlDuring, 16);
    setProperty("Ath10kPCIeLinkControlAfterRestore", (UInt64)_pcieLinkControlAfterRestore, 16);
    setProperty("Ath10kIRQPrepareAttempted", _irqPrepareAttempted);
    setProperty("Ath10kIRQPrepared", _irqPrepared);
    setProperty("Ath10kIRQEnabled", _irqEnabled);
    setProperty("Ath10kIRQMessaged", _irqMessaged);
    setProperty("Ath10kIRQMode",_irqMessaged?"MSI":"INTx");
    setProperty("Ath10kPLLStatus",_pll.status);
    setProperty("Ath10kPLLComplete",_pll.complete);
    setProperty("Ath10kPLLStep",UInt64(_pll.step),32);
    setProperty("Ath10kPLLReferenceHz",UInt64(_pll.referenceHz),32);
    setProperty("Ath10kPLLPolls",UInt64(_pll.polls),32);
    setProperty("Ath10kIRQIndex", (UInt64)(_irqIndex < 0 ? 0xffffffffu : (UInt32)_irqIndex), 32);
    setProperty("Ath10kIRQType", (UInt64)_irqType, 32);
    setProperty("Ath10kIRQObservedCount", (UInt64)_irqObservedCount, 32);
    setProperty("Ath10kMSIControlBefore", (UInt64)_msiControlBefore, 16);
    setProperty("Ath10kMSIControlAfterRegister", (UInt64)_msiControlAfterRegister, 16);
    setProperty("Ath10kMSIControlPostReset", (UInt64)_msiControlPostReset, 16);
    setProperty("Ath10kMSIControlAfterWait", (UInt64)_msiControlAfterWait, 16);
    setProperty("Ath10kPCICapOffsetMSI", (UInt64)_msiCapOffset, 8);
    setProperty("Ath10kPCICapOffsetPCIe", (UInt64)_pcieCapOffset, 8);
    setProperty("Ath10kTargetProbeAttempted", _targetProbeAttempted);
    setProperty("Ath10kTargetWakeSucceeded", _targetWakeSucceeded);
    setProperty("Ath10kPostResetWakeSucceeded", _postResetWakeSucceeded);
    setProperty("Ath10kTargetWasAlreadyAwake", _targetWasAlreadyAwake);
    setProperty("Ath10kChipIdValid", _chipIdValid);
    setProperty("Ath10kRTCStateBefore", (UInt64)_rtcStateBefore, 32);
    setProperty("Ath10kRTCStateAfter", (UInt64)_rtcStateAfter, 32);
    setProperty("Ath10kWakeDelayUs", (UInt64)_wakeDelayUs, 32);
    setProperty("Ath10kChipId", (UInt64)_chipId, 32);
    setProperty("Ath10kChipRevision", (UInt64)_chipRevision, 8);
    setProperty("Ath10kColdResetAttempted", _coldResetAttempted);
    setProperty("Ath10kColdResetSucceeded", _coldResetSucceeded);
    setProperty("Ath10kSocGlobalResetBefore", (UInt64)_socGlobalResetBefore, 32);
    setProperty("Ath10kSocGlobalResetAfter", (UInt64)_socGlobalResetAfter, 32);
    setProperty("Ath10kTargetInitWaitAttempted", _targetInitWaitAttempted);
    setProperty("Ath10kTargetInitWaitSucceeded", _targetInitWaitSucceeded);
    setProperty("Ath10kTargetInitWaitMs", (UInt64)_targetInitWaitMs, 32);
    setProperty("Ath10kFWIndicatorBefore", (UInt64)_fwIndicatorBefore, 32);
    setProperty("Ath10kFWIndicatorAfter", (UInt64)_fwIndicatorAfter, 32);
    setProperty("Ath10kFWIndicatorEventPending", _fwIndicatorEventPending);
    setProperty("Ath10kFWIndicatorNonZeroSeen", _fwIndicatorNonZeroSeen);
    setProperty("Ath10kFWIndicatorSampleCount", (UInt64)_fwIndicatorSampleCount, 32);
    setProperty("Ath10kFWIndicatorChangeCount", (UInt64)_fwIndicatorChangeCount, 32);
    setProperty("Ath10kColdTargetInitSucceeded", _coldTargetInitSucceeded);
    setProperty("Ath10kColdTargetInitMs", (UInt64)_coldTargetInitMs, 32);
    setProperty("Ath10kColdFWIndicator", (UInt64)_coldFWIndicator, 32);
    setProperty("Ath10kWarmResetAttempted", _warmResetAttempted);
    setProperty("Ath10kWarmResetSucceeded", _warmResetSucceeded);
    setProperty("Ath10kWarmResetIRQMasked", _warmResetIRQMasked);
    setProperty("Ath10kWarmResetSI0Succeeded", _warmResetSI0Succeeded);
    setProperty("Ath10kWarmResetCPU1Succeeded", _warmResetCPU1Succeeded);
    setProperty("Ath10kWarmResetCPU2Succeeded", _warmResetCPU2Succeeded);
    setProperty("Ath10kWarmResetCESucceeded", _warmResetCESucceeded);
    setProperty("Ath10kWarmResetLFCleared", _warmResetLFCleared);
    setProperty("Ath10kWarmResetFirstTargetInitSucceeded", _warmResetFirstTargetInitSucceeded);
    setProperty("Ath10kWarmResetSecondTargetInitSucceeded", _warmResetSecondTargetInitSucceeded);
    setProperty("Ath10kWarmResetFirstTargetInitMs", (UInt64)_warmResetFirstTargetInitMs, 32);
    setProperty("Ath10kWarmResetSecondTargetInitMs", (UInt64)_warmResetSecondTargetInitMs, 32);
    setProperty("Ath10kWarmResetFirstFWIndicator", (UInt64)_warmResetFirstFWIndicator, 32);
    setProperty("Ath10kWarmResetSecondFWIndicator", (UInt64)_warmResetSecondFWIndicator, 32);
    setProperty("Ath10kWarmResetControlBefore", (UInt64)_warmResetControlBefore, 32);
    setProperty("Ath10kWarmResetControlAfter", (UInt64)_warmResetControlAfter, 32);
    setProperty("Ath10kWarmResetLFBefore", (UInt64)_warmResetLFBefore, 32);
    setProperty("Ath10kWarmResetLFAfter", (UInt64)_warmResetLFAfter, 32);
    setProperty("Ath10kPostWarmChipId", (UInt64)_postWarmChipId, 32);
    setProperty("Ath10kPostWarmChipRevision", (UInt64)_postWarmChipRevision, 8);
    if (_selectedHw) {
        setProperty("Ath10kTargetRevision", _selectedHw->targetRevision);
        setProperty("Ath10kFirmwareDirectory", _selectedHw->firmwareDirectory);
        setProperty("Ath10kTargetProfileState", "post-reset-verified");
    } else {
        setProperty("Ath10kTargetRevision", "unresolved");
        setProperty("Ath10kTargetProfileState", "unresolved");
    }
    setProperty("Ath10kCECount", (UInt64)Ath10k::kCECount, 32);
    setProperty("Ath10kCEInspectionAttempted", _ceInspectionAttempted);
    setProperty("Ath10kCEInspectionSucceeded", _ceInspectionSucceeded);
    setProperty("Ath10kCEValidCount", (UInt64)_ceValidCount, 32);
    setProperty("Ath10kCEWrapperInterruptSummary", (UInt64)_ceWrapperInterruptSummary, 32);
    setProperty("Ath10kCERingAllocationAttempted", _ceRingAllocationAttempted);
    setProperty("Ath10kCERingAllocationSucceeded", _ceRingAllocationSucceeded);
    setProperty("Ath10kCERingsAllocated", (UInt64)_ceRingAllocatedCount, 32);
    setProperty("Ath10kCERingAllocatedBytes", (UInt64)_ceRingAllocatedBytes, 64);
    setProperty("Ath10kCERingAllocationFailureCE", (UInt64)_ceRingAllocationFailureCE, 32);
    setProperty("Ath10kCERingAllocationFailureDirection", _ceRingAllocationFailureDirection);
    setProperty("Ath10kCERingAllocationFailureStage", _ceRingAllocationFailureStage);
    setProperty("Ath10kCERingAllocationFailureStatus", (UInt64)(UInt32)_ceRingAllocationFailureStatus, 32);
    setProperty("Ath10kCERingFailureRequestedBytes", _ceRingFailureRequestedBytes, 64);
    setProperty("Ath10kCERingFailureDMAAddress", _ceRingFailureDMAAddress, 64);
    setProperty("Ath10kCERingFailureSegmentLength", _ceRingFailureSegmentLength, 64);
    setProperty("Ath10kCERingFailurePhysicalAddress", _ceRingFailurePhysicalAddress, 64);
    setProperty("Ath10kCERingFailureAlignment", _ceRingFailureAlignment, 64);
    setProperty("Ath10kCERingProgramAttempted", _ceRingProgramAttempted);
    setProperty("Ath10kCERingProgramSucceeded", _ceRingProgramSucceeded);
    setProperty("Ath10kCERingRestoreSucceeded", _ceRingRestoreSucceeded);
    setProperty("Ath10kCERingProgramVerifiedCount", (UInt64)_ceRingProgramVerifiedCount, 32);
    setProperty("Ath10kCERingRestoreVerifiedCount", (UInt64)_ceRingRestoreVerifiedCount, 32);
    setProperty("Ath10kCEPipeInitAttempted", _cePipeInitAttempted);
    setProperty("Ath10kCEPipeInitSucceeded", _cePipeInitSucceeded);
    setProperty("Ath10kCEPipesProgrammedAtPublish", _cePipesProgrammed);
    setProperty("Ath10kCEPipeInitPassCount", (UInt64)_cePipeInitPassCount, 32);
    setProperty("Ath10kCEPipeLastVerifiedCount", (UInt64)_cePipeLastVerifiedCount, 32);
    setProperty("Ath10kCEPipeDeinitSucceeded", _cePipeDeinitSucceeded);
    setProperty("Ath10kCEPipeDeinitVerifiedCount", (UInt64)_cePipeDeinitVerifiedCount, 32);
    setProperty("Ath10kCEDiagTransferLimit", (UInt64)Ath10k::kCEDiagTransferLimit, 32);
    setProperty("Ath10kCE7DiagReadAttempted", _ceDiagReadAttempted);
    setProperty("Ath10kCE7DiagReadSucceeded", _ceDiagReadSucceeded);
    setProperty("Ath10kCE7DiagReadOnly", _ceDiagReadOnly);
    setProperty("Ath10kCE7DiagWriteAttempted", _ceDiagWriteAttempted);
    setProperty("Ath10kCE7DiagWriteSucceeded", _ceDiagWriteSucceeded);
    setProperty("Ath10kCE7DiagWriteTransactionCount", (UInt64)_ceDiagWriteTransactionCount, 32);
    setProperty("Ath10kCE7DiagTransferUncertain", _ceDiagTransferUncertain);
    setProperty("Ath10kCE7DiagTransactionCount", (UInt64)_ceDiagTransactionCount, 32);
    setProperty("Ath10kInitConfigAttempted", _initConfigAttempted);
    setProperty("Ath10kInitConfigSucceeded", _initConfigSucceeded);
    setProperty("Ath10kInitConfigNoOpWriteVerified", _initConfigNoOpWriteVerified);
    setProperty("Ath10kInitConfigPipeCfgVerified", _initConfigPipeCfgVerified);
    setProperty("Ath10kInitConfigServiceMapVerified", _initConfigServiceMapVerified);
    setProperty("Ath10kInitConfigConfigFlagsVerified", _initConfigConfigFlagsVerified);
    setProperty("Ath10kInitConfigEarlyAllocVerified", _initConfigEarlyAllocVerified);
    setProperty("Ath10kInitConfigOptionFlag2Verified", _initConfigOptionFlag2Verified);
    setProperty("Ath10kInitConfigRollbackAttempted", _initConfigRollbackAttempted);
    setProperty("Ath10kInitConfigRollbackSucceeded", _initConfigRollbackSucceeded);
    setProperty("Ath10kInitConfigTargetDirty", _initConfigTargetDirty);
    setProperty("Ath10kInitConfigOriginalConfigFlags", (UInt64)_initConfigOriginalConfigFlags, 32);
    setProperty("Ath10kInitConfigAppliedConfigFlags", (UInt64)_initConfigAppliedConfigFlags, 32);
    setProperty("Ath10kInitConfigOriginalEarlyAlloc", (UInt64)_initConfigOriginalEarlyAlloc, 32);
    setProperty("Ath10kInitConfigAppliedEarlyAlloc", (UInt64)_initConfigAppliedEarlyAlloc, 32);
    setProperty("Ath10kInitConfigOriginalOptionFlag2", (UInt64)_initConfigOriginalOptionFlag2, 32);
    setProperty("Ath10kInitConfigAppliedOptionFlag2", (UInt64)_initConfigAppliedOptionFlag2, 32);
    setProperty("Ath10kInitConfigPipeCfgBytes", (UInt64)Ath10k::kQCA6174TargetCEConfigBytes, 32);
    setProperty("Ath10kInitConfigServiceMapBytes", (UInt64)Ath10k::kQCA6174ServiceMapBytes, 32);
    setProperty("Ath10kInitConfigTargetNumBanks", (UInt64)_initConfigTargetNumBanks, 32);
    setProperty("Ath10kInitConfigPersistentAtBMI", _initConfigPersistent);
    setProperty("Ath10kWakeTargetCPUAttempted", _wakeTargetCPUAttempted);
    setProperty("Ath10kWakeTargetCPUSucceeded", _wakeTargetCPUSucceeded);
    setProperty("Ath10kWakeTargetCPUCoreCtrlBefore", (UInt64)_wakeTargetCPUCoreCtrlBefore, 32);
    setProperty("Ath10kWakeTargetCPUCoreCtrlWritten", (UInt64)_wakeTargetCPUCoreCtrlWritten, 32);
    setProperty("Ath10kWakeTargetCPUCoreCtrlAfter", (UInt64)_wakeTargetCPUCoreCtrlAfter, 32);
    setProperty("Ath10kBMIAttempted", _bmiAttempted);
    setProperty("Ath10kBMISucceeded", _bmiSucceeded);
    setProperty("Ath10kBMIGetTargetInfoAttempted", _bmiGetTargetInfoAttempted);
    setProperty("Ath10kBMIGetTargetInfoSucceeded", _bmiGetTargetInfoSucceeded);
    setProperty("Ath10kBMIPipeValidationAttempted", _bmiPipeValidationAttempted);
    setProperty("Ath10kBMIPipeStructuralValidationSucceeded", _bmiPipeStructuralValidationSucceeded);
    setProperty("Ath10kBMIPipeStrictValidationMismatch", _bmiPipeStrictValidationMismatch);
    setProperty("Ath10kBMITransferUncertain", _bmiTransferUncertain);
    setProperty("Ath10kBMITimedOut", _bmiTimedOut);
    setProperty("Ath10kBMIQuiesceAttempted", _bmiQuiesceAttempted);
    setProperty("Ath10kBMIQuiesceSucceeded", _bmiQuiesceSucceeded);
    setProperty("Ath10kDMAQuarantine", _dmaQuarantine);
    setProperty("Ath10kBMIRequestLength", (UInt64)_bmiRequestLength, 32);
    setProperty("Ath10kBMIResponseLength", (UInt64)_bmiResponseLength, 32);
    setProperty("Ath10kBMITargetInfoLength", (UInt64)_bmiTargetInfoLength, 32);
    setProperty("Ath10kBMITargetVersion", (UInt64)_bmiTargetVersion, 32);
    setProperty("Ath10kBMITargetType", (UInt64)_bmiTargetType, 32);
    setProperty("Ath10kBMIWaitUs", (UInt64)_bmiWaitUs, 32);
    setProperty("Ath10kBMICE0SrcWriteIndex", (UInt64)_bmiCE0SrcWrite, 32);
    setProperty("Ath10kBMICE0SrcReadIndex", (UInt64)_bmiCE0SrcRead, 32);
    setProperty("Ath10kBMICE1DstWriteIndex", (UInt64)_bmiCE1DstWrite, 32);
    setProperty("Ath10kBMICE1DstReadIndex", (UInt64)_bmiCE1DstRead, 32);
    setProperty("Ath10kBMICompletedBytes", (UInt64)_bmiCompletedBytes, 32);
    setProperty("Ath10kBMIROMStatus",(UInt64)_bmiROMStatus,32);
    setProperty("Ath10kBMIROMCore",(UInt64)_bmiROMCore,32);
    setProperty("Ath10kBMIROMFW",(UInt64)_bmiROMFW,32);
    setProperty("Ath10kBMIROMDescriptorCPU",(UInt64)_bmiROMDescriptorCPU,32);
    setProperty("Ath10kBMIROMBufferCPU",(UInt64)_bmiROMBufferCPU,32);
    setProperty("Ath10kBMIROMWordLE",(UInt64)_bmiROMWord,32);
    setProperty("Ath10kBMIROMDescriptorAddress",(UInt64)_bmiROMDescriptorAddress,32);
    setProperty("Ath10kBMIROMDescriptorBytes",(UInt64)_bmiROMDescriptorBytes,32);
    setProperty("Ath10kBMIROMDescriptorFlags",(UInt64)_bmiROMDescriptorFlags,32);
    setProperty("Ath10kBMIROMSlot",(UInt64)_bmiROMSlot,32);
    setProperty("Ath10kBMIROMReadCount",(UInt64)_bmiROMReadCount,32);
    setProperty("Ath10kBMIROMCoreRead",_bmiROMCoreRead);
    setProperty("Ath10kBMIROMFWRead",_bmiROMFWRead);
    setProperty("Ath10kBMIROMStable",_bmiROMStable);
    setProperty("Ath10kBMIROMVersion",UInt64(_bmiROMVersion),32);
    setProperty("Ath10kBMIROMType",UInt64(_bmiROMType),32);
    setProperty("Ath10kBMIROMResponseCandidate",_bmiROMResponseCandidate);
    setProperty("Ath10kBMIROMCE1DescriptorCPU",UInt64(_bmiROMCE1DescriptorCPU),32);
    setProperty("Ath10kBMIROMCE1Address",UInt64(_bmiROMCE1Address),32);
    setProperty("Ath10kBMIROMCE1Bytes",UInt64(_bmiROMCE1Bytes),32);
    setProperty("Ath10kBMIROMCE1Flags",UInt64(_bmiROMCE1Flags),32);
    setProperty("Ath10kBMIROMCE1Stable",_bmiROMCE1Stable);
    setProperty("Ath10kBMIEvidenceEntered",_bmiEvidenceEntered);
    setProperty("Ath10kBMIEvidenceCondition",_bmiEvidenceCondition);
    setProperty("Ath10kBMIEvidenceRequestLength",(UInt64)_bmiEvidenceRequestLength,32);
    setProperty("Ath10kBMIEvidenceCommandLE",(UInt64)_bmiEvidenceCommand,32);
    setProperty("Ath10kBMIEvidencePreCaptured",_bmiEvidenceCaptured[0]);
    setProperty("Ath10kBMIEvidencePreTXAddress",(UInt64)_bmiEvidenceDescriptor[0][0],32);
    setProperty("Ath10kBMIEvidencePreTXBytes",(UInt64)_bmiEvidenceDescriptor[0][1],32);
    setProperty("Ath10kBMIEvidencePreTXFlags",(UInt64)_bmiEvidenceDescriptor[0][2],32);
    setProperty("Ath10kBMIEvidencePreRXAddress",(UInt64)_bmiEvidenceDescriptor[0][3],32);
    setProperty("Ath10kBMIEvidencePreRXBytes",(UInt64)_bmiEvidenceDescriptor[0][4],32);
    setProperty("Ath10kBMIEvidencePreRXFlags",(UInt64)_bmiEvidenceDescriptor[0][5],32);
    setProperty("Ath10kBMIEvidencePreCE0SnapshotValid",(UInt64)_bmiEvidenceCE[0][0][0],32);
    setProperty("Ath10kBMIEvidencePreCE0HostStatusReadOK",(UInt64)_bmiEvidenceCE[0][0][1],32);
    setProperty("Ath10kBMIEvidencePreCE0SrcBase",(UInt64)_bmiEvidenceCE[0][0][2],32);
    setProperty("Ath10kBMIEvidencePreCE0SrcSize",(UInt64)_bmiEvidenceCE[0][0][3],32);
    setProperty("Ath10kBMIEvidencePreCE0DstBase",(UInt64)_bmiEvidenceCE[0][0][4],32);
    setProperty("Ath10kBMIEvidencePreCE0DstSize",(UInt64)_bmiEvidenceCE[0][0][5],32);
    setProperty("Ath10kBMIEvidencePreCE0Ctrl1",(UInt64)_bmiEvidenceCE[0][0][6],32);
    setProperty("Ath10kBMIEvidencePreCE0Command",(UInt64)_bmiEvidenceCE[0][0][7],32);
    setProperty("Ath10kBMIEvidencePreCE0HostIE",(UInt64)_bmiEvidenceCE[0][0][8],32);
    setProperty("Ath10kBMIEvidencePreCE0HostIS",(UInt64)_bmiEvidenceCE[0][0][9],32);
    setProperty("Ath10kBMIEvidencePreCE0MiscIE",(UInt64)_bmiEvidenceCE[0][0][10],32);
    setProperty("Ath10kBMIEvidencePreCE0SrcWatermark",(UInt64)_bmiEvidenceCE[0][0][11],32);
    setProperty("Ath10kBMIEvidencePreCE0DstWatermark",(UInt64)_bmiEvidenceCE[0][0][12],32);
    setProperty("Ath10kBMIEvidencePreCE0SrcWrite",(UInt64)_bmiEvidenceCE[0][0][13],32);
    setProperty("Ath10kBMIEvidencePreCE0SrcRead",(UInt64)_bmiEvidenceCE[0][0][14],32);
    setProperty("Ath10kBMIEvidencePreCE0DstWrite",(UInt64)_bmiEvidenceCE[0][0][15],32);
    setProperty("Ath10kBMIEvidencePreCE0DstRead",(UInt64)_bmiEvidenceCE[0][0][16],32);
    setProperty("Ath10kBMIEvidencePreCE1SnapshotValid",(UInt64)_bmiEvidenceCE[0][1][0],32);
    setProperty("Ath10kBMIEvidencePreCE1HostStatusReadOK",(UInt64)_bmiEvidenceCE[0][1][1],32);
    setProperty("Ath10kBMIEvidencePreCE1SrcBase",(UInt64)_bmiEvidenceCE[0][1][2],32);
    setProperty("Ath10kBMIEvidencePreCE1SrcSize",(UInt64)_bmiEvidenceCE[0][1][3],32);
    setProperty("Ath10kBMIEvidencePreCE1DstBase",(UInt64)_bmiEvidenceCE[0][1][4],32);
    setProperty("Ath10kBMIEvidencePreCE1DstSize",(UInt64)_bmiEvidenceCE[0][1][5],32);
    setProperty("Ath10kBMIEvidencePreCE1Ctrl1",(UInt64)_bmiEvidenceCE[0][1][6],32);
    setProperty("Ath10kBMIEvidencePreCE1Command",(UInt64)_bmiEvidenceCE[0][1][7],32);
    setProperty("Ath10kBMIEvidencePreCE1HostIE",(UInt64)_bmiEvidenceCE[0][1][8],32);
    setProperty("Ath10kBMIEvidencePreCE1HostIS",(UInt64)_bmiEvidenceCE[0][1][9],32);
    setProperty("Ath10kBMIEvidencePreCE1MiscIE",(UInt64)_bmiEvidenceCE[0][1][10],32);
    setProperty("Ath10kBMIEvidencePreCE1SrcWatermark",(UInt64)_bmiEvidenceCE[0][1][11],32);
    setProperty("Ath10kBMIEvidencePreCE1DstWatermark",(UInt64)_bmiEvidenceCE[0][1][12],32);
    setProperty("Ath10kBMIEvidencePreCE1SrcWrite",(UInt64)_bmiEvidenceCE[0][1][13],32);
    setProperty("Ath10kBMIEvidencePreCE1SrcRead",(UInt64)_bmiEvidenceCE[0][1][14],32);
    setProperty("Ath10kBMIEvidencePreCE1DstWrite",(UInt64)_bmiEvidenceCE[0][1][15],32);
    setProperty("Ath10kBMIEvidencePreCE1DstRead",(UInt64)_bmiEvidenceCE[0][1][16],32);
    setProperty("Ath10kBMIEvidenceTimeoutCaptured",_bmiEvidenceCaptured[1]);
    setProperty("Ath10kBMIEvidenceTimeoutTXAddress",(UInt64)_bmiEvidenceDescriptor[1][0],32);
    setProperty("Ath10kBMIEvidenceTimeoutTXBytes",(UInt64)_bmiEvidenceDescriptor[1][1],32);
    setProperty("Ath10kBMIEvidenceTimeoutTXFlags",(UInt64)_bmiEvidenceDescriptor[1][2],32);
    setProperty("Ath10kBMIEvidenceTimeoutRXAddress",(UInt64)_bmiEvidenceDescriptor[1][3],32);
    setProperty("Ath10kBMIEvidenceTimeoutRXBytes",(UInt64)_bmiEvidenceDescriptor[1][4],32);
    setProperty("Ath10kBMIEvidenceTimeoutRXFlags",(UInt64)_bmiEvidenceDescriptor[1][5],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE0SnapshotValid",(UInt64)_bmiEvidenceCE[1][0][0],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE0HostStatusReadOK",(UInt64)_bmiEvidenceCE[1][0][1],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE0SrcBase",(UInt64)_bmiEvidenceCE[1][0][2],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE0SrcSize",(UInt64)_bmiEvidenceCE[1][0][3],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE0DstBase",(UInt64)_bmiEvidenceCE[1][0][4],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE0DstSize",(UInt64)_bmiEvidenceCE[1][0][5],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE0Ctrl1",(UInt64)_bmiEvidenceCE[1][0][6],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE0Command",(UInt64)_bmiEvidenceCE[1][0][7],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE0HostIE",(UInt64)_bmiEvidenceCE[1][0][8],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE0HostIS",(UInt64)_bmiEvidenceCE[1][0][9],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE0MiscIE",(UInt64)_bmiEvidenceCE[1][0][10],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE0SrcWatermark",(UInt64)_bmiEvidenceCE[1][0][11],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE0DstWatermark",(UInt64)_bmiEvidenceCE[1][0][12],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE0SrcWrite",(UInt64)_bmiEvidenceCE[1][0][13],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE0SrcRead",(UInt64)_bmiEvidenceCE[1][0][14],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE0DstWrite",(UInt64)_bmiEvidenceCE[1][0][15],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE0DstRead",(UInt64)_bmiEvidenceCE[1][0][16],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE1SnapshotValid",(UInt64)_bmiEvidenceCE[1][1][0],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE1HostStatusReadOK",(UInt64)_bmiEvidenceCE[1][1][1],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE1SrcBase",(UInt64)_bmiEvidenceCE[1][1][2],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE1SrcSize",(UInt64)_bmiEvidenceCE[1][1][3],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE1DstBase",(UInt64)_bmiEvidenceCE[1][1][4],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE1DstSize",(UInt64)_bmiEvidenceCE[1][1][5],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE1Ctrl1",(UInt64)_bmiEvidenceCE[1][1][6],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE1Command",(UInt64)_bmiEvidenceCE[1][1][7],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE1HostIE",(UInt64)_bmiEvidenceCE[1][1][8],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE1HostIS",(UInt64)_bmiEvidenceCE[1][1][9],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE1MiscIE",(UInt64)_bmiEvidenceCE[1][1][10],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE1SrcWatermark",(UInt64)_bmiEvidenceCE[1][1][11],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE1DstWatermark",(UInt64)_bmiEvidenceCE[1][1][12],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE1SrcWrite",(UInt64)_bmiEvidenceCE[1][1][13],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE1SrcRead",(UInt64)_bmiEvidenceCE[1][1][14],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE1DstWrite",(UInt64)_bmiEvidenceCE[1][1][15],32);
    setProperty("Ath10kBMIEvidenceTimeoutCE1DstRead",(UInt64)_bmiEvidenceCE[1][1][16],32);
    setProperty("Ath10kBMIWakeAtTimeout", (UInt64)_bmiWakeAtTimeout, 32);
    const char *miscNames[2][2]={{"Ath10kBMIPreCE0MiscIS","Ath10kBMIPreCE1MiscIS"},{"Ath10kBMITimeoutCE0MiscIS","Ath10kBMITimeoutCE1MiscIS"}};
    const char *miscReadNames[2][2]={{"Ath10kBMIPreCE0MiscReadOK","Ath10kBMIPreCE1MiscReadOK"},{"Ath10kBMITimeoutCE0MiscReadOK","Ath10kBMITimeoutCE1MiscReadOK"}};
    for(UInt32 phase=0;phase<2;++phase)for(UInt32 ce=0;ce<2;++ce){
        setProperty(miscNames[phase][ce],(UInt64)_bmiMiscStatus[phase][ce],32);setProperty(miscReadNames[phase][ce],_bmiMiscRead[phase][ce]);}
    setProperty("Ath10kBMIROMPreStatus",(UInt64)_bmiROMPreStatus,32);
    setProperty("Ath10kBMIROMPreStable",_bmiROMPreStable);
    setProperty("Ath10kBMIROMPreDescriptorCPU",(UInt64)_bmiROMPreDescriptorCPU,32);
    setProperty("Ath10kBMIROMPreBufferCPU",(UInt64)_bmiROMPreBufferCPU,32);
    setProperty("Ath10kBMIROMPreWord0",(UInt64)_bmiROMPreWords[0],32);
    setProperty("Ath10kBMIROMPreWord1",(UInt64)_bmiROMPreWords[1],32);
    setProperty("Ath10kBMIROMPreWord2",(UInt64)_bmiROMPreWords[2],32);
    setProperty("Ath10kBMIRTCAtTimeout", (UInt64)_bmiRTCAtTimeout, 32);
    setProperty("Ath10kBMIReadMemoryAttempted", _bmiReadMemoryAttempted);
    setProperty("Ath10kBMIReadMemorySucceeded", _bmiReadMemorySucceeded);
    setProperty("Ath10kBMIReadMemoryTransactionCount", (UInt64)_bmiReadMemoryTransactionCount, 32);
    setProperty("Ath10kBMIBoardPrereqAttempted", _bmiBoardPrereqAttempted);
    setProperty("Ath10kBMIBoardPrereqSucceeded", _bmiBoardPrereqSucceeded);
    setProperty("Ath10kBMIInterconnectMatchesDiag", _bmiInterconnectMatchesDiag);
    setProperty("Ath10kBMIEarlyCfgDoneSeen", _bmiEarlyCfgDoneSeen);
    setProperty("Ath10kBMIBoardDataPointerValid", _bmiBoardDataPointerValid);
    setProperty("Ath10kBMIBoardExtDataValid", _bmiBoardExtDataValid);
    setProperty("Ath10kBMIDesiredCPUSpeed", (UInt64)_bmiDesiredCPUSpeed, 32);
    setProperty("Ath10kBMIBoardDataAddress", (UInt64)_bmiBoardDataAddress, 32);
    setProperty("Ath10kBMIBoardDataInitialized", (UInt64)_bmiBoardDataInitialized, 32);
    setProperty("Ath10kBMIBoardExtDataAddress", (UInt64)_bmiBoardExtDataAddress, 32);
    setProperty("Ath10kBMIBoardExtDataConfig", (UInt64)_bmiBoardExtDataConfig, 32);
    setProperty("Ath10kBMIBoardExtDataSize", (UInt64)_bmiBoardExtDataSize, 32);
    setProperty("Ath10kBMICalDataAddress", (UInt64)_bmiCalDataAddress, 32);
    setProperty("Ath10kBMIInterconnectState", (UInt64)_bmiInterconnectState, 32);
    setProperty("Ath10kBMIOptionFlag2", (UInt64)_bmiOptionFlag2, 32);
    setProperty("Ath10kBMIBoardFingerprint0", (UInt64)_bmiBoardFingerprint[0], 32);
    setProperty("Ath10kBMIBoardFingerprint1", (UInt64)_bmiBoardFingerprint[1], 32);
    setProperty("Ath10kBMIBoardFingerprint2", (UInt64)_bmiBoardFingerprint[2], 32);
    setProperty("Ath10kBMIBoardFingerprint3", (UInt64)_bmiBoardFingerprint[3], 32);
    if (_bmiRequestBuffer.allocated())
        setProperty("Ath10kBMIRequestIOVM", (UInt64)_bmiRequestBuffer.dmaAddress(), 64);
    if (_bmiResponseBuffer.allocated())
        setProperty("Ath10kBMIResponseIOVM", (UInt64)_bmiResponseBuffer.dmaAddress(), 64);
    setProperty("Ath10kHostInterestEarlyAllocAddress", (UInt64)Ath10k::kHostInterestEarlyAllocAddress, 32);
    setProperty("Ath10kHostInterestOptionFlag2Address", (UInt64)Ath10k::kHostInterestOptionFlag2Address, 32);
    setProperty("Ath10kHostInterestDesiredCPUSpeedAddress", (UInt64)Ath10k::kHostInterestDesiredCPUSpeedAddress, 32);
    setProperty("Ath10kHostInterestBoardDataAddress", (UInt64)Ath10k::kHostInterestBoardDataAddress, 32);
    setProperty("Ath10kHostInterestBoardDataInitAddress", (UInt64)Ath10k::kHostInterestBoardDataInitAddress, 32);
    setProperty("Ath10kHostInterestBoardExtDataAddress", (UInt64)Ath10k::kHostInterestBoardExtDataAddress, 32);
    setProperty("Ath10kHostInterestBoardExtConfigAddress", (UInt64)Ath10k::kHostInterestBoardExtConfigAddress, 32);
    setProperty("Ath10kHostInterestCalDataAddress", (UInt64)Ath10k::kHostInterestCalDataAddress, 32);
    setProperty("Ath10kCE7DiagCoreCtrl", (UInt64)_ceDiagCoreCtrl, 32);
    setProperty("Ath10kCE7DiagLastTargetAddress", (UInt64)_ceDiagLastTargetAddress, 32);
    setProperty("Ath10kCE7DiagLastCEAddress", (UInt64)_ceDiagLastCEAddress, 32);
    setProperty("Ath10kCE7DiagLastCompletedBytes", (UInt64)_ceDiagLastCompletedBytes, 32);
    setProperty("Ath10kCE7DiagLastWaitUs", (UInt64)_ceDiagLastWaitUs, 32);
    setProperty("Ath10kCE7DiagLastSrcWriteIndex", (UInt64)_ceDiagLastSrcWriteIndex, 32);
    setProperty("Ath10kCE7DiagLastDstWriteIndex", (UInt64)_ceDiagLastDstWriteIndex, 32);
    setProperty("Ath10kCE7DiagLastSrcReadIndex", (UInt64)_ceDiagLastSrcReadIndex, 32);
    setProperty("Ath10kCE7DiagLastDstReadIndex", (UInt64)_ceDiagLastDstReadIndex, 32);
    setProperty("Ath10kHostInterestInterconnectAddress",
                (UInt64)Ath10k::kHostInterestInterconnectAddress, 32);
    setProperty("Ath10kPCIeStateTargetAddress", (UInt64)_ceDiagPCIeStateAddress, 32);
    setProperty("Ath10kPCIePipeCfgTargetAddress", (UInt64)_ceDiagPipeCfgAddress, 32);
    setProperty("Ath10kPCIeServiceMapTargetAddress", (UInt64)_ceDiagServiceMapAddress, 32);
    setProperty("Ath10kPCIeConfigFlags", (UInt64)_ceDiagConfigFlags, 32);
    if (_ceDiagBounce.allocated()) {
        setProperty("Ath10kCE7DiagBounceIOVM", (UInt64)_ceDiagBounce.dmaAddress(), 64);
        setProperty("Ath10kCE7DiagBouncePhysical",
                    (UInt64)_ceDiagBounce.physicalAddress(), 64);
    }
    if (_ceSrcRings[7].allocated())
        setProperty("Ath10kCE7SourceRingIOVM", (UInt64)_ceSrcRings[7].dmaAddress(), 64);
    if (_ceDstRings[7].allocated())
        setProperty("Ath10kCE7DestinationRingIOVM", (UInt64)_ceDstRings[7].dmaAddress(), 64);
    if (_ceSrcRings[4].allocated()) {
        setProperty("Ath10kCE4SourceRingIOVM", (UInt64)_ceSrcRings[4].dmaAddress(), 64);
        setProperty("Ath10kCE4SourceRingBytes", (UInt64)_ceSrcRings[4].bytes(), 64);
    }
    setProperty("Ath10kDMATestAttempted", _dmaTestAttempted);
    setProperty("Ath10kDMATestSucceeded", _dmaTestSucceeded);
    if (_dmaTestBuffer.allocated()) {
        setProperty("Ath10kDMATestPhysical", (UInt64)_dmaTestBuffer.physicalAddress(), 64);
        setProperty("Ath10kDMATestIOVM", (UInt64)_dmaTestBuffer.dmaAddress(), 64);
        setProperty("Ath10kDMATestSegmentLength", (UInt64)_dmaTestBuffer.segmentLength(), 64);
        setProperty("Ath10kDMATestLength", (UInt64)_dmaTestBuffer.length(), 64);
        setProperty("Ath10kDMATestAlignment", (UInt64)_dmaTestBuffer.alignment(), 64);
    }
    setProperty("Ath10kHasPM", _hasPM);
    setProperty("Ath10kHasMSI", _hasMSI);
    setProperty("Ath10kHasMSIX", _hasMSIX);
    setProperty("Ath10kHasPCIe", _hasPCIe);
    setProperty("vendor-id-value", (UInt64)_vendorId, 16);
    setProperty("device-id-value", (UInt64)_deviceId, 16);
    setProperty("subsystem-vendor-id-value", (UInt64)_subsystemVendorId, 16);
    setProperty("subsystem-id-value", (UInt64)_subsystemId, 16);
    setProperty("pci-revision-id-value", (UInt64)_revisionId, 8);
    setProperty("pci-class-code", (UInt64)_classCode, 8);
    setProperty("pci-subclass", (UInt64)_subclass, 8);
    setProperty("pci-prog-if", (UInt64)_progIf, 8);
    setProperty("pci-interrupt-line", (UInt64)_interruptLine, 8);
    setProperty("pci-interrupt-pin", (UInt64)_interruptPin, 8);
    if (_bar0Map) {
        setProperty("bar0-physical", (UInt64)_bar0Map->getPhysicalAddress(), 64);
        setProperty("bar0-length", (UInt64)_bar0Map->getLength(), 64);
    }
    setName(chipName());
}
