//
//  DragonflyUserClient.cpp
//

#include <os/log.h>

#include <DriverKit/DriverKit.h>
#include <DriverKit/OSData.h>

#include "DragonflyUserClient.h"
#include "DragonflyDriver.h"
#include "DragonflyShared.h"

struct DragonflyUserClient_IVars
{
    DragonflyDriver *driver;
};

bool DragonflyUserClient::init()
{
    if (!super::init()) return false;
    ivars = IONewZero(DragonflyUserClient_IVars, 1);
    return ivars != nullptr;
}

void DragonflyUserClient::free()
{
    if (ivars) OSSafeReleaseNULL(ivars->driver);
    IOSafeDeleteNULL(ivars, DragonflyUserClient_IVars, 1);
    super::free();
}

kern_return_t IMPL(DragonflyUserClient, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) return ret;
    ivars->driver = OSDynamicCast(DragonflyDriver, provider);
    if (!ivars->driver) return kIOReturnNoDevice;
    ivars->driver->retain();
    return kIOReturnSuccess;
}

kern_return_t IMPL(DragonflyUserClient, Stop)
{
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t DragonflyUserClient::ExternalMethod(uint64_t selector,
                                                  IOUserClientMethodArguments *args,
                                                  const IOUserClientMethodDispatch *dispatch,
                                                  OSObject *target,
                                                  void *reference)
{
    if (!ivars->driver) return kIOReturnNotReady;

    switch (selector) {
    case kDragonflySelectorWrite: {
        if (!args->structureInput) return kIOReturnBadArgument;
        size_t len = args->structureInput->getLength();
        if (len == 0 || len > kDragonflyReportSize) return kIOReturnBadArgument;
        return ivars->driver->WriteReport(
            static_cast<const uint8_t *>(args->structureInput->getBytesNoCopy()), (uint32_t)len);
    }
    case kDragonflySelectorRead: {
        if (args->scalarInputCount < 1) return kIOReturnBadArgument;
        uint32_t timeoutMs = (uint32_t)args->scalarInput[0];
        uint8_t report[kDragonflyReportSize] = {};
        uint32_t len = 0;
        kern_return_t ret = ivars->driver->ReadReport(report, &len, timeoutMs);
        if (ret != kIOReturnSuccess) return ret;
        args->structureOutput = OSData::withBytes(report, len);
        return args->structureOutput ? kIOReturnSuccess : kIOReturnNoMemory;
    }
    default:
        return kIOReturnUnsupported;
    }
}
