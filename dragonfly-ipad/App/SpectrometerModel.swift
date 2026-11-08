//
//  SpectrometerModel.swift
//  App state: connection handling, settings, scanning, reference and processing.
//

import Foundation
import SwiftUI

enum DisplayMode: String, CaseIterable, Identifiable {
    case intensity = "Intensity"
    case reflectance = "Reflectance"
    case absorbance = "Absorbance"
    var id: String { rawValue }
    var axisLabel: String {
        switch self {
        case .intensity: return "Intensity (counts)"
        case .reflectance: return "Reflectance / transmittance"
        case .absorbance: return "Absorbance (−log₁₀ R)"
        }
    }
}

enum ConnectionState: Equatable {
    case searching
    case connected(DeviceInfo)
    case demo(DeviceInfo)

    var isConnected: Bool {
        if case .searching = self { return false }
        return true
    }
}

struct PlotPoint: Identifiable {
    let id: Int
    let x: Double
    let y: Double
}

@MainActor
final class SpectrometerModel: ObservableObject {
    // Connection
    @Published private(set) var connection: ConnectionState = .searching
    @Published var demoMode = false { didSet { if demoMode != oldValue { reconnect() } } }

    // Configuration
    @Published private(set) var configs: [ScanConfigInfo] = []
    @Published private(set) var activeConfig: Int = 0
    @Published private(set) var usingCustomConfig = false
    @Published var custom = CustomScanSettings()
    @Published var repeats: Int = 6
    @Published var autoGain = true
    @Published var gain: Int = 64

    // Acquisition
    @Published private(set) var isScanning = false
    @Published var continuous = false { didSet { if continuous && !isScanning { startScanning() } } }
    @Published private(set) var progressPhase = 0
    @Published private(set) var progress: Double = 0
    @Published private(set) var lastScanDuration: TimeInterval?

    // Data
    @Published private(set) var spectrum: Spectrum?
    @Published private(set) var reference: Spectrum?
    @Published private(set) var dark: Spectrum?
    @Published var displayMode: DisplayMode = .intensity
    @Published var smoothing: Int = 0   // moving-average half width in points

    @Published var errorMessage: String?

    private var backend: SpectrometerBackend?
    private let queue = DispatchQueue(label: "dragonfly.usb", qos: .userInitiated)
    private var pollTimer: Timer?
    private let cancelFlag = UnsafeMutablePointer<Int32>.allocate(capacity: 1)
    private var scanTask: Task<Void, Never>?

    init() {
        cancelFlag.initialize(to: 0)
        startPolling()
    }

    // MARK: Connection

    private func startPolling() {
        pollTimer?.invalidate()
        pollTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.pollConnection() }
        }
        pollConnection()
    }

    private var connecting = false

    private func pollConnection() {
        if let b = backend, !b.isAlive { dropConnection(message: "Spectrometer disconnected.") }
        guard backend == nil, !connecting else { return }
        connecting = true
        let demo = demoMode
        queue.async { [weak self] in
            let b: SpectrometerBackend? = demo ? SimulatedSpectrometer() : USBSpectrometer()
            var info: DeviceInfo?
            var cfg: (configs: [ScanConfigInfo], active: Int)?
            var err: Error?
            if let b {
                do {
                    info = try b.deviceInfo()
                    cfg = try? b.storedConfigs()
                } catch { err = error; b.close() }
            }
            Task { @MainActor in
                guard let self else { return }
                self.connecting = false
                guard let b, let info, err == nil, demo == self.demoMode else {
                    if let err { self.errorMessage = err.localizedDescription }
                    return
                }
                self.backend = b
                self.connection = demo ? .demo(info) : .connected(info)
                self.configs = cfg?.configs ?? []
                self.activeConfig = cfg?.active ?? 0
                self.usingCustomConfig = false
                if let c = self.configs.first(where: { $0.index == self.activeConfig }) {
                    self.repeats = max(1, c.repeats)
                }
                self.errorMessage = nil
            }
        }
    }

    private func dropConnection(message: String?) {
        continuous = false
        cancelFlag.pointee = 1
        let b = backend
        backend = nil
        queue.async { b?.close() }
        connection = .searching
        configs = []
        if let message { errorMessage = message }
    }

    func reconnect() {
        dropConnection(message: nil)
        pollConnection()
    }

    // MARK: Device calls

    private func onDevice<T>(_ work: @escaping (SpectrometerBackend) throws -> T) async throws -> T {
        guard let backend else { throw DFError.notConnected }
        return try await withCheckedThrowingContinuation { cont in
            queue.async {
                do { cont.resume(returning: try work(backend)) } catch { cont.resume(throwing: error) }
            }
        }
    }

    private func report(_ error: Error) {
        if let e = error as? DFError {
            if e.isCancellation { return }
            if e.code == DF_ERR_IO { dropConnection(message: e.localizedDescription); return }
        }
        errorMessage = error.localizedDescription
    }

    func selectConfig(_ index: Int) {
        Task {
            do {
                try await onDevice { try $0.setActiveConfig(index) }
                activeConfig = index
                usingCustomConfig = false
                if let c = configs.first(where: { $0.index == index }) { repeats = max(1, c.repeats) }
            } catch { report(error) }
        }
    }

    func applyCustomConfig() {
        let settings = custom
        let n = repeats
        Task {
            do {
                let patterns = try await onDevice { try $0.applyCustom(settings, repeats: n) }
                usingCustomConfig = true
                if patterns != settings.patterns {
                    errorMessage = "Device generated \(patterns) patterns (requested \(settings.patterns))."
                }
            } catch { report(error) }
        }
    }

    func refreshConfigs() {
        Task {
            do {
                let r = try await onDevice { try $0.storedConfigs() }
                configs = r.configs
                activeConfig = r.active
            } catch { report(error) }
        }
    }

    // MARK: Scanning

    func scanOnce() {
        guard !isScanning else { return }
        startScanning()
    }

    func stop() {
        continuous = false
        cancelFlag.pointee = 1
    }

    private func startScanning() {
        guard backend != nil, !isScanning else { return }
        isScanning = true
        scanTask = Task {
            repeat {
                guard let s = await acquire() else { break }
                spectrum = s
            } while continuous && backend != nil
            isScanning = false
            progress = 0
        }
    }

    /// Runs one acquisition with the current settings. Returns nil on error / cancel.
    private func acquire() async -> Spectrum? {
        cancelFlag.pointee = 0
        progress = 0
        progressPhase = 0
        let n = repeats, auto = autoGain, g = gain
        let flag = cancelFlag
        let started = Date()
        do {
            let s = try await onDevice { b -> Spectrum in
                try b.setRepeats(n)
                try b.setGain(auto: auto, value: g)
                return try b.scan(cancel: flag) { phase, fraction in
                    Task { @MainActor [weak self] in
                        self?.progressPhase = phase
                        self?.progress = fraction
                    }
                }
            }
            lastScanDuration = Date().timeIntervalSince(started)
            return s
        } catch {
            report(error)
            return nil
        }
    }

    // MARK: Reference / dark

    func storeReference() { if let spectrum { reference = spectrum } }
    func clearReference() { reference = nil; if displayMode != .intensity { displayMode = .intensity } }
    func storeDark() { if let spectrum { dark = spectrum } }
    func clearDark() { dark = nil }

    var canShowRatio: Bool { reference != nil }

    // MARK: Processing

    /// Points for the chart in the current display mode.
    var plotPoints: [PlotPoint] {
        guard let s = spectrum else { return [] }
        return points(for: s, mode: displayMode)
    }

    /// The stored reference, drawn faintly in intensity mode.
    var referencePoints: [PlotPoint] {
        guard displayMode == .intensity, let r = reference else { return [] }
        return points(for: r, mode: .intensity)
    }

    private func points(for s: Spectrum, mode: DisplayMode) -> [PlotPoint] {
        var y = s.intensities
        if let d = dark { y = subtract(y, s.wavelengths, d) }
        if mode != .intensity, let r = reference {
            var ref = interpolate(r.intensities, from: r.wavelengths, to: s.wavelengths)
            if let d = dark { ref = subtract(ref, s.wavelengths, d) }
            y = zip(y, ref).map { v, rv in rv > 0 ? v / rv : .nan }
            if mode == .absorbance { y = y.map { $0 > 0 ? -log10($0) : .nan } }
        }
        y = smooth(y, halfWidth: smoothing)
        return zip(s.wavelengths, y).enumerated().compactMap { i, p in
            p.1.isFinite ? PlotPoint(id: i, x: p.0, y: p.1) : nil
        }
    }

    private func subtract(_ y: [Double], _ wl: [Double], _ d: Spectrum) -> [Double] {
        let dk = interpolate(d.intensities, from: d.wavelengths, to: wl)
        return zip(y, dk).map { $0 - $1 }
    }

    private func interpolate(_ v: [Double], from x: [Double], to xi: [Double]) -> [Double] {
        if x == xi { return v }
        guard x.count > 1 else { return xi.map { _ in .nan } }
        // Sort by wavelength (slew scans are already sorted; be safe).
        let pairs = zip(x, v).sorted { $0.0 < $1.0 }
        let xs = pairs.map(\.0), vs = pairs.map(\.1)
        return xi.map { t in
            if t <= xs[0] { return vs[0] }
            if t >= xs[xs.count - 1] { return vs[vs.count - 1] }
            var lo = 0, hi = xs.count - 1
            while hi - lo > 1 { let m = (lo + hi) / 2; if xs[m] <= t { lo = m } else { hi = m } }
            let f = (t - xs[lo]) / (xs[hi] - xs[lo])
            return vs[lo] + f * (vs[hi] - vs[lo])
        }
    }

    private func smooth(_ y: [Double], halfWidth w: Int) -> [Double] {
        guard w > 0, y.count > 2 * w else { return y }
        return y.indices.map { i in
            let lo = max(0, i - w), hi = min(y.count - 1, i + w)
            let window = y[lo...hi].filter(\.isFinite)
            return window.isEmpty ? .nan : window.reduce(0, +) / Double(window.count)
        }
    }

    // MARK: Export

    func csv() -> String {
        guard let s = spectrum else { return "" }
        var out = "# DragonflyScope export \(ISO8601DateFormatter().string(from: s.date))\n"
        out += "# config: \(s.metadata.configName), serial: \(s.metadata.serial), PGA: \(s.metadata.pga), "
        out += "averages: \(s.metadata.repeats), system temp C: \(s.metadata.systemTempC), "
        out += "detector temp C: \(s.metadata.detectorTempC), humidity %: \(s.metadata.humidityPct)\n"
        let ref = reference.map { interpolate($0.intensities, from: $0.wavelengths, to: s.wavelengths) }
        let dk = dark.map { interpolate($0.intensities, from: $0.wavelengths, to: s.wavelengths) }
        out += "wavelength_nm,intensity"
        if dk != nil { out += ",dark" }
        if ref != nil { out += ",reference,reflectance,absorbance" }
        out += "\n"
        for i in s.wavelengths.indices {
            let d = dk?[i] ?? 0
            var line = String(format: "%.3f,%.0f", s.wavelengths[i], s.intensities[i])
            if let dk { line += String(format: ",%.0f", dk[i]) }
            if let ref {
                let r = ref[i] - d
                let refl = r > 0 ? (s.intensities[i] - d) / r : .nan
                let abs = refl > 0 ? -log10(refl) : .nan
                line += String(format: ",%.0f,%.6f,%.6f", ref[i], refl, abs)
            }
            out += line + "\n"
        }
        return out
    }
}
