//
//  ASFWApp.swift
//  ASFW
//
//  Created by Alexander Shabelnikov on 21.09.2025.
//

import SwiftUI

@main
struct ASFWApp: App {
    private let autoActivateDriverOnLaunch = ProcessInfo.processInfo.arguments.contains("--activate-driver")

    var body: some Scene {
        WindowGroup {
            ModernContentView(autoActivateDriverOnLaunch: autoActivateDriverOnLaunch)
        }
        .defaultSize(width: 1000, height: 700)
    }
}
