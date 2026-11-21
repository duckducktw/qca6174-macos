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
    // Skywalk-era overloads exist in the 10.15+ class layout and must resolve
    // to real symbols, or OpenCore's vtable reconstruction aborts the injection.
    IOReturn enable(IO80211SkywalkInterface *) override;
    IOReturn disable(IO80211SkywalkInterface *) override;
    // OSMetaClassDeclareReservedUnused slots: leave no zero vtable entry, or
    // OpenCore aborts the injection when it cannot map them onto the kernel.
    void _RESERVEDIO80211Controller0() override;
    void _RESERVEDIO80211Controller1() override;
    void _RESERVEDIO80211Controller2() override;
    void _RESERVEDIO80211Controller3() override;
    void _RESERVEDIO80211Controller4() override;
    void _RESERVEDIO80211Controller5() override;
    void _RESERVEDIO80211Controller6() override;
    void _RESERVEDIO80211Controller7() override;
    void _RESERVEDIO80211Controller8() override;
    void _RESERVEDIO80211Controller9() override;
    void _RESERVEDIO80211Controller10() override;
    void _RESERVEDIO80211Controller11() override;
    void _RESERVEDIO80211Controller12() override;
    void _RESERVEDIO80211Controller13() override;
    void _RESERVEDIO80211Controller14() override;
    void _RESERVEDIO80211Controller15() override;

private:
    friend class Ath10kVenturaInterface;
    // corecapture/IO80211 logging must exist before IO80211Controller::start()
    // runs: its findAndAttachToFaultReporter() step panics when the driver has
    // not published a FaultReporter stream.
    // IO80211Controller::findAndAttachToFaultReporter() asks the driver for
    // its corecapture pipes through this virtual; the default implementation
    // returns nothing, which leaves ivars->_faultReporter null and makes the
    // controller panic inside logDebug().
    void getLogPipes(CCPipe **, CCPipe **, CCPipe **) override;
    // The IO80211Family build that actually loads on 13.7 is the Skywalk-era
    // generation: it dispatches getLogPipes at +0xd20 and
    // getFaultReporterFromDriver at +0xd58, while the v1 headers we compile
    // against place them at +0xd48 and not at all. Without this the kernel
    // calls an empty slot, finds no fault reporter and panics in logDebug().
    static bool venturaLogPipesSlot(IO80211Controller *, CCPipe **, CCPipe **, CCPipe **);
    static void *venturaFaultReporterSlot(IO80211Controller *);
    void alignVenturaVtable();
    bool prepareVenturaLogs();
    void releaseVenturaLogs();
    CCPipe *_venturaPipes[3]{};
    bool _venturaPipesStarted[3]{};
    CCLogStream *_venturaLogger=nullptr;
    CCStream *_venturaFaultStream=nullptr;
    CCFaultReporter *_venturaCoreFault=nullptr;
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
