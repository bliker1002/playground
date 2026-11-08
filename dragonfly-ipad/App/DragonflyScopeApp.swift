//
//  DragonflyScopeApp.swift
//  View the spectrum of an OtO Photonics Dragonfly (DF1514 / DF1914 / DF1924 / DF1934)
//  spectrometer on an iPad over USB.
//

import SwiftUI

@main
struct DragonflyScopeApp: App {
    @StateObject private var model = SpectrometerModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
        }
    }
}
