// Included inside Ath10kPCIDevice for the ATH10K_VENTURA (macOS 13) build.
// Only the classic IO80211 v1 surface lives here; radio control stays in the
// shared ath10k core.
public:
    IONetworkInterface *createInterface() override;
    // The live 13.7 IO80211Family has no apple80211Request virtual: airportd
    // reaches a driver through the Virtual/Skywalk request pair instead.
    SInt32 apple80211VirtualRequest(UInt, int, IO80211VirtualInterface *, void *) override;
    // The live family's per-type handlers reach a driver through one of these two
    // ioctl slots (setPower/setScanRequest both call [vptr+0xcb0]). They are shimmed
    // so the arguments can be measured instead of guessed: they publish the raw
    // registers and then run Apple's own implementation unchanged.
    SInt32 apple80211_ioctl(IO80211VirtualInterface *, ifnet_t, unsigned long, void *) override;
    // Five arguments: the family passes (cmd, type, iface, payload) in rsi/rdx/rcx/r8,
    // so a four-argument shim never sees the payload of a forwarded ASSOCIATE.
    SInt32 apple80211_ioctl(IO80211SkywalkInterface *, unsigned long, void *, void *,
                            void *) override;
    SInt32 apple80211SkywalkRequest(UInt, int, IO80211SkywalkInterface *, void *) override;
    UInt32 hardwareOutputQueueDepth() override;
    // setAssociateRequest() calls [vptr+0xc78] (= this predicate, live
    // 0xffffff8001fed798) right after the driver accepts ASSOCIATE and, when it
    // answers true, writes a 15-unit supplicant deadline into the vif and returns
    // 6 to the caller — i.e. it hands the four-way handshake to Apple's in-kernel
    // RSN supplicant. That class only exists in IO80211FamilyLegacy, which an
    // injected kext can never bind (HANDOFF §6.27), so the join would never finish:
    // airportd turns that error into -3903 and reports "could not join".
    // This personality runs its own supplicant, so answer false.
    bool useAppleRSNSupplicant(IO80211VirtualInterface *) override;
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
    // Who called, with which ioctl command, and with what request number: the
    // dispatcher only sees what survives each slot's calling convention, so a
    // request that dies above it (Apple's own handler answering 0x66) has to be
    // caught here - entry id 1 = interface ioctl, 2 = skywalk slot.
    void venturaTraceRequest(UInt32 entry, UInt32 cmd, UInt32 type, UInt32 len);
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
    // Replays a parked join once the previous link has finished tearing down, and
    // makes DISASSOCIATE actually drop the link instead of only publishing a state.
    // Credentials this driver already holds, used when a caller's ASSOCIATE arrives
    // without key material (airportd's Associate2 does exactly that).
    const char *venturaCredentialForSSID(const UInt8 *ssid, UInt32 length);
    bool venturaBSSProtected(const UInt8 *ssid, UInt32 length, const UInt8 **rsn,
                             UInt32 *rsnLength);
    // Association completion events for the client's join loop (see the
    // implementation: without them Apple80211Associate2 times out with -3905).
    void venturaPostLinkEvents(bool up);
    // Same state on the published interface (the node BSD knows as en1).
    void venturaInterfaceLink(bool up);
    IO80211LinkState _venturaLinkState = kIO80211NetworkLinkUndefined;
    // DISASSOCIATE(22) only publishes state (a real teardown made airportd's
    // routine disassociates knock the link over). The link therefore stays up,
    // so the getters have to *report* the interface as idle or a client that
    // checks "is the SSID empty now?" concludes the disconnect failed and aborts
    // its join - measured: airportd's last requests before -3903 were
    // s22, g1 (SSID), g103 (CURRENT_NETWORK), g1 (SSID).
    bool _venturaDisassociated = false;
    // While airportd is driving a join it disassociates first. Our own auto-join
    // must not race it: without this the link comes back up immediately and
    // airportd aborts the user's join with error 37 "Already associated, and
    // network transition not permitted" (measured in its WiFiManager log).
    UInt64 _venturaAutoJoinSuppressUntilMs = 0;
    void venturaPendingJoinService(UInt64 nowMs);
    void venturaDisassociate(bool deauth);
    IOReturn venturaScanResult(apple80211_scan_result *result);
    // Served by venturaScanResult(). The 13.7 SDK header advertises a pointer
    // for asr_ie_data, but macOS 12+ made it a 1024-byte inline array packed
    // straight after asr_ie_len; airportd's req_len is the only honest signal
    // for which shape to write, so performCommand records it here.
    UInt32 _venturaIoctlLength = 0;
    UInt32 _venturaScanRequests = 0;
    UInt32 _venturaScanRearms = 0;
    UInt32 _venturaScanServed = 0;
    UInt32 _venturaScanEnds = 0;
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
    thread_call_t _venturaAutoJoinCall = nullptr;
    UInt32 _venturaAutoJoinTries = 0;
    // Join list from boot-args (ath10kjoin="ssid:psk,ssid2:psk2"): lets the same
    // binary follow a different network without a rebuild.
    static constexpr UInt32 kVenJoinMax = 4;
    char _venturaJoinSSID[kVenJoinMax][33]{};
    char _venturaJoinPSK[kVenJoinMax][65]{};
    UInt32 _venturaJoinCount = 0;
    bool _venturaJoinParsed = false;
    void venturaParseJoinList();
    void venturaP256SelfTest();
    UInt32 _vifPublishAttempts = 0;
    static void vifPublishCall(thread_call_param_t, thread_call_param_t);
    static void venturaAutoJoinCall(thread_call_param_t, thread_call_param_t);
    void runVenturaAutoJoin();
    void venturaScheduleAutoJoin();
    void runVifPublish();
    // airportd collects scan results only after the scan-finished signal; the v1
    // interface has no postMessage on the live family, so this polls the shared
    // scan state from a delayed call and reports what the event route did.
    thread_call_t _scanNotifyCall = nullptr;
    UInt32 _scanNotifyAttempts = 0;
    UInt32 _scanNotifyGeneration = 0;
    static void scanNotifyCall(thread_call_param_t, thread_call_param_t);
    void runScanNotify();
    // Which message id the userspace client actually acts on is not documented and
    // not visible from the kernel side of a normal post, so after the first scan
    // every id in 1..APPLE80211_M_MAX is posted once, 200 ms apart, and the sweep
    // stops as soon as the client reacts by asking for SCAN_RESULT.
    thread_call_t _eventSweepCall = nullptr;
    UInt32 _eventSweepId = 0;
    static void eventSweepCall(thread_call_param_t, thread_call_param_t);
    void runEventSweep();
    // IO80211ScanManager (see setupVenturaScanManager): the live family keeps the
    // scan cache there, and the vif's getScanManager() is an unconditional null
    // stub. One manager per controller, plus a private copy of the vif's vtable
    // whose single slot 373 points at a thunk returning it.
    static constexpr size_t kVenturaVifGetScanManagerSlot = 373; // vptr + 0xb98
    static constexpr size_t kVenturaVtableCopySlots = 768;       // 660 live + headroom
    bool setupVenturaScanManager();
    OSObject *_venturaScanManager = nullptr;
    void **_venturaVifVtableCopy = nullptr;
    UInt32 _venturaScanMgrGeneration = 0xffffffffu;
    UInt32 _venturaScanMgrDones = 0;
    UInt32 _venturaScanMgrServes = 0;
    SInt32 venturaScanManagerRequest(int type, void *data);
    bool _venturaManagerCall = false;
    int _venturaFallType = -1;
    // Per-pid read positions in the last sweep (see venturaReaderSlot).
    static constexpr UInt32 kVenReaderSlots = 8;
    struct VenturaReader {
        int pid = 0;
        int8_t index = 0;
        UInt32 stamp = 0;
    } _venturaReaders[kVenReaderSlots]{};
    UInt32 _venturaReaderStamp = 0;
    int8_t *venturaReaderSlot();
    // [iframeIvars+0x208]: the event pipe Skywalk registration would install. Read
    // (never written) here so "events cannot be delivered" is a measurement.
    void *_venturaEventPipe = nullptr;
    bool _venturaScanNotifySent = false;
    // The radio is up from the moment the firmware boots, so report it as on from
    // the first POWER get: measured with this false, airportd's own state machine
    // treats Wi-Fi as "Off" and never asks for a scan, while the POWER *set* that
    // would fix it fails inside the family (Apple80211IOCTLSetWrapper returns
    // 102/EOPNOTSUPP from IO80211Family's setPower).
    bool _venturaPowerOn = true;
    UInt32 _venturaState = APPLE80211_S_INIT;
    UInt32 _venturaPinnedChannel = 0;
    UInt8 _venturaPinnedBSSID[6] = {0};
    bool _venturaPinnedBSSIDValid = false;
    UInt8 _venturaScanIndex = 0;
    // SSID that airportd handed us with Apple80211Set(SSID) before its join. The
    // join itself re-sends it inside apple80211_assoc_data; this copy exists so a
    // missing/failed ASSOCIATE can be told apart from "the set never arrived".
    UInt8 _venturaPendingSSID[APPLE80211_MAX_SSID_LEN + 1] = {};
    UInt8 _venturaPendingSSIDLength = 0;
    // A join that arrives while the previous link is still up (or while its teardown
    // is draining) cannot start a second vdev: the firmware answers the second
    // VDEV_START with "vdev-start-unsolicited" and the runtime stops (measured in
    // the click-to-join replay). Such a request is parked here and replayed once the
    // state machine is idle again.
    apple80211_assoc_data _venturaPendingAssoc = {};
    bool _venturaPendingAssocValid = false;
    UInt64 _venturaPendingSinceMs = 0;
    UInt32 _venturaAssocDefers = 0;
    // Copy-in target for request payloads that arrive from userspace (the family
    // forwards ASSOCIATE with a user pointer in r8).
    UInt8 _venturaAssocScratch[0x800] = {};
    // Request breadcrumbs: airportd reports "Off (forced)" when the power
    // request never reaches the driver, and there is no other channel to tell
    // "no request arrived" from "request refused".
    UInt32 _venturaRequestCount = 0;
    UInt32 _venturaLastRequest = 0xffffffffu;
    bool _venturaLastRequestIsSet = false;
    SInt32 _venturaLastRequestStatus = 0;
    // Rolling window of the last requests, so a client that stops asking after a
    // particular getter can be told apart from one that was never talking to us.
    static constexpr UInt32 kVenTraceSlots = 64;
    struct VenturaTraceEntry {
        UInt32 type = 0;
        UInt32 status = 0;
        UInt32 pid = 0;
        bool isSet = false;
    } _venturaTrace[kVenTraceSlots]{};
    // Same ring, but only the requests a *user-space* client made (pid != 0). The
    // 64-slot window above is consumed by airportd's getter polling within a second,
    // so "what did airportd ask right before it gave up" needs its own history.
    static constexpr UInt32 kVenClientTraceSlots = 16;
    struct VenturaClientTraceEntry {
        UInt32 type = 0;
        UInt32 status = 0;
        UInt32 pid = 0;
        bool isSet = false;
    } _venturaClientTrace[kVenClientTraceSlots]{};
    UInt32 _venturaClientTraceNext = 0;
    UInt32 _venturaClientTraceCount = 0;
    // Entry-point trace: entry id, ioctl cmd word, request number, length. The
    // dispatcher trace above cannot see a request that Apple's own handler kills
    // before it reaches us, which is exactly the case that refused the join.
    static constexpr UInt32 kVenEntryTraceSlots = 32;
    struct VenturaEntryTrace {
        UInt32 entry = 0;
        UInt32 cmd = 0;
        UInt32 type = 0;
        UInt32 len = 0;
    } _venturaEntryTrace[kVenEntryTraceSlots]{};
    UInt32 _venturaEntryTraceNext = 0;
    UInt32 _venturaEntryTraceCount = 0;
    // Last entry point / cmd seen, so the fall-through breadcrumb can name where
    // the request that Apple's handler refused actually came from.
    UInt32 _venturaLastEntry = 0;
    UInt32 _venturaLastEntryCmd = 0;
    // Raw dwords of every request block that reached the ioctl slot, so a block
    // whose req_type reads as nonsense can be compared with one that parses.
    static constexpr UInt32 kVenArgDumpSlots = 4;
    UInt32 _venturaArgDump[kVenArgDumpSlots][7]{};
    UInt32 _venturaArgDumpNext = 0;
    UInt32 _venturaArgDumpCount = 0;
    UInt32 _venturaTraceNext = 0;
    UInt32 _venturaTraceCount = 0;

    // In-kernel WPA2 supplicant. This personality has no user-space helper, so the
    // 4-way handshake runs here; key material still goes through the shared state
    // machine, which owns the firmware key install and the controlled port.
    // Opaque on purpose: the template type stays out of this header.
    struct Ath10kVenturaSupplicant *_venturaSecurity = nullptr;
    IOReturn venturaSecurityArm(const UInt8 *psk, UInt32 pskLength, UInt8 akm);
    bool venturaSecurityAccept(unsigned int result);
    const char *venturaSecurityService(UInt64 nowMs);
    void venturaSecurityStop();

public:
