//
//  ControlsView.swift
//  Sidebar: scan configuration, acquisition settings, reference/dark, display options.
//

import SwiftUI

struct ControlsView: View {
    @EnvironmentObject var model: SpectrometerModel
    @State private var showCustom = false

    private let gains = [1, 2, 4, 8, 16, 32, 64]

    var body: some View {
        Form {
            Section("Scan configuration") {
                if model.configs.isEmpty {
                    Text(model.connection.isConnected ? "No stored configurations reported." : "Not connected")
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(model.configs) { cfg in
                        Button { model.selectConfig(cfg.index) } label: {
                            HStack {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(cfg.name.isEmpty ? "Config \(cfg.index + 1)" : cfg.name)
                                        .foregroundStyle(.primary)
                                    Text(cfg.summary).font(.caption).foregroundStyle(.secondary)
                                }
                                Spacer()
                                if !model.usingCustomConfig && cfg.index == model.activeConfig {
                                    Image(systemName: "checkmark").foregroundStyle(.blue)
                                }
                            }
                        }
                        .disabled(model.isScanning)
                    }
                }
                DisclosureGroup("Custom (temporary)", isExpanded: $showCustom) {
                    Picker("Type", selection: $model.custom.type) {
                        Text("Column").tag(ScanType.column)
                        Text("Hadamard").tag(ScanType.hadamard)
                    }
                    Stepper("Start \(model.custom.startNm) nm", value: $model.custom.startNm, in: 800...2300, step: 10)
                    Stepper("End \(model.custom.endNm) nm", value: $model.custom.endNm, in: 800...2300, step: 10)
                    Stepper("Width \(model.custom.widthPx) px (≈\(String(format: "%.1f", Double(model.custom.widthPx) * 1.17)) nm)",
                            value: $model.custom.widthPx, in: 2...52)
                    Stepper("Points \(model.custom.patterns)", value: $model.custom.patterns, in: 2...624, step: 2)
                    Button("Apply custom configuration") { model.applyCustomConfig() }
                        .disabled(!model.connection.isConnected || model.isScanning
                                  || model.custom.endNm <= model.custom.startNm)
                    if model.usingCustomConfig {
                        Label("Custom configuration active", systemImage: "checkmark.circle")
                            .foregroundStyle(.blue).font(.caption)
                    }
                }
            }

            Section {
                Stepper("Average \(model.repeats) scan\(model.repeats == 1 ? "" : "s")",
                        value: $model.repeats, in: 1...1000)
                Toggle("Automatic gain", isOn: $model.autoGain)
                if !model.autoGain {
                    Picker("PGA gain", selection: $model.gain) {
                        ForEach(gains, id: \.self) { Text("×\($0)").tag($0) }
                    }
                }
            } header: {
                Text("Acquisition")
            } footer: {
                if let t = model.lastScanDuration {
                    Text(String(format: "Last scan took %.1f s.", t))
                }
            }

            Section {
                HStack {
                    Button("Store reference") { model.storeReference() }
                        .disabled(model.spectrum == nil)
                    Spacer()
                    if model.reference != nil {
                        Button("Clear", role: .destructive) { model.clearReference() }
                    }
                }
                HStack {
                    Button("Store dark") { model.storeDark() }
                        .disabled(model.spectrum == nil)
                    Spacer()
                    if model.dark != nil {
                        Button("Clear", role: .destructive) { model.clearDark() }
                    }
                }
            } header: {
                Text("Reference")
            } footer: {
                Text("Scan a white standard (reflection) or the empty path / blank (transmission) and store it as the reference. Optionally block the light, scan, and store as dark; it is subtracted from sample and reference.")
            }

            Section("Display") {
                Stepper(model.smoothing == 0 ? "Smoothing off" : "Smoothing ±\(model.smoothing) pts",
                        value: $model.smoothing, in: 0...10)
            }

            Section("Device") {
                Toggle("Demo mode (no hardware)", isOn: $model.demoMode)
                Button("Reload configurations") { model.refreshConfigs() }
                    .disabled(!model.connection.isConnected || model.isScanning)
                Button("Reconnect") { model.reconnect() }
            }
        }
    }
}
