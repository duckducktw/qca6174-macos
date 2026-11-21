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
