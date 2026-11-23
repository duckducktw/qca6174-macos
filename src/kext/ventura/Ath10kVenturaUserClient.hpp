// User client for the Ventura interface node.
//
// Apple80211 opens the interface's IOUserClientClass with type 0 and then drives
// the driver through external methods (the apple80211 kernel request protocol).
// The family's own IO80211APIUserClient rejects this provider because it casts it
// to IO80211SkywalkInterface first, which a v1 IO80211Interface can never be. So
// the interface advertises this class instead: it records every selector that
// arrives (so the protocol can be implemented in the order it is actually used)
// and answers with kIOReturnUnsupported for now.
#pragma once

#ifdef ATH10K_VENTURA

#include <IOKit/IOUserClient.h>

class Ath10kVenturaUserClient : public IOUserClient {
    OSDeclareDefaultStructors(Ath10kVenturaUserClient)

public:
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
                      OSDictionary *properties) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch, OSObject *target,
                            void *reference) override;

private:
    task_t _task = nullptr;
    IOService *_provider = nullptr;
};

#endif
