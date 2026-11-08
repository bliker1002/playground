//
//  ContentView.swift
//

import SwiftUI
import Charts
import UniformTypeIdentifiers

struct ContentView: View {
    @EnvironmentObject var model: SpectrometerModel

    var body: some View {
        NavigationSplitView {
            ControlsView()
                .navigationTitle("Dragonfly")
                .navigationSplitViewColumnWidth(min: 300, ideal: 340, max: 400)
        } detail: {
            VStack(spacing: 0) {
                StatusBar()
                Divider()
                if model.connection.isConnected {
                    SpectrumChart()
                        .padding()
                } else {
                    ConnectHelpView()
                }
            }
            .toolbar { ScanToolbar(model: model) }
            .navigationBarTitleDisplayMode(.inline)
        }
        .alert("Spectrometer", isPresented: Binding(
            get: { model.errorMessage != nil },
            set: { if !$0 { model.errorMessage = nil } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(model.errorMessage ?? "")
        }
    }
}

// MARK: - Toolbar

struct ScanToolbar: ToolbarContent {
    @ObservedObject var model: SpectrometerModel

    var body: some ToolbarContent {
        ToolbarItemGroup(placement: .primaryAction) {
            if model.spectrum != nil {
                ShareLink(item: CSVExport(text: model.csv()),
                          preview: SharePreview("spectrum.csv")) {
                    Label("Export CSV", systemImage: "square.and.arrow.up")
                }
            }
            Toggle(isOn: $model.continuous) {
                Label("Continuous", systemImage: "repeat")
            }
            .toggleStyle(.button)
            .disabled(!model.connection.isConnected)

            if model.isScanning {
                Button(role: .destructive) { model.stop() } label: {
                    Label("Stop", systemImage: "stop.fill")
                }
            } else {
                Button { model.scanOnce() } label: {
                    Label("Scan", systemImage: "play.fill")
                }
                .keyboardShortcut(.return, modifiers: [])
                .disabled(!model.connection.isConnected)
            }
        }
    }
}

struct CSVExport: Transferable {
    let text: String
    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(exportedContentType: .commaSeparatedText) { item in
            let formatter = DateFormatter()
            formatter.dateFormat = "yyyyMMdd-HHmmss"
            let url = FileManager.default.temporaryDirectory
                .appendingPathComponent("spectrum-\(formatter.string(from: Date())).csv")
            try item.text.write(to: url, atomically: true, encoding: .utf8)
            return SentTransferredFile(url)
        }
    }
}

// MARK: - Status bar

struct StatusBar: View {
    @EnvironmentObject var model: SpectrometerModel

    var body: some View {
        HStack(spacing: 16) {
            switch model.connection {
            case .searching:
                ProgressView().controlSize(.small)
                Text("Waiting for spectrometer…").foregroundStyle(.secondary)
            case .connected(let info), .demo(let info):
                Image(systemName: model.demoMode ? "testtube.2" : "cable.connector")
                    .foregroundStyle(.green)
                Text("\(info.model)  ·  S/N \(info.serial.isEmpty ? "—" : info.serial)  ·  FW \(info.firmware)")
                    .font(.callout)
            }
            Spacer()
            if model.isScanning {
                Text(model.progressPhase == 0 ? "Scanning" : "Downloading")
                    .font(.callout).foregroundStyle(.secondary)
                ProgressView(value: model.progress).frame(width: 140)
            } else if let s = model.spectrum {
                let m = s.metadata
                Text(String(format: "%@  ·  PGA %d  ·  %d avg  ·  %.1f °C  ·  %.0f %% RH",
                            m.configName, m.pga, m.repeats, m.systemTempC, m.humidityPct))
                    .font(.caption).foregroundStyle(.secondary)
                    .lineLimit(1)
            }
        }
        .padding(.horizontal)
        .padding(.vertical, 8)
    }
}

// MARK: - Chart

struct SpectrumChart: View {
    @EnvironmentObject var model: SpectrometerModel
    @State private var selectedX: Double?

    var body: some View {
        let points = model.plotPoints
        let refPoints = model.referencePoints
        let selected = nearest(points, to: selectedX)

        VStack(alignment: .leading, spacing: 8) {
            Picker("Display", selection: $model.displayMode) {
                ForEach(DisplayMode.allCases) { mode in
                    Text(mode.rawValue).tag(mode)
                        .disabled(mode != .intensity && !model.canShowRatio)
                }
            }
            .pickerStyle(.segmented)
            .frame(maxWidth: 420)

            if model.displayMode != .intensity && !model.canShowRatio {
                Text("Store a reference (white standard / blank) first to show reflectance or absorbance.")
                    .font(.callout).foregroundStyle(.orange)
            }

            if points.isEmpty {
                ContentUnavailableView("No spectrum yet", systemImage: "waveform.path.ecg",
                                       description: Text("Tap Scan (or press Return) to take a measurement."))
            } else {
                Chart {
                    ForEach(refPoints) { p in
                        LineMark(x: .value("Wavelength", p.x), y: .value("Value", p.y),
                                 series: .value("Series", "Reference"))
                            .foregroundStyle(.gray.opacity(0.5))
                            .lineStyle(StrokeStyle(lineWidth: 1, dash: [4, 3]))
                    }
                    ForEach(points) { p in
                        LineMark(x: .value("Wavelength", p.x), y: .value("Value", p.y),
                                 series: .value("Series", "Sample"))
                            .foregroundStyle(.blue)
                            .lineStyle(StrokeStyle(lineWidth: 1.6))
                    }
                    if let s = selected {
                        RuleMark(x: .value("Wavelength", s.x))
                            .foregroundStyle(.secondary.opacity(0.6))
                        PointMark(x: .value("Wavelength", s.x), y: .value("Value", s.y))
                            .foregroundStyle(.blue)
                            .annotation(position: .top, alignment: .center) {
                                Text(String(format: "%.1f nm\n%@", s.x, format(s.y)))
                                    .font(.caption.monospacedDigit())
                                    .padding(6)
                                    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 6))
                            }
                    }
                }
                .chartXSelection(value: $selectedX)
                .chartXAxisLabel("Wavelength (nm)")
                .chartYAxisLabel(model.displayMode.axisLabel)
                .chartXScale(domain: xDomain(points))
            }
        }
    }

    private func xDomain(_ p: [PlotPoint]) -> ClosedRange<Double> {
        guard let lo = p.map(\.x).min(), let hi = p.map(\.x).max(), hi > lo else { return 0...1 }
        return lo...hi
    }

    private func nearest(_ p: [PlotPoint], to x: Double?) -> PlotPoint? {
        guard let x, !p.isEmpty else { return nil }
        return p.min { abs($0.x - x) < abs($1.x - x) }
    }

    private func format(_ v: Double) -> String {
        model.displayMode == .intensity ? String(format: "%.0f", v) : String(format: "%.4f", v)
    }
}

// MARK: - Not connected

struct ConnectHelpView: View {
    @EnvironmentObject var model: SpectrometerModel

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                Label("Connect your Dragonfly", systemImage: "cable.connector")
                    .font(.title2.bold())
                VStack(alignment: .leading, spacing: 10) {
                    step(1, "Plug the spectrometer into the iPad with the USB-C to micro-USB adapter/cable. The green power LED should light up.")
                    step(2, "The first time: open Settings → DragonflyScope → Drivers and switch on “DragonflyDriver”.")
                    step(3, "If nothing happens within a few seconds, unplug the spectrometer and plug it in again so the driver can claim it.")
                    step(4, "TEC-cooled models (DF19x4): the cooler needs its own 5 V supply on the cooling input; the iPad only powers the electronics.")
                }
                HStack {
                    Button("Open Settings") {
                        if let url = URL(string: UIApplication.openSettingsURLString) {
                            UIApplication.shared.open(url)
                        }
                    }
                    .buttonStyle(.borderedProminent)
                    Button("Try demo mode") { model.demoMode = true }
                        .buttonStyle(.bordered)
                }
                Text("This needs an iPad with an M-series chip (iPad Pro 2021 or later, iPad Air 2022 or later) running iPadOS 17 or newer. Older iPads cannot load USB drivers.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
            .frame(maxWidth: 560, alignment: .leading)
            .padding(32)
        }
    }

    private func step(_ n: Int, _ text: String) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Text("\(n)").font(.headline).frame(width: 24, height: 24)
                .background(Circle().fill(.blue.opacity(0.15)))
            Text(text)
        }
    }
}
