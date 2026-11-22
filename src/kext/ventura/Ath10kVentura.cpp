// Ventura (macOS 13) IO80211 v1 controller glue.
//
// Implemented here: interface publication, hardware address, enable/disable,
// transmit path, power/state reporting and the apple80211 request dispatcher
// that airportd uses for everything the Wi-Fi UI shows.
#include "Ath10kVenturaABI.hpp"
#include <net/kpi_interface.h>

#ifdef ATH10K_VENTURA

// Live-family only: the step that hands an ifnet to the 80211 core.
extern "C" IOReturn ath10kPrepareVifBsdInterface(IO80211VirtualInterface *, ifnet_t, UInt32)
    __asm("__ZN23IO80211VirtualInterface19prepareBSDInterfaceEP7__ifnetj");

#include "Ath10kVenturaInterface.hpp"
#include "../Ath10kPCIDevice.hpp"

#include <IOKit/IOCommandGate.h>
#include <net/if_media.h>

// ---------------------------------------------------------------------------
// Interface publication
// ---------------------------------------------------------------------------

IONetworkInterface *Ath10kPCIDevice::createInterface()
{
    Ath10kVenturaInterface *interface = new Ath10kVenturaInterface;
    if (interface == nullptr)
        return nullptr;
    if (!interface->init(this, this)) {
        interface->release();
        return nullptr;
    }
    // SystemConfiguration classifies the interface from IOInterfaceType when it
    // first discovers it, i.e. at the registerService() right after this
    // returns. Stamping 71 here was measured to break the unit entirely (the
    // family's own attach path refuses anything but IFT_ETHER), so the 802.11
    // value is applied later, in start(), and the discovery-time record stays
    // Ethernet until the unit is published through Apple's vif layer instead.
    _venturaInterface = interface;
    return interface;
}


// ---------------------------------------------------------------------------
// Controller-mandated virtuals
// ---------------------------------------------------------------------------

void Ath10kPCIDevice::_RESERVEDIO80211Controller0() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller1() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller2() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller3() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller4() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller5() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller6() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller7() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller8() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller9() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller10() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller11() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller12() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller13() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller14() {}
void Ath10kPCIDevice::_RESERVEDIO80211Controller15() {}

static void venturaPhase(IOService *p, const char *phase)
{
    if (p) p->setProperty("Ath10kVenturaLogPhase", phase);
}

void Ath10kPCIDevice::getLogPipes(CCPipe **driver, CCPipe **datapath, CCPipe **snapshot)
{
    // Hand the family our corecapture pipes: findAndAttachToFaultReporter()
    // asks for them, and leaves ivars->_faultReporter null (panic in logDebug)
    // when the answer is empty. It runs inside IO80211Controller::start(), so
    // build the chain here if start() has not done it yet.
    if (!_venturaPipesStarted[0] && !_venturaCoreFault) {
        if (!getWorkLoop() && !createWorkLoop()) {
            if (IOService *p = getProvider()) p->setProperty("Ath10kFaultSlotPhase", "no-workloop");
            return;
        }
        if (!prepareVenturaLogs()) {
            if (IOService *p = getProvider()) p->setProperty("Ath10kFaultSlotPhase", "logs-failed");
            return;
        }
    }
    if (driver) *driver = _venturaPipes[0];
    if (datapath) *datapath = _venturaPipes[1];
    if (snapshot) *snapshot = _venturaPipes[2];
    if (IOService *p = getProvider()) p->setProperty("Ath10kFaultSlotPhase", "pipes");
}

void *Ath10kPCIDevice::getFaultReporterFromDriver()
{
    if (_venturaCoreFault == nullptr) {
        if (!getWorkLoop() && !createWorkLoop()) {
            if (IOService *p = getProvider()) p->setProperty("Ath10kFaultSlotPhase", "no-workloop");
            return nullptr;
        }
        if (!prepareVenturaLogs()) {
            if (IOService *p = getProvider()) p->setProperty("Ath10kFaultSlotPhase", "logs-failed");
            return nullptr;
        }
    }
    if (IOService *p = getProvider()) p->setProperty("Ath10kFaultSlotPhase", "reporter");
    return _venturaCoreFault;
}

// ---------------------------------------------------------------------------
// Vtable alignment with the loaded IO80211Family generation
// ---------------------------------------------------------------------------
// The declarations in vendor/itlwm-v1/Airport/IO80211Controller.h now follow
// the LIVE com.apple.iokit.IO80211Family slot order (13.7.8), so getLogPipes,
// getFaultReporterFromDriver, apple80211VirtualRequest and
// apple80211SkywalkRequest are overrides on the slots the kernel actually
// dispatches. This routine only reports where the object's vptr points and
// whether the four slots carry our code, so a header regression shows up as a
// breadcrumb instead of as a panic inside framework code.

bool Ath10kPCIDevice::alignVenturaVtable()
{
    void **vtable = *reinterpret_cast<void ***>(this);
    IOService *probe = getProvider();
    if (vtable == nullptr)
        return false;
    if (probe) {
        probe->setProperty("Ath10kAlignPhase", "abi-native");
        probe->setProperty("Ath10kVptr", static_cast<UInt64>(reinterpret_cast<uintptr_t>(vtable)), 64);
        probe->setProperty("Ath10kVtLogPipes", static_cast<UInt64>(reinterpret_cast<uintptr_t>(vtable[0xd10 / sizeof(void *)])), 64);
        probe->setProperty("Ath10kVtFaultReporter", static_cast<UInt64>(reinterpret_cast<uintptr_t>(vtable[0xd48 / sizeof(void *)])), 64);
        probe->setProperty("Ath10kVtVirtualRequest", static_cast<UInt64>(reinterpret_cast<uintptr_t>(vtable[0xca8 / sizeof(void *)])), 64);
    }
    return true;
}

bool Ath10kPCIDevice::prepareVenturaLogs()
{
    IOService *owner = getProvider();
    if (!getWorkLoop()) { venturaPhase(owner, "no-workloop"); return false; }
    venturaPhase(owner, "begin");
    static const char *names[] = {"DriverLogs", "DatapathEvents", "StateSnapshots"};
    for (unsigned i = 0; i < 3; ++i) {
        CCPipeOptions options{};
        if (i == 2) {
            options.pipe_type = 0x200000001ULL;
            options.log_data_type = 2;
            options.pipe_size = 128;
        } else {
            options.pipe_type = 0;
            options.log_data_type = (i == 0) ? 1 : 0;
            options.pipe_size = 0x200000;
            options.min_log_size_notify = 0xccccc;
            options.notify_threshold = 1000;
            options.pad9 = (i == 0) ? 0x1000000 : 2;
            options.pad10 = (i == 0) ? 2 : 0x2800000;
        }
        strlcpy(options.file_name, names[i], sizeof(options.file_name));
        if (i != 1) strlcpy(options.name, (i == 0) ? "wlan0" : "0", sizeof(options.name));
        strlcpy(options.directory_name, "WiFi", sizeof(options.directory_name));
        _venturaPipes[i] = CCPipe::withOwnerNameCapacity(this, "br.com.jonasw8.driver.ath10kmac",
                                                         names[i], &options);
        if (!_venturaPipes[i]) { venturaPhase(owner, "pipe-create-failed"); return false; }
        if (!(_venturaPipesStarted[i] = _venturaPipes[i]->CCPipe::startPipe())) {
            venturaPhase(owner, "pipe-start-failed"); return false;
        }
        if (i == 0) {
            CCStreamOptions loggerOptions{};
            CCStream *logger = CCStream::withPipeAndName(_venturaPipes[0], "Ath10kMac", &loggerOptions);
            _venturaLogger = OSDynamicCast(CCLogStream, logger);
            if (!_venturaLogger) { OSSafeReleaseNULL(logger); venturaPhase(owner, "logstream-failed"); return false; }
        }
    }
    CCStreamOptions faultOptions{};
    faultOptions.stream_type = 1;
    faultOptions.console_level = ~0ULL;
    _venturaFaultStream = CCStream::withPipeAndName(_venturaPipes[2], "FaultReporter", &faultOptions);
    auto data = OSDynamicCast(CCDataStream, _venturaFaultStream);
    if (!data) { venturaPhase(owner, "fault-stream-failed"); return false; }
    // Ventura ships CCFaultReporter but not IO80211FaultReporter (that class
    // arrived with the newer IO80211 family). Importing it made OpenCore abort
    // the injection, so stop at the CCFaultReporter the 13.x kernel exports.
    _venturaCoreFault = CCFaultReporter::withStreamWorkloop(data, getWorkLoop());
    if (!_venturaCoreFault) { venturaPhase(owner, "core-fault-failed"); return false; }
    venturaPhase(owner, "ready");
    return true;
}

void Ath10kPCIDevice::releaseVenturaLogs()
{
    OSSafeReleaseNULL(_venturaCoreFault);
    OSSafeReleaseNULL(_venturaFaultStream);
    OSSafeReleaseNULL(_venturaLogger);
    for (unsigned i = 0; i < 3; ++i) {
        if (_venturaPipesStarted[i] && _venturaPipes[i]) _venturaPipes[i]->CCPipe::stopPipe();
        _venturaPipesStarted[i] = false;
        OSSafeReleaseNULL(_venturaPipes[i]);
    }
}

// Skywalk-era overloads: the 10.15+ class declares them virtual, and OpenCore
// refuses to inject a kext whose vtable leaves those slots unresolved.
IOReturn Ath10kPCIDevice::enable(IO80211SkywalkInterface *)
{
    return kIOReturnSuccess;
}

IOReturn Ath10kPCIDevice::disable(IO80211SkywalkInterface *)
{
    return kIOReturnSuccess;
}

UInt32 Ath10kPCIDevice::hardwareOutputQueueDepth()
{
    return 1024;
}

// IO80211Controller stores its IO80211CommandGate on the queue this returns and
// IO80211VirtualInterface::init() dereferences it through
// apple80211VirtualRequestIoctl(), so a null answer faults inside the family.
// IO80211WorkQueue::workQueue() is the shared instance Apple's own drivers use.
bool Ath10kPCIDevice::createWorkQueue()
{
    if (_venturaWorkQueue == nullptr)
        _venturaWorkQueue = IO80211WorkQueue::workQueue();
    return _venturaWorkQueue != nullptr;
}

IO80211WorkQueue *Ath10kPCIDevice::getWorkQueue() const
{
    return _venturaWorkQueue;
}

// The last piece the legacy-class publication cannot do: register the ifnet we
// already own with the 80211 core. On 13.7 prepareBSDInterface(ifnet, flags)
// lives only on the live family's IO80211SkywalkInterface/IO80211Virtual-
// Interface, never on the plain IO80211Interface, which is why the unit exists
// but SystemConfiguration still files it as Ethernet and airportd never sends a
// request. The live implementation only sets properties, MTU (1500),
// IOReporters and eflags on the ifnet we hand it, so passing ours is safe.
bool Ath10kPCIDevice::publishVenturaVifBsdUnit(IO80211VirtualInterface *vif, IOService *provider)
{
    if (vif == nullptr)
        return false;
    // The unit name the kernel picked is not on the object we hold as
    // "BSD Name" at this point, but the interface does carry the unit number and
    // the name prefix (SystemConfiguration reads the same pair), so build enN
    // from those and fall back to a registry scan.
    char unitName[16] = {0};
    if (_venturaInterface) {
        OSNumber *unit = OSDynamicCast(OSNumber, _venturaInterface->getProperty(kIOInterfaceUnit));
        OSString *prefix = OSDynamicCast(OSString,
                                         _venturaInterface->getProperty(kIOInterfaceNamePrefix));
        if (unit)
            snprintf(unitName, sizeof(unitName), "%s%u",
                     prefix ? prefix->getCStringNoCopy() : "en",
                     static_cast<unsigned>(unit->unsigned32BitValue()));
    }
    OSString *bsdName = nullptr;
    if (unitName[0] != 0)
        bsdName = OSString::withCString(unitName);
    if (bsdName == nullptr) {
        // Our own class, not the abstract parent: matching "IO80211Interface"
        // does not return the registered subclass instances here.
        OSDictionary *match = IOService::serviceMatching("Ath10kVenturaInterface");
        OSIterator *iter = match ? IOService::getMatchingServices(match) : nullptr;
        while (iter && bsdName == nullptr) {
            IOService *candidate = OSDynamicCast(IOService, iter->getNextObject());
            if (candidate == nullptr)
                break;
            OSString *name = OSDynamicCast(OSString, candidate->getProperty("BSD Name"));
            if (name == nullptr)
                continue;
            bsdName = name;
            bsdName->retain();
        }
        if (iter) iter->release();
        if (match) match->release();
    }
    if (bsdName == nullptr) {
        provider->setProperty("Ath10kVifBsdPhase", "no-bsd-name");
        return false;
    }
    ifnet_t ifp = nullptr;
    if (ifnet_find_by_name(bsdName->getCStringNoCopy(), &ifp) != 0 || ifp == nullptr) {
        provider->setProperty("Ath10kVifBsdPhase", "ifnet-not-found");
        bsdName->release();
        return false;
    }
    provider->setProperty("Ath10kVifBsdPhase", "ifnet-found");

    // Naming properties a published 80211 interface carries; set them before
    // prepareBSDInterface so the core publishes the unit under this name.
    IOService *target = reinterpret_cast<IOService *>(vif);
    target->setProperty("built-in", kOSBooleanTrue);
    target->setProperty("IOInterfaceName", bsdName->getCStringNoCopy());
    target->setProperty(kIOInterfaceUnit, static_cast<UInt64>(ifnet_unit(ifp)), 32);
    target->setProperty(kIOInterfaceNamePrefix, ifnet_name(ifp));

    const IOReturn status = ath10kPrepareVifBsdInterface(vif, ifp, 0);
    provider->setProperty("Ath10kVifBsdPhase",
                          status == kIOReturnSuccess ? "prepared" : "prepare-failed");
    provider->setProperty("Ath10kVifBsdStatus",
                          static_cast<UInt64>(static_cast<UInt32>(status)), 32);
    ifnet_release(ifp);
    bsdName->release();
    return status == kIOReturnSuccess;
}

SInt32 Ath10kPCIDevice::performCountryCodeOperation(IO80211CountryCodeOp operation)
{
    if (operation == kIO80211CountryCodeReset)
        return kIOReturnSuccess;
    return kIOReturnUnsupported;
}

SInt32 Ath10kPCIDevice::enableFeature(IO80211FeatureCode code, void *)
{
    if (code == kIO80211Feature80211n)
        return kIOReturnSuccess;
    return kIOReturnUnsupported;
}

// ---------------------------------------------------------------------------
// apple80211 request dispatch
// ---------------------------------------------------------------------------

void Ath10kPCIDevice::venturaPublishState(UInt32 state)
{
    _venturaState = state;
    if (_venturaInterface == nullptr)
        return;
    _venturaInterface->setLinkState(state == APPLE80211_S_RUN
                                        ? kIO80211NetworkLinkUp
                                        : kIO80211NetworkLinkDown,
                                    0);
}

// Every apple80211 request from airportd funnels through here so the boot
// breadcrumbs can answer "did the request arrive, and what did we answer".
// airportd surfaces only "Off (forced)" for both a missing request and a
// refused one.
SInt32 Ath10kPCIDevice::venturaRecordRequest(UInt requestType, int requestNumber, void *data)
{
    ++_venturaRequestCount;
    _venturaLastRequest = static_cast<UInt32>(requestNumber);
    _venturaLastRequestIsSet = requestType == 1;
    const SInt32 status = venturaDispatchRequest(requestType, requestNumber, nullptr, data);
    _venturaLastRequestStatus = status;
    if (IOService *provider = getProvider()) {
        provider->setProperty("Ath10kApple80211Requests",
                              static_cast<UInt64>(_venturaRequestCount), 32);
        provider->setProperty("Ath10kLastApple80211Request",
                              static_cast<UInt64>(_venturaLastRequest), 32);
        provider->setProperty("Ath10kLastApple80211Status",
                              static_cast<UInt64>(static_cast<UInt32>(status)), 32);
        provider->setProperty("Ath10kLastApple80211Verb",
                              _venturaLastRequestIsSet ? "set" : "get");
    }
    return status;
}

// The live family reaches a driver through these two, one per interface class.
SInt32 Ath10kPCIDevice::apple80211VirtualRequest(UInt requestType, int requestNumber,
                                                 IO80211VirtualInterface *, void *data)
{
    return venturaRecordRequest(requestType, requestNumber, data);
}

SInt32 Ath10kPCIDevice::apple80211SkywalkRequest(UInt requestType, int requestNumber,
                                                 IO80211SkywalkInterface *, void *data)
{
    return venturaRecordRequest(requestType, requestNumber, data);
}

SInt32 Ath10kPCIDevice::venturaDispatchRequest(unsigned int requestType, int requestNumber,
                                               IO80211Interface *, void *data)
{
    if (data == nullptr)
        return kIOReturnBadArgument;
    const bool isSet = requestType == 1;
    switch (requestNumber) {
    case APPLE80211_IOC_POWER: {
        apple80211_power_data *power = static_cast<apple80211_power_data *>(data);
        if (isSet) {
            venturaPower(power->power_state[0] != 0);
        } else {
            power->version = APPLE80211_VERSION;
            power->num_radios = APPLE80211_MAX_RADIO;
            for (int i = 0; i < APPLE80211_MAX_RADIO; i++)
                power->power_state[i] = _venturaPowerOn ? 1 : 0;
        }
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_STATE: {
        if (isSet)
            return kIOReturnUnsupported;
        apple80211_state_data *state = static_cast<apple80211_state_data *>(data);
        state->version = APPLE80211_VERSION;
        state->state = _venturaState;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_SSID: {
        if (isSet)
            return kIOReturnUnsupported;
        apple80211_ssid_data *ssid = static_cast<apple80211_ssid_data *>(data);
        ssid->version = APPLE80211_VERSION;
        ssid->ssid_len = 0;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_BSSID: {
        if (isSet)
            return kIOReturnUnsupported;
        apple80211_bssid_data *bssid = static_cast<apple80211_bssid_data *>(data);
        bssid->version = APPLE80211_VERSION;
        memset(&bssid->bssid, 0, sizeof(bssid->bssid));
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_CHANNEL:
        return venturaChannel(data, isSet);
    case APPLE80211_IOC_RATE: {
        if (isSet)
            return kIOReturnUnsupported;
        apple80211_rate_data *rate = static_cast<apple80211_rate_data *>(data);
        rate->version = APPLE80211_VERSION;
        rate->num_radios = APPLE80211_MAX_RADIO;
        for (int i = 0; i < APPLE80211_MAX_RADIO; i++)
            rate->rate[i] = 0;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_RSSI: {
        if (isSet)
            return kIOReturnUnsupported;
        apple80211_rssi_data *rssi = static_cast<apple80211_rssi_data *>(data);
        rssi->version = APPLE80211_VERSION;
        rssi->num_radios = APPLE80211_MAX_RADIO;
        rssi->rssi[0] = 0;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_CARD_CAPABILITIES:
        return venturaCapabilities(data);
    case APPLE80211_IOC_SUPPORTED_CHANNELS:
    case APPLE80211_IOC_HW_SUPPORTED_CHANNELS:
        return venturaSupportedChannels(data);
    case APPLE80211_IOC_DRIVER_VERSION:
    case APPLE80211_IOC_HARDWARE_VERSION:
        return venturaVersion(data);
    case APPLE80211_IOC_COUNTRY_CODE:
        return venturaCountryCode(data, isSet);
    case APPLE80211_IOC_SCAN_REQ:
    case APPLE80211_IOC_SCAN_REQ_MULTIPLE:
        return venturaScan(static_cast<apple80211_scan_data *>(data));
    case APPLE80211_IOC_SCAN_RESULT:
        return venturaScanResult(static_cast<apple80211_scan_result *>(data));
    case APPLE80211_IOC_SCANCACHE_CLEAR:
        _venturaScanIndex = 0;
        return kIOReturnSuccess;
    case APPLE80211_IOC_ASSOCIATE:
        return venturaAssociate(static_cast<apple80211_assoc_data *>(data));
    case APPLE80211_IOC_DISASSOCIATE:
    case APPLE80211_IOC_DEAUTH:
        venturaDisassociate(requestNumber == APPLE80211_IOC_DEAUTH);
        return kIOReturnSuccess;
    case APPLE80211_IOC_ASSOCIATE_RESULT: {
        apple80211_assoc_result_data *result =
            static_cast<apple80211_assoc_result_data *>(data);
        result->version = APPLE80211_VERSION;
        result->result = 0;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_ASSOCIATION_STATUS: {
        apple80211_assoc_status_data *status =
            static_cast<apple80211_assoc_status_data *>(data);
        status->version = APPLE80211_VERSION;
        status->status = _venturaState;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_AUTH_TYPE:
    case APPLE80211_IOC_PHY_MODE:
    case APPLE80211_IOC_OP_MODE:
    case APPLE80211_IOC_POWERSAVE:
    case APPLE80211_IOC_PROTMODE:
    case APPLE80211_IOC_TXPOWER:
    case APPLE80211_IOC_NOISE:
    case APPLE80211_IOC_LOCALE:
    case APPLE80211_IOC_TX_NSS:
    case APPLE80211_IOC_NSS:
    case APPLE80211_IOC_MCS:
    case APPLE80211_IOC_MCS_VHT:
    case APPLE80211_IOC_TX_ANTENNA:
    case APPLE80211_IOC_ANTENNA_DIVERSITY:
        return kIOReturnSuccess;
    default:
        return kIOReturnUnsupported;
    }
}

// ---------------------------------------------------------------------------
// Staged helpers (radio wiring lands in the following stage)
// ---------------------------------------------------------------------------

IOReturn Ath10kPCIDevice::venturaPower(bool on)
{
    _venturaPowerOn = on;
    venturaPublishState(APPLE80211_S_INIT);
    if (IOService *provider = getProvider())
        provider->setProperty("Ath10kPowerPhase", on ? "power-on-request" : "power-off-request");
    return kIOReturnSuccess;
}

IOReturn Ath10kPCIDevice::venturaScan(const apple80211_scan_data *)
{
    _venturaScanIndex = 0;
    return kIOReturnSuccess;
}

IOReturn Ath10kPCIDevice::venturaScanResult(apple80211_scan_result *result)
{
    if (result == nullptr)
        return kIOReturnBadArgument;
    memset(result, 0, sizeof(*result));
    result->version = APPLE80211_VERSION;
    return kIOReturnNotFound;
}

IOReturn Ath10kPCIDevice::venturaAssociate(const apple80211_assoc_data *)
{
    return kIOReturnUnsupported;
}

void Ath10kPCIDevice::venturaDisassociate(bool)
{
    venturaPublishState(APPLE80211_S_INIT);
}

SInt32 Ath10kPCIDevice::venturaChannel(void *data, bool isSet)
{
    apple80211_channel_data *channel = static_cast<apple80211_channel_data *>(data);
    if (isSet)
        return kIOReturnUnsupported;
    channel->version = APPLE80211_VERSION;
    channel->channel.version = APPLE80211_VERSION;
    channel->channel.channel = 0;
    channel->channel.flags = 0;
    return kIOReturnSuccess;
}

SInt32 Ath10kPCIDevice::venturaCapabilities(void *data)
{
    // apple80211 expects a packed phase-1 capability octet string, not a cipher
    // list. The bits below advertise 802.11a/b/g/n/ac plus WEP/TKIP/CCMP.
    apple80211_capability_data *capabilities =
        static_cast<apple80211_capability_data *>(data);
    memset(capabilities, 0, sizeof(*capabilities));
    capabilities->version = APPLE80211_VERSION;
    UInt8 *bits = capabilities->capabilities;
    bits[0] = 0x0f;   // 802.11a/b/g/n
    bits[1] = APPLE80211_CAPINFO_ESS >> 8;
    bits[4] = 0x01;   // WEP40
    bits[5] = 0x01;   // WEP104
    bits[6] = 0x01;   // TKIP
    bits[7] = 0x01;   // CCMP
    bits[8] = 0x00;   // no WPA3 yet
    bits[10] = 0x01;  // 802.11ac
    return kIOReturnSuccess;
}

SInt32 Ath10kPCIDevice::venturaSupportedChannels(void *data)
{
    apple80211_sup_channel_data *channels = static_cast<apple80211_sup_channel_data *>(data);
    channels->version = APPLE80211_VERSION;
    channels->num_channels = 0;
    return kIOReturnSuccess;
}

SInt32 Ath10kPCIDevice::venturaVersion(void *data)
{
    apple80211_version_data *info = static_cast<apple80211_version_data *>(data);
    info->version = APPLE80211_VERSION;
    strlcpy(info->string, "0.0.105-ventura", sizeof(info->string));
    info->string_len = static_cast<u_int16_t>(strlen(info->string));
    return kIOReturnSuccess;
}

SInt32 Ath10kPCIDevice::venturaCountryCode(void *data, bool isSet)
{
    apple80211_country_code_data *country =
        static_cast<apple80211_country_code_data *>(data);
    if (isSet)
        return kIOReturnSuccess;
    country->version = APPLE80211_VERSION;
    memset(country->cc, 0, sizeof(country->cc));
    return kIOReturnSuccess;
}

#endif
