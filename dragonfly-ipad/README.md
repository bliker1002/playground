# DragonflyScope: an iPad app for OtO Photonics Dragonfly spectrometers

DragonflyScope shows live spectra from an OtO Photonics **Dragonfly** NIR spectrometer
(DF1514 / DF1914 / DF1924 / DF1934) on an iPad connected with a USB-C to micro-USB
adapter/cable.

Features:

- **Scan / Continuous / Stop.** Press Return on a keyboard to scan.
- **Interactive chart.** Drag on the chart to read the wavelength and value at any point.
- **Scan configurations.** Pick one of the configs stored in the spectrometer, or apply a
  temporary custom Column or Hadamard config (range, pattern width, number of points).
- **Averaging and gain.** Set the number of scans to average, and choose automatic PGA gain or a
  fixed gain (1–64).
- **Reference and dark.** Store a reference and a dark scan, then show
  **intensity / reflectance (transmittance) / absorbance**, with optional smoothing.
- **CSV export** through the iPad share sheet (Files, AirDrop, Mail…). The export includes
  temperature, humidity, PGA gain and number of averages.
- **Demo mode** with a simulated spectrometer, so you can try the UI without hardware.

## How it works

The datasheet (p. 13) says the DF series enumerates as USB **VID 0x0451 / PID 0x4200** and uses
**HID transfers**. That VID/PID belongs to Texas Instruments' DLP NIRscan Nano reference
design, and other details fit too: a Tiva ARM Cortex-M4F with a "Tiva wake" pin, a DLPC + DMD,
Column/Hadamard/Slew scans, at most 5 slew sections and 624 patterns. So this app speaks the TI
NIRscan Nano USB protocol (documented in TI's *DLP NIRscan Nano EVM User's Guide*, DLPU030). It
decodes scans with TI's DLP Spectrum Library, which converts the raw DMD/ADC data into
wavelength + intensity using the calibration stored in each scan.

iPadOS gives apps no way to talk to a vendor-specific USB HID device directly. CoreHID and
HIDDriverKit are not available on iPad. The supported route is a **USB DriverKit driver
extension** shipped inside the app. Such drivers only run on **iPads with an M-series chip**.

```
┌───────────────── iPad app (SwiftUI) ──────────────────┐
│ ContentView / ControlsView  ← SpectrometerModel       │
│                                   │                   │
│ Spectrometer.swift ── dfcore.c (protocol, scan flow)  │
│                        └─ dlpspec (TI decode library) │
│ DextConnection.c ── IOKit user client                 │
└───────────────────────────│───────────────────────────┘
┌─ DragonflyDriver.dext (DriverKit) ─────────────────────┐
│ matches USB device 0451:4200, opens interrupt IN/OUT   │
│ endpoints, moves raw 64-byte reports                   │
└───────────────────────────│────────────────────────────┘
                     Dragonfly (micro-USB)
```

| Folder | Contents |
|---|---|
| `App/` | SwiftUI app, IOKit bridge to the driver, entitlements |
| `Driver/` | DriverKit USB driver + user client, its Info.plist (USB matching) and entitlements |
| `Core/` | `dfcore.c/.h`: transport-independent protocol + decoding in C, with a host test (`make test`) |
| `tools/df_check.py` | Optional protocol check from a Mac/PC using `hidapi` |
| `fetch_dlpspec.sh` | Downloads TI's spectrum library into `ThirdParty/dlpspec/` (not committed) |

## What you need

- An **iPad with an M1 chip or newer**: iPad Pro 11"/12.9" (2021) or later, iPad Air (2022) or
  later. The 2018 and 2020 USB-C iPad Pros (A12X/A12Z) **cannot** load USB drivers.
- **iPadOS 17** or newer.
- A **Mac with Xcode 15 or newer** (Xcode 16 recommended).
- A paid **Apple Developer Program** membership. DriverKit capabilities aren't available to free
  accounts.
- For TEC models (DF1914/1924/1934): a separate 5 V supply on the cooling input, because the
  cooler draws extra current. The iPad powers the electronics (≈300 mA @ 5 V).

## Optional first step: confirm the protocol on a computer

```bash
pip install hidapi
python3 tools/df_check.py
```

The script prints the firmware version and stored scan configs, runs one scan and saves
`scan_blob.bin`. If it ends with "Protocol check OK", the app will work with your unit. If the
device NACKs or times out, OtO's firmware differs from TI's. Please tell me what it printed.

## Building

1. Get the TI spectrum library:
   ```bash
   cd dragonfly-ipad
   ./fetch_dlpspec.sh
   ```
   This fills `ThirdParty/dlpspec/`. You can also download *DLPNIRNANOSW* from ti.com and copy the
   same files (the list is in the script) from its spectrum-library source folder.

2. **Create the app project.** In Xcode, choose *File ▸ New ▸ Project ▸ iOS ▸ App*.
   - Product Name `DragonflyScope`, Interface **SwiftUI**, Language **Swift**.
   - Save it inside the `dragonfly-ipad/` folder. You get `dragonfly-ipad/DragonflyScope/`,
     and the paths below assume that layout.
   - Delete the template's `ContentView.swift` and `DragonflyScopeApp.swift`.
   - Drag in all files from `App/` (choose *Reference files in place* / don't copy).
   - Also drag in `Core/dfcore.c`, `Core/dfcore.h`, and every `.c` file in `ThirdParty/dlpspec/`.

3. **App target ▸ Build Settings:**
   - *Objective-C Bridging Header*: `$(SRCROOT)/../App/DragonflyScope-Bridging-Header.h`
   - *Header Search Paths*: `$(SRCROOT)/../Core`, `$(SRCROOT)/../ThirdParty/dlpspec`,
     `$(SRCROOT)/../Driver`
   - *Swift Language Version*: **Swift 5**
   - *Code Signing Entitlements*: `$(SRCROOT)/../App/DragonflyScope.entitlements`
   - *General ▸ Supported Destinations*: iPad only. *Minimum Deployments*: iOS 17.0.
   - Optional: to hide warnings from TI's code, add `-w` as *Compiler Flags* for the dlpspec `.c`
     files under *Build Phases ▸ Compile Sources*.

4. **Add the driver.** Choose *File ▸ New ▸ Target ▸ DriverKit ▸ Driver*, name it
   `DragonflyDriver`, and embed it in DragonflyScope.
   - Its bundle ID must start with the app's, e.g. `com.yourname.DragonflyScope.DragonflyDriver`.
   - Replace the generated `DragonflyDriver.iig`, `DragonflyDriver.cpp`, `Info.plist` and
     `.entitlements` with the files in `Driver/`. Add `DragonflyUserClient.iig`,
     `DragonflyUserClient.cpp` and `DragonflyShared.h` to the **driver** target.
   - *Build Phases ▸ Link Binary With Libraries*: add **USBDriverKit.framework**.
   - Make sure *Info.plist File* and *Code Signing Entitlements* point to the files from `Driver/`.

5. **Signing & Capabilities.** Select your team for both targets.
   - App target: add **DriverKit Communicates with Drivers**.
   - Driver target: add **DriverKit**, **DriverKit USB Transport** (vendor ID 1105 = 0x0451) and
     **DriverKit Allow Any UserClient**.
   - If Xcode reports that your team's profile lacks a DriverKit entitlement, request it from
     Apple at <https://developer.apple.com/contact/request/system-extension/> (DriverKit, USB
     transport, vendor ID 0x0451). Development entitlements are usually granted quickly. Some
     accounts already have them.

6. **Run on the iPad** (⌘R with the iPad selected).

## First use on the iPad

1. Open **Settings ▸ DragonflyScope ▸ Drivers** and switch on **DragonflyDriver**. You only do
   this once.
2. Plug in the spectrometer. If the app keeps saying *Waiting for spectrometer…*, unplug it and
   plug it back in. The driver must claim the device when it is attached.
3. Press **Scan**. For reflectance or absorbance: scan your white standard or blank, tap **Store
   reference**, then scan the sample and switch the display mode.

## Troubleshooting

- **Driver logs.** Connect the iPad to your Mac, open Console.app, select the iPad and filter for
  `DragonflyDriver`. The driver logs which endpoints it found and any USB error codes.
- **Build errors in `Driver/*.cpp`.** USBDriverKit signatures change slightly between SDKs. The
  calls used are `IOUSBHostDevice::Open / CopyConfigurationDescriptor / SetConfiguration /
  CreateInterfaceIterator / CopyInterface`, `IOUSBHostInterface::Open / CopyPipe / CreateIOBuffer /
  DeviceRequest`, and `IOUSBHostPipe::IO` (synchronous). Check them against the headers in your
  Xcode version.
- **"Device generated N patterns (requested M)".** The firmware adjusted the custom config to
  what the DMD can actually do. That is normal.
- **Host test of the C core.** `./fetch_dlpspec.sh && make -C Core test` runs the protocol and
  decoding code against a simulated spectrometer on any Mac or Linux machine.

## Status

The protocol is TI's published NIRscan Nano protocol. OtO's datasheet doesn't list the commands,
so the first real-hardware run is the true test. `tools/df_check.py` is the quickest way to run it.
