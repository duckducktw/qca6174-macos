// Included inside Ath10kPCIDevice for the ATH10K_VENTURA (macOS 13) build.
// Only the classic IO80211 v1 surface lives here; radio control stays in the
// shared ath10k core.
public:
    IONetworkInterface *createInterface() override;
    // The live 13.7 IO80211Family has no apple80211Request virtual: airportd
    // reaches a driver through the Virtual/Skywalk request pair instead.
    SInt32 apple80211VirtualRequest(UInt, int, IO80211VirtualInterface *, void *) override;
    SInt32 apple80211SkywalkRequest(UInt, int, IO80211SkywalkInterface *, void *) override;
    UInt32 hardwareOutputQueueDepth() override;
    SInt32 performCountryCodeOperation(IO80211CountryCodeOp) override;
    // IO80211Controller keeps its command gate on an IO80211WorkQueue it gets
    // from getWorkQueue(). Apple's start() asks before anything exists, then
    // apple80211VirtualRequestIoctl() dereferences the answer while the vif is
    // being created — a null return is a page fault inside IO80211Family.
    bool createWorkQueue() override;
    IO80211WorkQueue *getWorkQueue() const override;
    SInt32 enableFeature(IO80211FeatureCode, void *) override;
    // Skywalk-era overloads exist in the class layout and must resolve
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
    // Kept as an override so the vtable slot stays resolvable, but it now just
    // defers to Apple's own implementation: returning our private pipes here
    // made findAndAttachToFaultReporter() take the "no pipe" branch and panic.
    void getLogPipes(CCPipe **, CCPipe **, CCPipe **) override;
    // The live family's fault-reporter handoff: returns the CCFaultReporter
    // built from our FaultReporter stream, or null to let the controller log.
    void *getFaultReporterFromDriver() override;
    SInt32 venturaRecordRequest(UInt requestType, int requestNumber, void *data);
    bool alignVenturaVtable();
    bool createVenturaMediumTables(const IONetworkMedium **primary, bool forcePublish = false);
    // Dumps the controller's published medium keys plus the current medium type,
    // so "our publish returned true but SIOCGIFMEDIA still says IFM_ETHER" can be
    // traced to the publish being replaced instead of guessed at.
    bool venturaMediumDictionaryKeys(char *out, size_t outLen);
    // Retained so setCurrentMedium/setSelectedMedium keep a live object for the
    // interface's whole life.
    const IONetworkMedium *_venturaMedium = nullptr;
    bool prepareVenturaLogs();
    void releaseVenturaLogs();
    CCPipe *_venturaPipes[3]{};
    bool _venturaPipesStarted[3]{};
    IO80211WorkQueue *_venturaWorkQueue = nullptr;
    CCLogStream *_venturaLogger=nullptr;
    CCStream *_venturaFaultStream=nullptr;
    CCFaultReporter *_venturaCoreFault=nullptr;
    IOReturn venturaPower(bool on);
    // Hand the ifnet our IO80211Interface already owns to the virtual
    // interface, so the 80211 core publishes the BSD unit itself instead of
    // SystemConfiguration filing an Apple-Ethernet-shaped interface as
    // "Ethernet N". Needs prepareBSDInterface(), which only the live
    // IO80211Family exports.
    bool publishVenturaVifBsdUnit(IO80211VirtualInterface *vif, IOService *provider);
    // The interface object createInterface() published, read back through a
    // file-static because the member does not survive every path.
    static IO80211Interface *venturaPublishedInterface();
    // The ifnet we own: allocated here with IFT_IEEE80211, attached by us, then
    // handed to the vif. Kept for the driver's life (ifnet_allocate's reference).
    ifnet_t createVenturaVifIfnet(IOService *provider);
    ifnet_t _venturaVifIfnet = nullptr;
    bool _venturaVifBsdDone = false;
    static constexpr u_int32_t kAth10kVifIfnetUnit = 3;
    IOReturn venturaScan(const apple80211_scan_data *request);
    IOReturn venturaAssociate(const apple80211_assoc_data *request);
    void venturaDisassociate(bool deauth);
    IOReturn venturaScanResult(apple80211_scan_result *result);
    SInt32 venturaChannel(void *data, bool isSet);
    SInt32 venturaDispatchRequest(unsigned int requestType, int requestNumber,
                                  IO80211Interface *iface, void *data);
    SInt32 venturaCapabilities(void *data);
    SInt32 venturaSupportedChannels(void *data);
    SInt32 venturaVersion(void *data);
    SInt32 venturaCountryCode(void *data, bool isSet);
    void venturaPublishState(UInt32 state);
    IO80211Interface *_venturaInterface = nullptr;
    // Skywalk-side object handed to the controller (see attachPlatformVif).
    // Airportd's per-interface step and IO80211APIUserClient::commonStart both
    // cast to IO80211SkywalkInterface, which the v1 interface can never satisfy.
    IOService *_venturaSkywalkIf = nullptr;
    IOService *attachPlatformVif();
    // Airportd's per-interface filter reads "IO80211InterfaceRole" off an entry
    // that has not been identified yet, so stamp the whole subtree once.
    static UInt32 stampInterfaceRole(IOService *root, int depth);
    // The vif is created inside Apple's start(), but its BSD handoff cannot run
    // there: the controller's registry children at that moment are only the
    // corecapture pipes and the vif, the interface node that carries "BSD Name"
    // shows up afterwards. Keep the vif and retry the handoff from a delayed
    // thread call.
    IO80211VirtualInterface *_venturaVif = nullptr;
    thread_call_t _vifPublishCall = nullptr;
    UInt32 _vifPublishAttempts = 0;
    static void vifPublishCall(thread_call_param_t, thread_call_param_t);
    void runVifPublish();
    bool _venturaPowerOn = false;
    UInt32 _venturaState = APPLE80211_S_INIT;
    UInt8 _venturaScanIndex = 0;
    // Request breadcrumbs: airportd reports "Off (forced)" when the power
    // request never reaches the driver, and there is no other channel to tell
    // "no request arrived" from "request refused".
    UInt32 _venturaRequestCount = 0;
    UInt32 _venturaLastRequest = 0xffffffffu;
    bool _venturaLastRequestIsSet = false;
    SInt32 _venturaLastRequestStatus = 0;

public:
