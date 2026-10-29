#pragma once

#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/IOInterrupts.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOLocks.h>
#include "Ath10kPerformance.hpp"
#include "Ath10kPower.hpp"
#include <IOKit/IONotifier.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include "Ath10kHw.hpp"
#include "Ath10kPayloadProfile.hpp"
#include "Ath10kRegisterIO.hpp"
#include "Ath10kTarget.hpp"
#include "Ath10kRuntimePower.hpp"
#include "Ath10kCE.hpp"
#include "Ath10kCERing.hpp"
#include "Ath10kDMA.hpp"
#include "Ath10kBMI.hpp"
#include "Ath10kPLL.hpp"
#include "Ath10kBoot.hpp"
#include "Ath10kWMI.hpp"
#include "Ath10kRxSetup.hpp"
#include "Ath10kRX.hpp"
#include <IOKit/IOUserClient.h>
#include "../shared/Ath10kControl.hpp"

class Ath10kPCIDevice : public IOEthernetController
{
    OSDeclareDefaultStructors(Ath10kPCIDevice)

public:
    IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
    IOReturn controlRequest(IOUserClient *,const AthControl::Request *,AthControl::Response *);
    void closeControl(IOUserClient *);
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOReturn setPowerState(unsigned long state, IOService *device) override;
    void systemWillShutdown(IOOptionBits specifier) override;

private:
    const Ath10kPayload::Profile &payload() const {
        return Ath10kPayload::selectProfile(_vendorId, _deviceId, _subsystemVendorId,
            _subsystemId, _selectedHw ? _selectedHw->chipRevision : 0xff);
    }
    bool payloadMatchesTarget() const {
        return payload().matchesTarget(_bmiTargetVersion, _bmiTargetType, _bmiTargetInfoLength);
    }
    IOUserClient *_controlOwner=nullptr;
    bool _reprobeRequested=false,_systemSuspended=false;
    bool requestDeviceRestart();
    Ath10k::PowerState _power{};
    // Atomic: 0 active, 1 suspending/suspended, 2 awake but clean restart required.
    volatile UInt32 _powerIOBlock=0;
    IONotifier *_powerNotifier=nullptr;
    bool preparePowerNotifications();
    void releasePowerNotifications();
    bool powerIOBlocked();
    bool powerRestartAllowed();
    void requestPowerEvent(Ath10k::PowerEvent);
    void publishPowerState();
    static IOReturn controlPowerState(OSObject *,void *,void *,void *,void *);
    static IOReturn powerNotification(void *,void *,UInt32,IOService *,void *,vm_size_t);
    void fillControlInfo(AthControl::Response &);
    static IOReturn controlAction(OSObject *,void *,void *,void *,void *);
    bool configureInterface(IONetworkInterface *) override;
    const OSString *newModelString() const override;
    bool createWorkLoop() override;
    IOWorkLoop *getWorkLoop() const override;
    IOOutputQueue *createOutputQueue() override;
    UInt32 getFeatures() const override;
    IOReturn getHardwareAddress(IOEthernetAddress *) override;
    IOReturn selectMedium(const IONetworkMedium *) override;
    IOReturn enable(IONetworkInterface *) override;
    IOReturn disable(IONetworkInterface *) override;
    IOReturn setPromiscuousMode(bool) override;
    IOReturn setMulticastMode(bool) override;
    IOReturn setMulticastList(IOEthernetAddress *,UInt32) override;
    static IOReturn multicastAction(OSObject *,void *,void *,void *,void *);
    UInt32 outputPacket(mbuf_t,void *) override;
    bool prepareNetwork();
    bool prepareRXReserve();
    void pauseRXReserve();
    void releaseRXReserve();
    void refillRXReserve();
    mbuf_t takeRXPacket(UInt32);
    static void rxReserveTimerFired(OSObject *,IOTimerEventSource *);
    static IOReturn stopRXReserve(OSObject *,void *,void *,void *,void *);
    void flushNetworkRX();
    bool _rxDeliveryBatch=false;
    UInt32 _rxDeliveryQueued=0;
    IOWorkLoop *_rxReserveWorkLoop=nullptr;
    IOTimerEventSource *_rxReserveTimer=nullptr;
    IOSimpleLock *_rxReserveLock=nullptr;
    bool _rxReserveAdded=false;
    mbuf_t _rxReserve[Ath10k::kRXReserveCapacity]{};
    UInt32 _rxReserveCount=0;
    Ath10k::PerformanceState _perf{};
    bool _dataTXStalled=false;
    void networkLink(bool);
    void publishConnection();
    void deliverNetworkRX(const UInt8 *,UInt8,UInt16,UInt16 indicatedLength=0,UInt32 availableBytes=2048);
    const char *serviceConnection(UInt64);
    const char *serviceDisconnect(UInt64);
    const char *serviceChannelSwitch(UInt64);
    const char *serviceRadioMaintenance(UInt64);
    const char *serviceAPMaintenance(UInt64);
    const char *serviceStatsMaintenance(UInt64);
    const char *serviceDataTX(UInt64);
    const char *submitDataPacket(const UInt8 *,UInt32,UInt64);
    struct DataBuffers {Ath10kDMABuffer payload{},command{},fragments{};};
    DataBuffers _dataBuffers[Ath10k::kDataTxSlots]{};
    const char *submitNetworkCE(UInt32,Ath10kDMABuffer &,UInt32,UInt32 &);
    const char *submitNetworkPacket(const UInt8 *,UInt32,bool,UInt64);
    IOWorkLoop *_networkWorkLoop=nullptr;
    IONetworkInterface *_networkInterface=nullptr;
    bool _networkEnabled=false,_networkLinkUp=false;
    Ath10k::ConnectionState _connection{};
    Ath10k::WMIEchoState _connectionEcho{};
    Ath10kDMABuffer _connectionCommand{},_packetBuffer{},_packetCommand{},_packetFragments{};
    bool identifyDevice();
    bool prepareDeviceDMAMapper();
    bool stopDMAForRelease();
    void releaseDeviceDMAMapper();
    bool mapBAR0();
    void unmapBAR0();
    void dumpPCIConfig();
    void dumpPCICapabilities();
    void dumpHardwareCandidates();
    void validateRegisterLayer();

    bool preparePCIeBringupParity();
    void restorePCIeBringupParity();
    bool disableASPMForBringup();
    bool aspmWriteIsVirtualized() const;
    void restoreASPM();
    bool prepareIRQParity();
    void teardownIRQParity();
    void logMSIState(const char *stage);
    static void interruptHandler(OSObject *target, void *refCon,
                                 IOService *nub, int source);

    bool startRuntimeCEIRQ();
    bool stopRuntimeCEIRQ();
    bool htcRepostRuntimeReceive();
    bool htcSubmitControlFrame(const UInt8 *request, UInt32 requestBytes,
                               bool expectResponse, UInt16 expectedMessageId,
                               UInt8 *response, UInt32 &responseBytes);
    bool runHTCServiceSetup();
    bool runWMIBootstrap();
    bool repostWMIBuffer();
    bool runWMIInit();
    const char *serviceWMIEcho(UInt64 nowMs);
    const char *serviceWMIVdevCreate(UInt64 nowMs);
    Ath10k::WMIVdevCreateState _wmiVdevCreate{};
    const char *serviceWMIScanChannels(UInt64 nowMs);
    Ath10k::WMIScanChannelsState _wmiScanChannels{};
    Ath10k::RegulatoryPolicy _regulatory{};
    Ath10k::WMIScanChannelsState _wmiRadio[5]{};
    const char *serviceWMIRadio(UInt64 nowMs, UInt32 index);
    const char *serviceRxSetup(UInt64 nowMs);
    const char *sampleHTTRxPool();
    const char *serviceHTTRx();
    const char *serviceHTTRxOne(bool &);
    Ath10k::RxSetupState _rxSetup{};
    Ath10k::WMIEchoState _filterEcho{};
    Ath10kDMABuffer _httRecycled[Ath10k::kHTTRxPosted]{};
    Ath10kDMABuffer _httTxBuffer{},_httRxIndex{},_httRxRing{},_httRxPool{};
    const char *serviceWMIScan(UInt64 nowMs);
    Ath10k::WMIScanState _wmiScan{};
    Ath10k::WMIBSSCache _wmiBSS{};
    Ath10k::WMIEchoState _wmiEcho{};
    bool prepareWMIRuntime();
    const char *drainWMIRuntime();
    void endWMIRuntime(const char *reason);
    void releaseWMIRuntime();
    void publishWMIRuntimeProperties();
    static void wmiTimerFired(OSObject *owner, IOTimerEventSource *timer);
    const char *serviceRuntime();
    const char *startRuntimeWorkerIRQ();
    void stopRuntimeWorkerIRQ();
    bool maskRuntimeWorkerIRQ();
    bool unmaskRuntimeWorkerIRQ();
    bool maskRuntimeWorkerIRQLocked();
    bool unmaskRuntimeWorkerIRQLocked();
    IOSimpleLock *_runtimeIRQLock=nullptr;
    UInt32 _runtimeIRQUnmaskStep=0,_runtimeIRQUnmaskEnableValue=0;
    static bool runtimeIRQFilter(OSObject *,IOFilterInterruptEventSource *);
    static void runtimeIRQAction(OSObject *,IOInterruptEventSource *,int);
    bool prepareRuntimePower();
    bool finishRuntimePower();
    const char *idleRuntimePower(bool pending,bool &asleep);
    static bool runtimeMMIOBegin(void *,UInt32,UInt64 &);
    static void runtimeMMIOEnd(void *,UInt32,UInt64);
    bool runtimeASPM(bool enabled);
    void publishRuntimePower();
    IOSimpleLock *_runtimeWakeLock=nullptr;
    Ath10k::RuntimePowerState _runtimePower{};
    bool _runtimePowerCleanup=false,_runtimeASPM=false;
    IOFilterInterruptEventSource *_runtimeIRQ=nullptr;
    bool _runtimeIRQAdded=false;
    volatile bool _runtimeIRQArmed=false,_runtimeIRQMaskFailed=false;
    bool _runtimeIRQDeferred=false;
    UInt64 _nextRxDoneRetryMs=0;
    UInt32 _lastMaintenanceRxPackets=0,_lastMaintenanceTxPackets=0;
    UInt32 _runtimeIRQWorkers=0,_runtimeIRQReschedules=0;
    static IOReturn controlWMIRuntime(OSObject *owner, void *reason, void *, void *, void *);
    IOWorkLoop *_wmiWorkLoop = nullptr;
    IOTimerEventSource *_wmiTimer = nullptr;
    bool _wmiTimerAdded = false, _powerManaged = false;
    Ath10k::WMIRuntimeState _wmiLive{};
    Ath10k::WMIInitState _wmiEvents{};

    bool probeTarget();
    bool wakeTarget(bool &assertedWake);
    void releaseTargetWake(bool assertedWake);
    bool coldResetTarget();
    bool waitForTargetInit();
    bool readPostResetChipId();
    void clearTargetProbeState();

    bool warmResetTarget();
    bool maskTargetIRQsForWarmReset();
    bool warmResetSI0();
    bool warmResetCPU(const char *stage);
    bool warmResetCE();
    bool warmResetClearLF();

    bool inspectCopyEngines();
    void clearCEState();

    bool allocateCERings();
    void releaseCERings();
    bool validateCE0PostResetProgramming();
    bool initCopyEnginePipes(const char *stage);
    bool initOneCopyEnginePipe(UInt32 ceId, const char *stage);
    bool verifyCopyEnginePipe(UInt32 ceId, const Ath10k::CESnapshot &snapshot) const;
    bool deinitCopyEnginePipes(const char *stage);
    bool deinitOneCopyEnginePipe(UInt32 ceId);

    bool runCE7DiagnosticReadTest();
    bool runCE7InitConfigPersistentTest();
    bool wakeTargetCPUForBMI();
    void captureBMIROMEvidence(bool beforeSubmit=false);
    bool runBMIGetTargetInfoTest();
    bool bmiReadSOCRegister(UInt32,UInt32 &);
    bool bmiWriteSOCRegister(UInt32,UInt32);
    bool bmiWritePLLWord(UInt32,UInt32);
    bool configureTargetPLL();
    Ath10k::PLLState _pll{};
    bool bmiReadMemory(UInt32 targetAddress, void *outData, UInt32 length);
    bool bmiSendOnly(const void *request, UInt32 requestLength);
    bool bmiWriteMemory(UInt32 targetAddress, const void *data, UInt32 length);
    bool runBMIBoardUploadTest();
    bool bmiLZStreamStart(UInt32 address);
    bool bmiLZData(const UInt8 *data, UInt32 length);
    bool runBMIOtpTest();
    bool bmiFirmwareLZStart(UInt32 address);
    bool bmiFirmwareLZData(const UInt8 *data, UInt32 length);
    bool verifyFirmwareSegments();
    bool runBMIFirmwareDownloadTest();
    bool writeBootHIWord(UInt32 address, UInt32 value);
    bool verifyBootHIWord(UInt32 address, UInt32 value);
    bool verifyBootHIOptionsAfterDownload();
    bool configureBootHostInterest();
    bool configureBootFirmwareDownload();
    bool finishBootHostInterest();
    bool prepareHTCReadyReceive();
    bool waitForHTCReady();
    bool runFirmwareBootTest();
    bool runBMIBoardPrerequisiteProbe();
    bool bmiExchange(const void *request, UInt32 requestLength,
                     void *response, UInt32 &responseLength);
    bool validateBMIPipeStructure(UInt32 ceId, const Ath10k::CESnapshot &snapshot) const;
    bool quiesceTargetAfterBMI();
    bool pulseColdResetForQuiesce();
    bool ce7DiagnosticRead(UInt32 targetAddress, void *outData, UInt32 nbytes);
    bool ce7DiagnosticWrite(UInt32 targetAddress, const void *data, UInt32 nbytes);
    bool targetCpuToCEAddress(UInt32 targetAddress, UInt32 &ceAddress);

    bool programAndVerifyOneCE(UInt32 ceId);
    bool restoreOneCE(UInt32 ceId, const Ath10k::CESnapshot &baseline);
    bool verifyOneCEProgrammed(UInt32 ceId, const Ath10k::CESnapshot &snapshot) const;
    bool verifyOneCERestored(UInt32 ceId, const Ath10k::CESnapshot &baseline,
                             const Ath10k::CESnapshot &restored) const;

    bool runDMATest();
    // Diagnostic publisher is distinct from IOEthernetController::publishProperties(),
    // whose bool-returning implementation must remain inherited.
    void publishDiagnosticProperties();
    void finishNetworkRX();
    void releaseRXBatch(Ath10k::RXBatch &batch);
    void serviceRXReorder(UInt64 nowMs);
    Ath10k::RXBatch _rxBatch{};
    Ath10k::RXReorder _rxReorder{};
    Ath10k::RXDefrag _rxDefrag{};
    UInt8 _rxAssembly[Ath10k::kRXAssemblyBytes]{};
    UInt8 _rxNormalized[Ath10k::kRXAssemblyBytes]{};
    const UInt8 *_rxServiceData[256]{};
    const char *chipName() const;

    IOPCIDevice *_pci = nullptr;
    IOMapper *_dmaMapper = nullptr;
    bool _dmaMapperAttempted = false;
    bool _dmaMapperReady = false; // mapping policy ready; mapper may legitimately be null
    bool _dmaDeviceMapperAvailable = false;
    bool _dmaMapperUsedByRings = false;
    bool _dmaMapperUsedByDiag = false;
    bool _dmaMapperUsedByTest = false;
    IOMemoryMap *_bar0Map = nullptr;
    volatile UInt8 *_bar0 = nullptr;
    Ath10kRegisterIO _regs;

    UInt16 _vendorId = 0;
    UInt16 _deviceId = 0;
    UInt16 _subsystemVendorId = 0;
    UInt16 _subsystemId = 0;
    UInt16 _originalCommand = 0;
    UInt8  _revisionId = 0;
    UInt8  _classCode = 0;
    UInt8  _subclass = 0;
    UInt8  _progIf = 0;
    UInt8  _interruptLine = 0;
    UInt8  _interruptPin = 0;

    bool _hasPM = false;
    bool _hasMSI = false;
    bool _hasMSIX = false;
    bool _hasPCIe = false;
    UInt8 _pmCapOffset = 0;
    UInt8 _msiCapOffset = 0;
    UInt8 _msixCapOffset = 0;
    UInt8 _pcieCapOffset = 0;

    bool _pcieParityAttempted = false;
    bool _pcieParityPrepared = false;
    bool _pcieParityRestored = false;
    bool _aspmDisableAttempted = false;
    bool _aspmDisabled = false;
    bool _aspmRestoreSucceeded = false;
    UInt16 _pcieLinkControlBefore = 0xffffu;
    UInt16 _pcieLinkControlDuring = 0xffffu;
    UInt16 _pcieLinkControlAfterRestore = 0xffffu;

    bool _irqPrepareAttempted = false;
    bool _irqPrepared = false;
    bool _irqRegistered = false;
    bool _irqEnabled = false;
    bool _irqMessaged = false;
    bool _intxConfigChanged=false;
    UInt16 _intxCommandBefore=0,_intxMSIBefore=0;
    SInt32 _irqIndex = -1;
    UInt32 _irqType = 0;
    volatile UInt32 _irqObservedCount = 0;
    UInt16 _msiControlBefore = 0xffffu;
    UInt16 _msiControlAfterRegister = 0xffffu;
    UInt16 _msiControlPostReset = 0xffffu;
    UInt16 _msiControlAfterWait = 0xffffu;

    bool _targetProbeAttempted = false;
    bool _targetWakeSucceeded = false;
    bool _postResetWakeSucceeded = false;
    bool _targetWasAlreadyAwake = false;
    bool _chipIdValid = false;
    UInt32 _rtcStateBefore = 0xffffffffu;
    UInt32 _rtcStateAfter = 0xffffffffu;
    UInt32 _wakeDelayUs = 0;
    UInt32 _chipId = 0xffffffffu;
    UInt8 _chipRevision = 0xffu;
    const Ath10k::HwCandidate *_selectedHw = nullptr;

    bool _coldResetAttempted = false;
    bool _coldResetSucceeded = false;
    UInt32 _socGlobalResetBefore = 0xffffffffu;
    UInt32 _socGlobalResetAfter = 0xffffffffu;
    bool _targetInitWaitAttempted = false;
    bool _targetInitWaitSucceeded = false;
    UInt32 _targetInitWaitMs = 0;
    UInt32 _fwIndicatorBefore = 0xffffffffu;
    UInt32 _fwIndicatorAfter = 0xffffffffu;
    bool _fwIndicatorEventPending = false;
    bool _fwIndicatorNonZeroSeen = false;
    UInt32 _fwIndicatorSampleCount = 0;
    UInt32 _fwIndicatorChangeCount = 0;

    bool _coldTargetInitSucceeded = false;
    UInt32 _coldTargetInitMs = 0;
    UInt32 _coldFWIndicator = 0xffffffffu;

    bool _warmResetAttempted = false;
    bool _warmResetSucceeded = false;
    bool _warmResetIRQMasked = false;
    bool _warmResetSI0Succeeded = false;
    bool _warmResetCPU1Succeeded = false;
    bool _warmResetCPU2Succeeded = false;
    bool _warmResetCESucceeded = false;
    bool _warmResetLFCleared = false;
    bool _warmResetFirstTargetInitSucceeded = false;
    bool _warmResetSecondTargetInitSucceeded = false;
    UInt32 _warmResetFirstTargetInitMs = 0;
    UInt32 _warmResetSecondTargetInitMs = 0;
    UInt32 _warmResetFirstFWIndicator = 0xffffffffu;
    UInt32 _warmResetSecondFWIndicator = 0xffffffffu;
    UInt32 _warmResetControlBefore = 0xffffffffu;
    UInt32 _warmResetControlAfter = 0xffffffffu;
    UInt32 _warmResetLFBefore = 0xffffffffu;
    UInt32 _warmResetLFAfter = 0xffffffffu;
    UInt32 _postWarmChipId = 0xffffffffu;
    UInt8 _postWarmChipRevision = 0xffu;

    Ath10k::CESnapshot _ceSnapshots[Ath10k::kCECount]{};
    Ath10k::CESnapshot _ceBaselineSnapshots[Ath10k::kCECount]{};
    bool _ceInspectionAttempted = false;
    bool _ceInspectionSucceeded = false;
    UInt32 _ceValidCount = 0;
    UInt32 _ceWrapperInterruptSummary = 0xffffffffu;

    Ath10kCERing _ceSrcRings[Ath10k::kCECount];
    Ath10kCERing _ceDstRings[Ath10k::kCECount];
    bool _ceRingAllocationAttempted = false;
    bool _ceRingAllocationSucceeded = false;
    UInt32 _ceRingAllocatedCount = 0;
    UInt64 _ceRingAllocatedBytes = 0;
    UInt32 _ceRingSyncOutCount = 0;
    UInt32 _ceRingSyncOutFailureCount = 0;

    bool _ceRingProgramAttempted = false;
    bool _ceRingProgramSucceeded = false;
    bool _ceRingRestoreSucceeded = false;
    UInt32 _ceRingProgramVerifiedCount = 0;
    UInt32 _ceRingRestoreVerifiedCount = 0;

    bool _cePipeInitAttempted = false;
    bool _cePipeInitSucceeded = false;
    bool _cePipesProgrammed = false;
    bool _cePipeDeinitSucceeded = false;
    UInt32 _cePipeInitPassCount = 0;
    UInt32 _cePipeLastVerifiedCount = 0;
    UInt32 _cePipeDeinitVerifiedCount = 0;

    Ath10kDMABuffer _ceDiagBounce;
    bool _ceDiagReadAttempted = false;
    bool _ceDiagReadSucceeded = false;
    bool _ceDiagReadOnly = true;
    bool _ceDiagTransferUncertain = false;
    UInt32 _ceDiagTransactionCount = 0;
    UInt32 _ceDiagCoreCtrl = 0xffffffffu;
    UInt32 _ceDiagLastTargetAddress = 0xffffffffu;
    UInt32 _ceDiagLastCEAddress = 0xffffffffu;
    UInt32 _ceDiagLastCompletedBytes = 0;
    UInt32 _ceDiagLastWaitUs = 0;
    UInt32 _ceDiagLastSrcWriteIndex = 0xffffffffu;
    UInt32 _ceDiagLastDstWriteIndex = 0xffffffffu;
    UInt32 _ceDiagLastSrcReadIndex = 0xffffffffu;
    UInt32 _ceDiagLastDstReadIndex = 0xffffffffu;
    bool _ceDiagPreSubmitSyncSucceeded = false;
    bool _ceDiagCompletionSyncSucceeded = false;
    UInt32 _ceDiagSyncOutCount = 0;
    UInt32 _ceDiagSyncInCount = 0;
    UInt32 _ceDiagSyncFailureCount = 0;
    UInt32 _ceDiagPCIeStateAddress = 0xffffffffu;
    UInt32 _ceDiagPipeCfgAddress = 0xffffffffu;
    UInt32 _ceDiagServiceMapAddress = 0xffffffffu;
    UInt32 _ceDiagConfigFlags = 0xffffffffu;

    bool _ceDiagWriteAttempted = false;
    bool _ceDiagWriteSucceeded = false;
    UInt32 _ceDiagWriteTransactionCount = 0;
    bool _initConfigAttempted = false;
    bool _initConfigSucceeded = false;
    bool _initConfigNoOpWriteVerified = false;
    bool _initConfigPipeCfgVerified = false;
    bool _initConfigServiceMapVerified = false;
    bool _initConfigConfigFlagsVerified = false;
    bool _initConfigEarlyAllocVerified = false;
    bool _initConfigOptionFlag2Verified = false;
    bool _initConfigRollbackAttempted = false;
    bool _initConfigRollbackSucceeded = false;
    bool _initConfigTargetDirty = false;
    UInt32 _initConfigOriginalConfigFlags = 0xffffffffu;
    UInt32 _initConfigAppliedConfigFlags = 0xffffffffu;
    UInt32 _initConfigOriginalEarlyAlloc = 0xffffffffu;
    UInt32 _initConfigAppliedEarlyAlloc = 0xffffffffu;
    UInt32 _initConfigOriginalOptionFlag2 = 0xffffffffu;
    UInt32 _initConfigAppliedOptionFlag2 = 0xffffffffu;
    UInt32 _initConfigTargetNumBanks = 0;
    bool _initConfigPersistent = false;

    bool _wakeTargetCPUAttempted = false;
    bool _wakeTargetCPUSucceeded = false;
    UInt32 _wakeTargetCPUCoreCtrlBefore = 0xffffffffu;
    UInt32 _wakeTargetCPUCoreCtrlWritten = 0xffffffffu;
    UInt32 _wakeTargetCPUCoreCtrlAfter = 0xffffffffu;

    Ath10kDMABuffer _bmiRequestBuffer;
    Ath10kDMABuffer _bmiResponseBuffer;
    Ath10kDMABuffer _htcReadyBuffer;
    Ath10kDMABuffer _htcControlBuffer;
    Ath10kDMABuffer _wmiRxBuffer;
    Ath10k::WMIBootstrapState _wmi{};
    Ath10k::WMIInitState _wmiInit{};
    Ath10kDMABuffer _wmiHostMemory[Ath10k::kWMIHostMemoryMax];
    Ath10kDMABuffer _wmiTxBuffer;
    Ath10k::BootState _boot{};

    // Phase 3 step 2: HTC control transport and the first service endpoint
    // assignments. Credit accounting is intentionally deferred to step 3.
    bool _htcAttempted = false;
    bool _htcSetupCompleteSent = false;
    bool _htcReservedControlConnected = false;
    bool _htcServiceSetupSucceeded = false;
    bool _htcControlExchangeUncertain = false;
    bool _htcCreditFlowDeferred = true;
    UInt32 _htcControlTxCount = 0;
    UInt32 _htcControlRxCount = 0;
    UInt32 _htcControlLastTxBytes = 0;
    UInt32 _htcControlLastRxBytes = 0;
    UInt32 _htcControlLastTxSlot = 0xffffffffu;
    UInt32 _htcControlLastTxNext = 0xffffffffu;
    UInt32 _htcControlLastRxSlot = 0xffffffffu;
    UInt32 _htcControlLastRxNext = 0xffffffffu;
    UInt32 _htcControlLastWaitMs = 0;
    const char *_htcStatus = "not-run";
    Ath10k::HTCServiceInfo _htcReservedControl{};
    Ath10k::HTCServiceInfo _htcWMIControl{};
    Ath10k::HTCServiceInfo _htcHTTData{};

    // Phase 3 runtime: keep the post-BMI firmware alive with a persistent CE1
    // destination buffer and a real host interrupt path. Step 2 uses this
    // same single-buffer control path for HTC service setup; credit accounting,
    // WMI and HTT payloads remain later steps.
    bool _runtimeAttempted = false;
    bool _runtimeActive = false;
    bool _runtimeCE1Armed = false;
    bool _runtimeInterruptsEnabled = false;
    bool _runtimeWakeAsserted = false;
    UInt32 _runtimeRxSlot = 0xffffffffu;
    UInt32 _runtimeRxNext = 0xffffffffu;
    UInt32 _runtimeRxPostedCount = 0;
    UInt32 _runtimeRxCompletedCount = 0;
    UInt32 _runtimeRxReplenishedCount = 0;
    UInt32 _runtimeLastCE1Read = 0xffffffffu;
    UInt32 _runtimeLastCE1Write = 0xffffffffu;
    UInt32 _runtimeLastRxBytes = 0;
    UInt32 _runtimeLastHostIE = 0xffffffffu;
    UInt32 _runtimeLastMiscIE = 0xffffffffu;
    UInt32 _runtimeLastPCIeIntrEnable = 0xffffffffu;
    UInt32 _runtimeLastCoreCtrl = 0xffffffffu;
    const char *_runtimeStatus = "not-run";

    bool _bmiAttempted = false;
    bool _bmiSucceeded = false;
    bool _bmiGetTargetInfoAttempted = false;
    bool _bmiGetTargetInfoSucceeded = false;
    bool _bmiPipeValidationAttempted = false;
    bool _bmiPipeStructuralValidationSucceeded = false;
    bool _bmiPipeStrictValidationMismatch = false;
    bool _bmiTransferUncertain = false;
    bool _bmiTimedOut = false;
    bool _bmiQuiesceAttempted = false;
    bool _bmiQuiesceSucceeded = false;
    bool _dmaQuarantine = false;
    UInt32 _bmiRequestLength = 0;
    UInt32 _bmiResponseLength = 0;
    UInt32 _bmiTargetInfoLength = 0xffffffffu;
    UInt32 _bmiTargetVersion = 0xffffffffu;
    UInt32 _bmiTargetType = 0xffffffffu;
    UInt32 _bmiWaitUs = 0;
    UInt32 _bmiCE0SrcWrite = 0xffffffffu;
    UInt32 _bmiCE0SrcRead = 0xffffffffu;
    UInt32 _bmiCE1DstWrite = 0xffffffffu;
    UInt32 _bmiCE1DstRead = 0xffffffffu;
    UInt32 _bmiCompletedBytes = 0;
    // Retained across quiesce/reset; overwritten only by a new GET_TARGET_INFO attempt.
    UInt32 _bmiROMStatus=0, _bmiROMCore=0xffffffffu, _bmiROMFW=0xffffffffu;
    bool _bmiROMCoreRead=false, _bmiROMFWRead=false, _bmiROMStable=false;
    UInt32 _bmiROMDescriptorCPU=0, _bmiROMBufferCPU=0, _bmiROMWord=0xffffffffu;
    UInt32 _bmiROMDescriptorAddress=0, _bmiROMDescriptorBytes=0, _bmiROMDescriptorFlags=0;
    UInt32 _bmiROMSlot=0, _bmiROMReadCount=0;
    UInt32 _bmiROMVersion=0xffffffffu,_bmiROMType=0xffffffffu;
    UInt32 _bmiROMCE1DescriptorCPU=0,_bmiROMCE1Address=0,_bmiROMCE1Bytes=0,_bmiROMCE1Flags=0;
    bool _bmiROMResponseCandidate=false,_bmiROMCE1Stable=false;
    bool _bmiEvidenceEntered=false, _bmiEvidenceCondition=false;
    UInt32 _bmiEvidenceRequestLength=0, _bmiEvidenceCommand=0xffffffffu;
    bool _bmiEvidenceCaptured[2]={false,false};
    UInt32 _bmiEvidenceDescriptor[2][6]{};
    UInt32 _bmiEvidenceCE[2][2][18]{};
    UInt32 _bmiMiscStatus[2][2]{};bool _bmiMiscRead[2][2]{};
    UInt32 _bmiROMPreStatus=0,_bmiROMPreDescriptorCPU=0,_bmiROMPreBufferCPU=0,_bmiROMPreWords[3]{};
    bool _bmiROMPreStable=false;
    UInt32 _bmiWakeAtTimeout=0xffffffffu, _bmiRTCAtTimeout=0xffffffffu;

    bool _bmiReadMemoryAttempted = false;
    bool _bmiReadMemorySucceeded = false;
    UInt32 _bmiReadMemoryTransactionCount = 0;
    bool _bmiBoardUploadAttempted = false;
    bool _bmiBoardUploadSucceeded = false;
    bool _bmiBoardReadbackSucceeded = false;
    bool _bmiBoardInitVerified = false;
    UInt32 _bmiWriteMemoryTransactionCount = 0;
    UInt32 _bmiBoardBytesWritten = 0;
    UInt32 _bmiBoardBytesVerified = 0;
    const char *_bmiBoardUploadStatus = "not-run";
    bool _otpAttempted = false;
    bool _otpStreamStarted = false;
    bool _otpStreamClosed = false;
    bool _otpDownloadSucceeded = false;
    bool _otpExecuteAttempted = false;
    bool _otpExecuteSucceeded = false;
    bool _otpResultAccepted = false;
    bool _otpResultIgnored = false;
    UInt32 _otpResult = 0xffffffffu;
    UInt32 _otpWireBytesSent = 0;
    UInt32 _otpLZDataCommands = 0;
    UInt32 _otpExecuteResponseBytes = 0;
    const char *_otpStatus = "not-run";
    bool _fwAttempted = false;
    bool _fwStreamStarted = false;
    bool _fwStreamClosed = false;
    bool _fwDownloadSucceeded = false;
    bool _fwPostBMIProbeSucceeded = false;
    bool _fwReadbackAttempted = false;
    bool _fwReadbackSucceeded = false;
    bool _fwReadbackInProgress = false;
    UInt32 _fwWireBytesSent = 0;
    UInt32 _fwLZDataCommands = 0;
    UInt32 _fwReadbackBytes = 0;
    UInt32 _fwVerifiedSegments = 0;
    UInt32 _fwFailedSegment = 0xffffffffu;
    UInt32 _fwMismatchAddress = 0xffffffffu;
    UInt32 _fwPostTargetVersion = 0xffffffffu;
    UInt32 _fwPostTargetType = 0xffffffffu;
    const char *_fwStatus = "not-run";
    bool _bmiBoardPrereqAttempted = false;
    bool _bmiBoardPrereqSucceeded = false;
    bool _bmiInterconnectMatchesDiag = false;
    bool _bmiEarlyCfgDoneSeen = false;
    bool _bmiBoardDataPointerValid = false;
    bool _bmiBoardExtDataValid = false;
    UInt32 _bmiDesiredCPUSpeed = 0xffffffffu;
    UInt32 _bmiBoardDataAddress = 0xffffffffu;
    UInt32 _bmiBoardDataInitialized = 0xffffffffu;
    UInt32 _bmiBoardExtDataAddress = 0xffffffffu;
    UInt32 _bmiBoardExtDataConfig = 0xffffffffu;
    UInt32 _bmiBoardExtDataSize = 0;
    UInt32 _bmiCalDataAddress = 0xffffffffu;
    UInt32 _bmiInterconnectState = 0xffffffffu;
    UInt32 _bmiOptionFlag2 = 0xffffffffu;
    UInt32 _bmiBoardFingerprint[4] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};

    Ath10kDMABuffer _dmaTestBuffer;
    bool _dmaTestAttempted = false;
    bool _dmaTestSucceeded = false;
};
