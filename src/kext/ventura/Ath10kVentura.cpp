// Ventura (macOS 13) IO80211 v1 controller glue.
//
// Implemented here: interface publication, hardware address, enable/disable,
// transmit path, power/state reporting and the apple80211 request dispatcher
// that airportd uses for everything the Wi-Fi UI shows.
#include "Ath10kVenturaABI.hpp"

#ifdef ATH10K_VENTURA

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
    _venturaInterface = interface;
    return interface;
}


// ---------------------------------------------------------------------------
// Controller-mandated virtuals
// ---------------------------------------------------------------------------

SInt32 Ath10kPCIDevice::stopDMA()
{
    return kIOReturnSuccess;
}

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
    // Defer to the family: it hands back the controller's own corecapture pipes,
    // which is what its fault-reporter lookup expects.
    IO80211Controller::getLogPipes(driver, datapath, snapshot);
}

// ---------------------------------------------------------------------------
// Vtable alignment with the loaded IO80211Family generation
// ---------------------------------------------------------------------------
// 13.7 ships the Skywalk-era IO80211Controller layout: getLogPipes sits at
// object offset 0xd20 and getFaultReporterFromDriver at 0xd58. The vendored v1
// declarations put getLogPipes at 0xd48 and have no fault-reporter virtual at
// all, so the family's own dispatch never reached our overrides and
// findAndAttachToFaultReporter() panicked later inside logDebug(). Point those
// two slots at our implementations before any Apple code runs.

bool Ath10kPCIDevice::venturaLogPipesSlot(IO80211Controller *self, CCPipe **driver,
                                          CCPipe **datapath, CCPipe **snapshot)
{
    Ath10kPCIDevice *device = static_cast<Ath10kPCIDevice *>(self);
    device->getLogPipes(driver, datapath, snapshot);
    return (driver && *driver) || (datapath && *datapath) || (snapshot && *snapshot);
}

void *Ath10kPCIDevice::venturaFaultReporterSlot(IO80211Controller *self)
{
    Ath10kPCIDevice *device = static_cast<Ath10kPCIDevice *>(self);
    IOService *probe = device->getProvider();
    // IO80211Controller asks for this before start() has prepared corecapture,
    // and logDebug() panics on a null answer, so create the chain on demand.
    if (device->_venturaCoreFault == nullptr) {
        if (!device->getWorkLoop() && !device->createWorkLoop()) {
            if (probe) probe->setProperty("Ath10kFaultSlotPhase", "no-workloop");
            return nullptr;
        }
        if (!device->prepareVenturaLogs()) {
            if (probe) probe->setProperty("Ath10kFaultSlotPhase", "logs-failed");
            return nullptr;
        }
    }
    if (probe) probe->setProperty("Ath10kFaultSlotPhase", "reporter");
    return device->_venturaCoreFault;
}

bool Ath10kPCIDevice::alignVenturaVtable()
{
    void **vtable = *reinterpret_cast<void ***>(this);
    IOService *probe = getProvider();
    if (vtable == nullptr)
        return false;
    void *logSlot = reinterpret_cast<void *>(&Ath10kPCIDevice::venturaLogPipesSlot);
    void *faultSlot = reinterpret_cast<void *>(&Ath10kPCIDevice::venturaFaultReporterSlot);
    void *virtualRequestSlot = reinterpret_cast<void *>(&Ath10kPCIDevice::venturaVirtualRequestSlot);
    void *skywalkRequestSlot = reinterpret_cast<void *>(&Ath10kPCIDevice::venturaSkywalkRequestSlot);
    vtable[0xd20 / sizeof(void *)] = logSlot;
    vtable[0xd58 / sizeof(void *)] = faultSlot;
    // apple80211VirtualRequest / apple80211SkywalkRequest: the vif asks for its
    // capabilities through these, and our overrides use the older
    // apple80211Request signature, so route them to the same dispatcher.
    vtable[0xcb8 / sizeof(void *)] = virtualRequestSlot;
    vtable[0xcc0 / sizeof(void *)] = skywalkRequestSlot;
    const bool ok = vtable[0xd20 / sizeof(void *)] == logSlot &&
                    vtable[0xd58 / sizeof(void *)] == faultSlot;
    if (probe) {
        probe->setProperty("Ath10kAlignPhase", ok ? "aligned" : "write-failed");
        probe->setProperty("Ath10kVptr", static_cast<UInt64>(reinterpret_cast<uintptr_t>(vtable)), 64);
    }
    return ok;
}

SInt32 Ath10kPCIDevice::venturaVirtualRequestSlot(IO80211Controller *self, UInt type,
                                                  int number, IO80211VirtualInterface *,
                                                  void *data)
{
    return static_cast<Ath10kPCIDevice *>(self)->apple80211Request(type, number, nullptr, data);
}

SInt32 Ath10kPCIDevice::venturaSkywalkRequestSlot(IO80211Controller *self, UInt type,
                                                  int number, IO80211SkywalkInterface *,
                                                  void *data)
{
    return static_cast<Ath10kPCIDevice *>(self)->apple80211Request(type, number, nullptr, data);
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

UInt32 Ath10kPCIDevice::hardwareOutputQueueDepth(IO80211Interface *)
{
    return 1024;
}

SInt32 Ath10kPCIDevice::performCountryCodeOperation(IO80211Interface *,
                                                    IO80211CountryCodeOp operation)
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

SInt32 Ath10kPCIDevice::apple80211Request(unsigned int requestType, int requestNumber,
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
