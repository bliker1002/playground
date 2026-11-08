//
//  DextConnection.c
//

#include "DextConnection.h"

#include <stdlib.h>
#include <string.h>
#include <IOKit/IOKitLib.h>

#include "DragonflyShared.h"

/* USB transaction timeout (iokit_usb_err(0x51)); not exported by the iPadOS SDK headers. */
#define DF_USB_TRANSACTION_TIMEOUT ((kern_return_t)0xe0004051)

struct dext_connection {
    io_connect_t connect;
    bool         dead;
};

dext_connection *dext_open(void)
{
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
                                                       IOServiceNameMatching(kDragonflyServiceName));
    if (service == IO_OBJECT_NULL) return NULL;

    io_connect_t connect = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &connect);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) return NULL;

    dext_connection *c = calloc(1, sizeof *c);
    if (!c) { IOServiceClose(connect); return NULL; }
    c->connect = connect;
    return c;
}

void dext_close(dext_connection *c)
{
    if (!c) return;
    IOServiceClose(c->connect);
    free(c);
}

bool dext_is_alive(dext_connection *c) { return c && !c->dead; }

static int dext_write(void *ctx, const uint8_t packet[DF_PACKET_SIZE])
{
    dext_connection *c = ctx;
    kern_return_t kr = IOConnectCallStructMethod(c->connect, kDragonflySelectorWrite,
                                                 packet, DF_PACKET_SIZE, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        if (kr == MACH_SEND_INVALID_DEST || kr == kIOReturnNotAttached || kr == kIOReturnNoDevice)
            c->dead = true;
        return DF_ERR_IO;
    }
    return 0;
}

static int dext_read(void *ctx, uint8_t packet[DF_PACKET_SIZE], uint32_t timeout_ms)
{
    dext_connection *c = ctx;
    uint64_t in = timeout_ms;
    uint8_t buf[DF_PACKET_SIZE] = {0};
    size_t outSize = sizeof buf;
    kern_return_t kr = IOConnectCallMethod(c->connect, kDragonflySelectorRead,
                                           &in, 1, NULL, 0, NULL, NULL, buf, &outSize);
    if (kr == kIOReturnTimeout || kr == DF_USB_TRANSACTION_TIMEOUT) return 0;
    if (kr != KERN_SUCCESS) {
        if (kr == MACH_SEND_INVALID_DEST || kr == kIOReturnNotAttached || kr == kIOReturnNoDevice)
            c->dead = true;
        return DF_ERR_IO;
    }
    memset(packet, 0, DF_PACKET_SIZE);
    memcpy(packet, buf, outSize > DF_PACKET_SIZE ? DF_PACKET_SIZE : outSize);
    return outSize > 0 ? (int)outSize : 0;
}

void dext_bind_device(dext_connection *c, df_device *dev)
{
    df_device_init(dev, c, dext_write, dext_read);
}
