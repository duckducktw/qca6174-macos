#include "Ath10kVenturaABI.hpp"

#ifdef ATH10K_VENTURA

#include "Ath10kInfraInterface.hpp"
#include "../Ath10kPCIDevice.hpp"

// Apple's implementations, addressed by mangled name so the call is direct: a
// qualified C++ call to a virtual dispatches back into this override.
extern "C" bool ath10kSkywalkInit(IO80211InfraInterface *self, IOService *provider)
    __asm("__ZN23IO80211SkywalkInterface4initEP9IOService");
extern "C" void ath10kSetInterfaceRole(IO80211InfraInterface *self, UInt32 role)
    __asm("__ZN23IO80211SkywalkInterface16setInterfaceRoleEj");
extern "C" void ath10kSetInterfaceId(IO80211InfraInterface *self, UInt32 ident)
    __asm("__ZN23IO80211SkywalkInterface14setInterfaceIdEj");
extern "C" UInt32 ath10kGetInterfaceRole(IO80211InfraInterface *self)
    __asm("__ZN23IO80211SkywalkInterface16getInterfaceRoleEv");
extern "C" bool ath10kControllerAttachInterface(IO80211Controller *self, OSObject *iface,
                                                IOService *service)
    __asm("__ZN17IO80211Controller15attachInterfaceEP8OSObjectP9IOService");

#define super IO80211InfraInterface
OSDefineMetaClassAndStructors(Ath10kInfraInterface, IO80211InfraInterface)

void Ath10kInfraInterface::noteUserClientAttempt(UInt32 type)
{
    IOService *probe = (_owner != nullptr && _owner->getProvider() != nullptr)
                           ? _owner->getProvider() : this;
    UInt64 seen = 0;
    if (OSNumber *n = OSDynamicCast(OSNumber, probe->getProperty("Ath10kInfraUserClient")))
        seen = n->unsigned64BitValue();
    probe->setProperty("Ath10kInfraUserClient", seen + 1, 32);
    probe->setProperty("Ath10kInfraUserClientType", static_cast<UInt64>(type), 32);
    IOLog("Ath10kMac: infra newUserClient type=%u (#%llu)\n", (unsigned)type,
          static_cast<unsigned long long>(seen + 1));
}

IOReturn Ath10kInfraInterface::newUserClient(task_t owningTask, void *securityID,
                                             UInt32 type, OSDictionary *properties,
                                             IOUserClient **handler)
{
    (void)owningTask; (void)securityID; (void)properties;
    if (handler != nullptr)
        *handler = nullptr;
    noteUserClientAttempt(type);
    return kIOReturnUnsupported;
}

IOReturn Ath10kInfraInterface::newUserClient(task_t owningTask, void *securityID,
                                             UInt32 type, IOUserClient **handler)
{
    (void)owningTask; (void)securityID;
    if (handler != nullptr)
        *handler = nullptr;
    noteUserClientAttempt(type | 0x80000000u);
    return kIOReturnUnsupported;
}

Ath10kInfraInterface *Ath10kInfraInterface::createAndAttach(IO80211Controller *controller,
                                                           Ath10kPCIDevice *owner)
{
    if (controller == nullptr)
        return nullptr;
    IOService *probe = owner != nullptr ? owner->getProvider() : nullptr;
    if (probe == nullptr)
        probe = controller;

    Ath10kInfraInterface *iface = new Ath10kInfraInterface;
    if (iface == nullptr) {
        probe->setProperty("Ath10kInfraPhase", "alloc-failed");
        return nullptr;
    }
    // IO80211SkywalkInterface::init(IOService*) allocates the ivar block and
    // stashes safeMetaCast(provider, IO80211Controller) in it, which is what the
    // family reads back later. Pass the controller, exactly like AirportItlwm.
    if (!ath10kSkywalkInit(iface, controller)) {
        probe->setProperty("Ath10kInfraPhase", "init-failed");
        iface->release();
        return nullptr;
    }
    iface->_owner = owner;
    ath10kSetInterfaceRole(iface, 1);  // infra role: required by attachInterface
    ath10kSetInterfaceId(iface, 1);
    probe->setProperty("Ath10kInfraRole", static_cast<UInt64>(ath10kGetInterfaceRole(iface)), 32);

    if (!iface->attach(controller)) {
        probe->setProperty("Ath10kInfraPhase", "service-attach-failed");
        iface->release();
        return nullptr;
    }
    const bool attached = ath10kControllerAttachInterface(controller, iface, controller);
    probe->setProperty("Ath10kInfraAttach", attached ? "ok" : "failed");
    probe->setProperty("Ath10kInfraPhase", attached ? "attached" : "controller-attach-failed");
    IOLog("Ath10kMac: infra iface=%p role=%u attach=%d\n", iface,
          (unsigned)ath10kGetInterfaceRole(iface), (int)attached);
    return iface;
}

#endif
