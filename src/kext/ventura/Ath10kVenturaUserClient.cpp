#include "Ath10kVenturaABI.hpp"

#ifdef ATH10K_VENTURA

#include "Ath10kVenturaUserClient.hpp"
#include <string.h>
#include <stdint.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(Ath10kVenturaUserClient, IOUserClient)

namespace {

// Per-selector breadcrumbs live on the interface node as
// "Ath10kUCSel<selector>" counters, so one boot shows the call order.
void noteSelector(IOService *where, uint32_t selector,
                  IOExternalMethodArguments *args)
{
    if (where == nullptr)
        return;
    char key[48];
    snprintf(key, sizeof key, "Ath10kUCSel%u", selector);
    UInt64 seen = 0;
    if (OSNumber *n = OSDynamicCast(OSNumber, where->getProperty(key)))
        seen = n->unsigned64BitValue();
    where->setProperty(key, seen + 1, 32);

    UInt64 calls = 0;
    if (OSNumber *n = OSDynamicCast(OSNumber, where->getProperty("Ath10kUCalls")))
        calls = n->unsigned64BitValue();
    where->setProperty("Ath10kUCalls", calls + 1, 32);
    where->setProperty("Ath10kUCLastSelector", static_cast<UInt64>(selector), 32);
    if (args != nullptr) {
        where->setProperty("Ath10kUCLastScalarIn",
                           static_cast<UInt64>(args->scalarInputCount), 32);
        where->setProperty("Ath10kUCLastStructIn",
                           static_cast<UInt64>(args->structureInputSize), 32);
        where->setProperty("Ath10kUCLastStructOut",
                           static_cast<UInt64>(args->structureOutputSize), 32);
        if (args->scalarInputCount > 0 && args->scalarInput != nullptr)
            where->setProperty("Ath10kUCLastScalar0",
                               static_cast<UInt64>(args->scalarInput[0]), 64);
    }
}

}  // namespace

bool Ath10kVenturaUserClient::initWithTask(task_t owningTask, void *securityID,
                                           UInt32 type, OSDictionary *properties)
{
    if (type != 0)
        return false;
    if (!super::initWithTask(owningTask, securityID, type, properties))
        return false;
    _task = owningTask;
    if (_task != nullptr)
        task_reference(_task);
    return true;
}

bool Ath10kVenturaUserClient::start(IOService *provider)
{
    if (!super::start(provider))
        return false;
    _provider = provider;
    if (provider != nullptr)
        provider->setProperty("Ath10kUCStarted", true);
    return true;
}

void Ath10kVenturaUserClient::stop(IOService *provider)
{
    (void)provider;
    if (_task != nullptr) {
        task_deallocate(_task);
        _task = nullptr;
    }
    _provider = nullptr;
    super::stop(provider);
}

IOReturn Ath10kVenturaUserClient::clientClose()
{
    terminate();
    return kIOReturnSuccess;
}

IOReturn Ath10kVenturaUserClient::externalMethod(uint32_t selector,
                                                 IOExternalMethodArguments *args,
                                                 IOExternalMethodDispatch *dispatch,
                                                 OSObject *target, void *reference)
{
    noteSelector(_provider, selector, args);
    // Selector 0 arrives with an empty request and a 256-byte output structure.
    // Answer it with a zeroed structure so airportd moves on to the real
    // commands; the selector counters show what it asks for next.
    // Selector 0 is IO80211APIUserClient's extRunDriverCommand: the request
    // travels as a >= 0x3c30-byte structure input (apple80211req-shaped) and the
    // reply comes back in the structure output. Record the head of whatever
    // arrives so the field layout can be read off the real traffic instead of
    // guessed, and answer with a zeroed output buffer of the offered size.
    if (selector == 0 && args != nullptr) {
        if (args->structureInput != nullptr && args->structureInputSize >= 8) {
            const uint32_t *words = static_cast<const uint32_t *>(args->structureInput);
            _provider->setProperty("Ath10kUCIn0", static_cast<UInt64>(words[0]), 32);
            _provider->setProperty("Ath10kUCIn1", static_cast<UInt64>(words[1]), 32);
            if (args->structureInputSize >= 16)
                _provider->setProperty("Ath10kUCIn3", static_cast<UInt64>(words[3]), 32);
        }
        if (args->structureOutput != nullptr && args->structureOutputSize > 0 &&
            args->structureOutputDescriptor == nullptr)
            memset(args->structureOutput, 0, args->structureOutputSize);
        if (args->structureOutputSize > 0)
            return kIOReturnSuccess;
    }
    // Selector 1 asks a yes/no question with a one-byte answer (the family's
    // client uses the same shape for "can I use the IOUC event pipe"). Answering
    // false makes airportd stop right there - measured: no request ever reaches
    // the driver and the count stays at 1 - so answer true and find out which
    // selectors the real request path uses.
    if (selector == 1 && args != nullptr && args->structureOutput != nullptr &&
        args->structureOutputSize == 1) {
        if (args->structureOutputDescriptor == nullptr)
            *static_cast<uint8_t *>(args->structureOutput) = 1;
        return kIOReturnSuccess;
    }
    return super::externalMethod(selector, args, dispatch, target, reference);
}

#endif
