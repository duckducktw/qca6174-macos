// Included inside Ath10kPCIDevice for the ATH10K_VENTURA (macOS 13) build.
// Only the classic IO80211 v1 surface lives here; radio control stays in the
// shared ath10k core.
public:
    IONetworkInterface *createInterface() override;
    IOReturn getHardwareAddressForInterface(IO80211Interface *, IOEthernetAddress *) override;
    SInt32 apple80211Request(unsigned int, int, IO80211Interface *, void *) override;
    SInt32 stopDMA() override;
    UInt32 hardwareOutputQueueDepth(IO80211Interface *) override;
    SInt32 performCountryCodeOperation(IO80211Interface *, IO80211CountryCodeOp) override;
    SInt32 enableFeature(IO80211FeatureCode, void *) override;
    SInt32 monitorModeSetEnabled(IO80211Interface *, bool, UInt32) override;
    void dataLinkLayerAttachComplete(IO80211Interface *) override;
    bool useAppleRSNSupplicant(IO80211Interface *) override;

private:
    friend class Ath10kVenturaInterface;
    IOReturn venturaPower(bool on);
    IOReturn venturaScan(const apple80211_scan_data *request);
    IOReturn venturaAssociate(const apple80211_assoc_data *request);
    void venturaDisassociate(bool deauth);
    IOReturn venturaScanResult(apple80211_scan_result *result);
    SInt32 venturaChannel(void *data, bool isSet);
    SInt32 venturaCapabilities(void *data);
    SInt32 venturaSupportedChannels(void *data);
    SInt32 venturaVersion(void *data);
    SInt32 venturaCountryCode(void *data, bool isSet);
    void venturaPublishState(UInt32 state);
    IO80211Interface *_venturaInterface = nullptr;
    bool _venturaPowerOn = false;
    UInt32 _venturaState = APPLE80211_S_INIT;
    UInt8 _venturaScanIndex = 0;

public:
