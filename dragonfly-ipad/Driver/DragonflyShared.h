//
//  DragonflyShared.h
//  Constants shared between the driver extension and the app.
//

#ifndef DragonflyShared_h
#define DragonflyShared_h

// Registry name of the driver service (IOUserClass in the dext Info.plist).
#define kDragonflyServiceName      "DragonflyDriver"

#define kDragonflyReportSize       64
#define kDragonflyWriteTimeoutMs   2000

// User client selectors.
enum {
    // structureInput: up to 64 bytes (one output report, no report-ID byte)
    kDragonflySelectorWrite = 0,
    // scalarInput[0]: timeout in ms; structureOutput: the received report (up to 64 bytes)
    kDragonflySelectorRead  = 1,
};

#endif /* DragonflyShared_h */
