//
//  DextConnection.h
//  Opens a connection to DragonflyDriver (the DriverKit extension) and provides the
//  read/write callbacks dfcore needs.
//

#ifndef DextConnection_h
#define DextConnection_h

#include <stdint.h>
#include <stdbool.h>
#include "dfcore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dext_connection dext_connection;

/* Returns NULL if the driver is not loaded (device unplugged, or driver not enabled). */
dext_connection *dext_open(void);
void dext_close(dext_connection *c);
bool dext_is_alive(dext_connection *c);

/* Initialise a df_device that talks through this connection. */
void dext_bind_device(dext_connection *c, df_device *dev);

#ifdef __cplusplus
}
#endif

#endif /* DextConnection_h */
