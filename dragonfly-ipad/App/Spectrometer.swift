//
//  Spectrometer.swift
//  Swift-facing types and the two backends: the real USB device (through the DriverKit
//  extension + dfcore) and a simulator for trying the app without hardware.
//
//  Backends are blocking and NOT thread-safe; SpectrometerModel calls them from one serial queue.
//

import Foundation

// MARK: - Value types

struct DeviceInfo: Equatable {
    var model: String
    var serial: String
    var firmware: String
}

struct ScanSection: Equatable {
    var type: ScanType
    var startNm: Int
    var endNm: Int
    var widthPx: Int
    var patterns: Int
    var exposure: Int
}

enum ScanType: Int, CaseIterable, Identifiable {
    case column = 0, hadamard = 1, slew = 2
    var id: Int { rawValue }
    var label: String {
        switch self {
        case .column: return "Column"
        case .hadamard: return "Hadamard"
        case .slew: return "Slew"
        }
    }
}

struct ScanConfigInfo: Identifiable, Equatable {
    var index: Int
    var name: String
    var type: ScanType
    var repeats: Int
    var sections: [ScanSection]
    var id: Int { index }

    var summary: String {
        guard let first = sections.first, let last = sections.last else { return type.label }
        let points = sections.reduce(0) { $0 + $1.patterns }
        return "\(type.label) · \(first.startNm)–\(last.endNm) nm · \(points) pts"
    }
}

struct ScanMetadata: Equatable {
    var configName: String
    var serial: String
    var pga: Int
    var repeats: Int
    var systemTempC: Double
    var detectorTempC: Double
    var humidityPct: Double
}

struct Spectrum: Equatable {
    var wavelengths: [Double]
    var intensities: [Double]
    var date: Date
    var metadata: ScanMetadata
}

struct CustomScanSettings: Equatable {
    var type: ScanType = .hadamard
    var startNm: Int = 900
    var endNm: Int = 1700
    var widthPx: Int = 6
    var patterns: Int = 228
}

struct DFError: LocalizedError {
    /// One of the DF_ERR_* values from dfcore.h (anonymous C enum constants import as Int).
    var code: Int
    var context: String
    var isCancellation: Bool { code == DF_ERR_CANCELLED }
    var errorDescription: String? {
        let base = String(cString: df_strerror(Int32(code)))
        return context.isEmpty ? base : "\(context): \(base)"
    }
    static let notConnected = DFError(code: DF_ERR_IO, context: "Not connected")
}

@discardableResult
private func check(_ result: Int32, _ context: String) throws -> Int32 {
    if result < 0 { throw DFError(code: Int(result), context: context) }
    return result
}

// MARK: - Backend protocol

protocol SpectrometerBackend: AnyObject {
    var isAlive: Bool { get }
    func deviceInfo() throws -> DeviceInfo
    func storedConfigs() throws -> (configs: [ScanConfigInfo], active: Int)
    func setActiveConfig(_ index: Int) throws
    func applyCustom(_ s: CustomScanSettings, repeats: Int) throws -> Int
    func setRepeats(_ n: Int) throws
    func setGain(auto: Bool, value: Int) throws
    func scan(cancel: UnsafeMutablePointer<Int32>, progress: @escaping (Int, Double) -> Void) throws -> Spectrum
    func close()
}

// MARK: - Helpers for C fixed-size arrays (imported as tuples)

private func string<T>(fromCTuple tuple: T) -> String {
    withUnsafeBytes(of: tuple) { raw in
        let bytes = raw.prefix { $0 != 0 }
        return String(decoding: bytes, as: UTF8.self)
    }
}

private func copy<T>(_ s: String, intoCTuple tuple: inout T) {
    withUnsafeMutableBytes(of: &tuple) { raw in
        raw.initializeMemory(as: UInt8.self, repeating: 0)
        let utf8 = Array(s.utf8.prefix(raw.count - 1))
        raw.copyBytes(from: utf8)
    }
}

private func sections(of c: df_config) -> [df_section] {
    withUnsafeBytes(of: c.sections) { Array($0.bindMemory(to: df_section.self)) }
}

private func versionString(_ v: UInt32) -> String {
    "\((v >> 16) & 0xFF).\((v >> 8) & 0xFF).\(v & 0xFF)"
}

private final class ProgressBox {
    let handler: (Int, Double) -> Void
    init(_ h: @escaping (Int, Double) -> Void) { handler = h }
}

// MARK: - Real device

final class USBSpectrometer: SpectrometerBackend, @unchecked Sendable {
    private let connection: OpaquePointer
    private let dev: UnsafeMutablePointer<df_device>

    /// Returns nil when the driver extension has not matched a device (unplugged / not enabled).
    init?() {
        guard let c = dext_open() else { return nil }
        connection = c
        dev = .allocate(capacity: 1)
        dev.initialize(to: df_device())
        dext_bind_device(c, dev)
    }

    deinit { close() ; dev.deallocate() }

    private var closed = false
    func close() {
        if !closed { dext_close(connection); closed = true }
    }

    var isAlive: Bool { !closed && dext_is_alive(connection) }

    func deviceInfo() throws -> DeviceInfo {
        var v = df_version()
        try check(df_get_version(dev, &v), "Reading firmware version")
        var sn = [CChar](repeating: 0, count: 9)
        var model = [CChar](repeating: 0, count: 17)
        // Not every firmware implements these two; ignore failures.
        _ = df_get_serial_number(dev, &sn)
        _ = df_get_model_name(dev, &model)
        return DeviceInfo(model: String(cString: model).isEmpty ? "Dragonfly" : String(cString: model),
                          serial: String(cString: sn),
                          firmware: versionString(v.tiva_sw))
    }

    func storedConfigs() throws -> (configs: [ScanConfigInfo], active: Int) {
        let count = Int(try check(df_get_num_configs(dev), "Reading number of scan configs"))
        var list: [ScanConfigInfo] = []
        for i in 0..<count {
            var c = df_config()
            if df_read_config(dev, UInt8(i), &c) < 0 { continue }
            let secs = sections(of: c).prefix(Int(c.num_sections)).map {
                ScanSection(type: ScanType(rawValue: Int($0.scan_type)) ?? .column,
                            startNm: Int($0.wavelength_start_nm), endNm: Int($0.wavelength_end_nm),
                            widthPx: Int($0.width_px), patterns: Int($0.num_patterns),
                            exposure: Int($0.exposure_time))
            }
            list.append(ScanConfigInfo(index: i, name: string(fromCTuple: c.name),
                                       type: ScanType(rawValue: Int(c.scan_type)) ?? .column,
                                       repeats: Int(c.num_repeats), sections: Array(secs)))
        }
        let active = Int(df_get_active_config(dev))
        return (list, max(active, 0))
    }

    func setActiveConfig(_ index: Int) throws {
        try check(df_set_active_config(dev, UInt8(index)), "Selecting scan config")
    }

    func applyCustom(_ s: CustomScanSettings, repeats: Int) throws -> Int {
        var c = df_config()
        c.scan_type = UInt8(s.type == .slew ? ScanType.hadamard.rawValue : s.type.rawValue)
        c.num_repeats = UInt16(clamping: repeats)
        c.num_sections = 1
        copy("iPad custom", intoCTuple: &c.name)
        c.sections.0 = df_section(scan_type: c.scan_type, width_px: UInt8(clamping: s.widthPx),
                                  wavelength_start_nm: UInt16(clamping: s.startNm),
                                  wavelength_end_nm: UInt16(clamping: s.endNm),
                                  num_patterns: UInt16(clamping: s.patterns), exposure_time: 0)
        return Int(try check(df_apply_config(dev, &c), "Applying scan config"))
    }

    func setRepeats(_ n: Int) throws {
        try check(df_set_num_repeats(dev, UInt16(clamping: n)), "Setting scans to average")
    }

    func setGain(auto: Bool, value: Int) throws {
        try check(df_set_pga_gain(dev, !auto, UInt8(clamping: value)), "Setting PGA gain")
    }

    func scan(cancel: UnsafeMutablePointer<Int32>, progress: @escaping (Int, Double) -> Void) throws -> Spectrum {
        let result = UnsafeMutablePointer<df_scan_result>.allocate(capacity: 1)
        defer { result.deallocate() }
        let box = ProgressBox(progress)
        let ctx = Unmanaged.passRetained(box).toOpaque()
        defer { Unmanaged<ProgressBox>.fromOpaque(ctx).release() }
        let cb: df_progress_fn = { ctx, phase, fraction in
            guard let ctx else { return }
            Unmanaged<ProgressBox>.fromOpaque(ctx).takeUnretainedValue().handler(Int(phase), fraction)
        }
        try check(df_scan(dev, cancel, cb, ctx, result), "Scan")

        let r = result.pointee
        let n = Int(r.length)
        let wl = withUnsafeBytes(of: r.wavelength_nm) { Array($0.bindMemory(to: Double.self).prefix(n)) }
        let it = withUnsafeBytes(of: r.intensity) { Array($0.bindMemory(to: Int32.self).prefix(n)) }
        let meta = ScanMetadata(configName: string(fromCTuple: r.config_name),
                                serial: string(fromCTuple: r.serial_number),
                                pga: Int(r.pga), repeats: Int(r.num_repeats),
                                systemTempC: r.system_temp_c, detectorTempC: r.detector_temp_c,
                                humidityPct: r.humidity_pct)
        return Spectrum(wavelengths: wl, intensities: it.map(Double.init), date: Date(), metadata: meta)
    }
}

// MARK: - Simulator (demo mode)

final class SimulatedSpectrometer: SpectrometerBackend, @unchecked Sendable {
    private var configs: [ScanConfigInfo] = [
        ScanConfigInfo(index: 0, name: "Column 1", type: .column, repeats: 6,
                       sections: [ScanSection(type: .column, startNm: 900, endNm: 1700, widthPx: 6, patterns: 228, exposure: 0)]),
        ScanConfigInfo(index: 1, name: "Hadamard 1", type: .hadamard, repeats: 6,
                       sections: [ScanSection(type: .hadamard, startNm: 900, endNm: 1700, widthPx: 7, patterns: 228, exposure: 0)]),
        ScanConfigInfo(index: 2, name: "Slew 5 sections", type: .slew, repeats: 1,
                       sections: [900, 1000, 1200, 1400, 1600].enumerated().map { i, s in
                           ScanSection(type: .column, startNm: s, endNm: [1000, 1200, 1400, 1600, 1700][i],
                                       widthPx: 2 + i, patterns: 20, exposure: i / 2)
                       }),
    ]
    private var active = 1
    private var repeats = 6
    private var gain = 64
    private var autoGain = true
    private var custom: CustomScanSettings?

    var isAlive: Bool { true }
    func close() {}

    func deviceInfo() throws -> DeviceInfo {
        DeviceInfo(model: "Simulated DF1514", serial: "DEMO0001", firmware: "demo")
    }
    func storedConfigs() throws -> (configs: [ScanConfigInfo], active: Int) { (configs, active) }
    func setActiveConfig(_ index: Int) throws { active = index; custom = nil }
    func applyCustom(_ s: CustomScanSettings, repeats: Int) throws -> Int { custom = s; return s.patterns }
    func setRepeats(_ n: Int) throws { repeats = max(1, n) }
    func setGain(auto: Bool, value: Int) throws { autoGain = auto; gain = value }

    func scan(cancel: UnsafeMutablePointer<Int32>, progress: @escaping (Int, Double) -> Void) throws -> Spectrum {
        var start = 900.0, end = 1700.0, n = 228, name = "iPad custom"
        if let c = custom {
            start = Double(c.startNm); end = Double(c.endNm); n = max(c.patterns, 2)
        } else {
            let c = configs[active]
            start = Double(c.sections.first?.startNm ?? 900)
            end = Double(c.sections.last?.endNm ?? 1700)
            n = max(c.sections.reduce(0) { $0 + $1.patterns }, 2)
            name = c.name
        }
        let steps = 10
        for i in 0..<steps {
            if cancel.pointee != 0 { throw DFError(code: DF_ERR_CANCELLED, context: "") }
            Thread.sleep(forTimeInterval: 0.03 * Double(repeats) / Double(steps) + 0.02)
            progress(0, Double(i + 1) / Double(steps))
        }
        let g = autoGain ? 64.0 : Double(gain)
        let noise = 1500.0 / sqrt(Double(repeats))
        var wl: [Double] = [], y: [Double] = []
        for i in 0..<n {
            let x = start + (end - start) * Double(i) / Double(n - 1)
            // Lamp-through-InGaAs-like curve similar to the data sheet's DF1514 plot,
            // with two water-ish absorption dips.
            let rise = 1 / (1 + exp(-(x - 960) / 25))
            let fall = 1 / (1 + exp((x - 1590) / 35))
            let hump = 1 + 0.25 * exp(-pow((x - 1400) / 120, 2))
            let dips = 1 - 0.12 * exp(-pow((x - 1080) / 30, 2)) - 0.18 * exp(-pow((x - 1450) / 40, 2))
            let value = 4800 * g * rise * fall * hump * dips + Double.random(in: -noise...noise)
            wl.append(x)
            y.append(max(0, value))
        }
        let meta = ScanMetadata(configName: name, serial: "DEMO0001", pga: Int(g), repeats: repeats,
                                systemTempC: 27.3 + Double.random(in: -0.1...0.1),
                                detectorTempC: 26.1, humidityPct: 41.0)
        return Spectrum(wavelengths: wl, intensities: y, date: Date(), metadata: meta)
    }
}
