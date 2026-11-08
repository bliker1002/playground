//
//  DragonflyDriver.cpp
//

#include <os/log.h>

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOUserClient.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <USBDriverKit/USBDriverKit.h>

#include "DragonflyDriver.h"
#include "DragonflyShared.h"

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "DragonflyDriver: " fmt, ##__VA_ARGS__)

struct DragonflyDriver_IVars
{
    IOUSBHostDevice           *device;
    IOUSBHostInterface        *interface;
    IOUSBHostPipe             *inPipe;
    IOUSBHostPipe             *outPipe;      // may be NULL -> use HID SET_REPORT on EP0
    IOBufferMemoryDescriptor  *inBuffer;
    IOBufferMemoryDescriptor  *outBuffer;
    uint8_t                    interfaceNumber;
    uint16_t                   inMaxPacket;
};

bool DragonflyDriver::init()
{
    if (!super::init()) return false;
    ivars = IONewZero(DragonflyDriver_IVars, 1);
    return ivars != nullptr;
}

void DragonflyDriver::free()
{
    if (ivars) {
        OSSafeReleaseNULL(ivars->inBuffer);
        OSSafeReleaseNULL(ivars->outBuffer);
        OSSafeReleaseNULL(ivars->inPipe);
        OSSafeReleaseNULL(ivars->outPipe);
        OSSafeReleaseNULL(ivars->interface);
        OSSafeReleaseNULL(ivars->device);
    }
    IOSafeDeleteNULL(ivars, DragonflyDriver_IVars, 1);
    super::free();
}

kern_return_t IMPL(DragonflyDriver, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) return ret;

    ivars->device = OSDynamicCast(IOUSBHostDevice, provider);
    if (!ivars->device) { LOG("provider is not a USB device"); return kIOReturnNoDevice; }
    ivars->device->retain();

    ret = ivars->device->Open(this, 0, 0);
    if (ret != kIOReturnSuccess) { LOG("device Open failed 0x%x", ret); return ret; }

    // Select the first configuration without letting the system match interface drivers (so the
    // generic HID driver does not grab the interface).
    const IOUSBConfigurationDescriptor *devCfg = ivars->device->CopyConfigurationDescriptor(0);
    if (!devCfg) { LOG("no configuration descriptor"); return kIOReturnNotFound; }
    uint8_t cfgValue = devCfg->bConfigurationValue;
    IOUSBHostFreeDescriptor(devCfg);
    ret = ivars->device->SetConfiguration(cfgValue, false);
    if (ret != kIOReturnSuccess) { LOG("SetConfiguration failed 0x%x", ret); return ret; }

    uintptr_t iter = 0;
    ret = ivars->device->CreateInterfaceIterator(&iter);
    if (ret != kIOReturnSuccess) { LOG("CreateInterfaceIterator failed 0x%x", ret); return ret; }
    ret = ivars->device->CopyInterface(iter, &ivars->interface);
    ivars->device->DestroyInterfaceIterator(iter);
    if (ret != kIOReturnSuccess || !ivars->interface) { LOG("no interface 0x%x", ret); return kIOReturnNotFound; }

    ret = ivars->interface->Open(this, 0, 0);
    if (ret != kIOReturnSuccess) { LOG("interface Open failed 0x%x", ret); return ret; }

    // Find the interrupt endpoints.
    const IOUSBConfigurationDescriptor *cfg = ivars->interface->CopyConfigurationDescriptor();
    const IOUSBInterfaceDescriptor *ifd = cfg ? ivars->interface->GetInterfaceDescriptor(cfg) : nullptr;
    if (!ifd) {
        if (cfg) IOUSBHostFreeDescriptor(cfg);
        LOG("no interface descriptor");
        return kIOReturnNotFound;
    }
    ivars->interfaceNumber = ifd->bInterfaceNumber;
    const IOUSBEndpointDescriptor *ep = nullptr;
    while ((ep = IOUSBGetNextEndpointDescriptor(cfg, ifd, (const IOUSBDescriptorHeader *)ep)) != nullptr) {
        if (IOUSBGetEndpointType(ep) != kIOUSBEndpointTypeInterrupt) continue;
        uint8_t addr = ep->bEndpointAddress;
        if ((addr & 0x80) && !ivars->inPipe) {
            ivars->interface->CopyPipe(addr, &ivars->inPipe);
            ivars->inMaxPacket = (uint16_t)(ep->wMaxPacketSize & 0x7FF);
        } else if (!(addr & 0x80) && !ivars->outPipe) {
            ivars->interface->CopyPipe(addr, &ivars->outPipe);
        }
    }
    IOUSBHostFreeDescriptor(cfg);
    if (!ivars->inPipe) { LOG("no interrupt IN endpoint"); return kIOReturnNotFound; }
    LOG("interface %u, IN max packet %u, OUT pipe %s", ivars->interfaceNumber, ivars->inMaxPacket,
        ivars->outPipe ? "yes" : "no (using SET_REPORT)");

    ret = ivars->interface->CreateIOBuffer(kIOMemoryDirectionIn, kDragonflyReportSize, &ivars->inBuffer);
    if (ret != kIOReturnSuccess) return ret;
    ret = ivars->interface->CreateIOBuffer(kIOMemoryDirectionOut, kDragonflyReportSize, &ivars->outBuffer);
    if (ret != kIOReturnSuccess) return ret;

    // Make the driver visible to the app (IOServiceGetMatchingService by name).
    ret = RegisterService();
    LOG("started (0x%x)", ret);
    return ret;
}

kern_return_t IMPL(DragonflyDriver, Stop)
{
    LOG("stopping");
    if (ivars->inPipe) ivars->inPipe->Abort(0, kIOReturnAborted, nullptr);
    if (ivars->outPipe) ivars->outPipe->Abort(0, kIOReturnAborted, nullptr);
    if (ivars->interface) ivars->interface->Close(this, 0);
    if (ivars->device) ivars->device->Close(this, 0);
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t IMPL(DragonflyDriver, NewUserClient)
{
    IOService *client = nullptr;
    kern_return_t ret = Create(this, "UserClientProperties", &client);
    if (ret != kIOReturnSuccess) { LOG("Create user client failed 0x%x", ret); return ret; }
    *userClient = OSDynamicCast(IOUserClient, client);
    if (!*userClient) {
        client->release();
        return kIOReturnError;
    }
    return kIOReturnSuccess;
}

kern_return_t DragonflyDriver::WriteReport(const uint8_t *data, uint32_t length)
{
    if (!ivars->interface || !ivars->outBuffer) return kIOReturnNotReady;
    if (length > kDragonflyReportSize) return kIOReturnBadArgument;

    IOAddressSegment seg = {};
    ivars->outBuffer->GetAddressRange(&seg);
    memset(reinterpret_cast<void *>(seg.address), 0, kDragonflyReportSize);
    memcpy(reinterpret_cast<void *>(seg.address), data, length);
    ivars->outBuffer->SetLength(kDragonflyReportSize);

    if (ivars->outPipe) {
        uint32_t done = 0;
        return ivars->outPipe->IO(ivars->outBuffer, kDragonflyReportSize, &done, kDragonflyWriteTimeoutMs);
    }

    // No interrupt OUT endpoint: HID class SET_REPORT(Output, ID 0) on the control pipe.
    uint16_t done = 0;
    return ivars->interface->DeviceRequest(0x21,                 // host->device, class, interface
                                           0x09,                 // SET_REPORT
                                           0x0200,               // report type Output, ID 0
                                           ivars->interfaceNumber,
                                           kDragonflyReportSize,
                                           ivars->outBuffer,
                                           &done,
                                           kDragonflyWriteTimeoutMs);
}

kern_return_t DragonflyDriver::ReadReport(uint8_t *data, uint32_t *length, uint32_t timeoutMs)
{
    if (!ivars->inPipe || !ivars->inBuffer) return kIOReturnNotReady;
    uint32_t want = ivars->inMaxPacket ? ivars->inMaxPacket : kDragonflyReportSize;
    if (want > kDragonflyReportSize) want = kDragonflyReportSize;
    ivars->inBuffer->SetLength(want);

    uint32_t done = 0;
    kern_return_t ret = ivars->inPipe->IO(ivars->inBuffer, want, &done, timeoutMs);
    if (ret != kIOReturnSuccess) {
        *length = 0;
        return ret;
    }
    IOAddressSegment seg = {};
    ivars->inBuffer->GetAddressRange(&seg);
    if (done > kDragonflyReportSize) done = kDragonflyReportSize;
    memcpy(data, reinterpret_cast<const void *>(seg.address), done);
    *length = done;
    return kIOReturnSuccess;
}
