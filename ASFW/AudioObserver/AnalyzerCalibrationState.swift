import Combine
import Foundation
import SwiftUI

/// Configuration for non-destructive metering calibration offset.
/// Allows matching pre-room-correction programme loudness (e.g. Sonarworks SoundID Reference)
/// without altering audio samples passing through the driver to hardware.
nonisolated struct AnalyzerCalibrationConfig: Sendable, Equatable {
    var offsetDB: Double = 0.0
    var isEnabled: Bool = true

    var effectiveOffsetDB: Double {
        isEnabled ? offsetDB : 0.0
    }

    var linearGain: Float {
        effectiveOffsetDB == 0.0 ? 1.0 : pow(10.0, Float(effectiveOffsetDB) / 20.0)
    }

    var energyScale: Float {
        effectiveOffsetDB == 0.0 ? 1.0 : pow(10.0, Float(effectiveOffsetDB) / 10.0)
    }

    static let minimumOffsetDB: Double = -24.0
    static let maximumOffsetDB: Double = 24.0

    struct Preset: Identifiable {
        let id: String
        let label: String
        let offsetDB: Double
    }

    static let presets: [Preset] = [
        Preset(id: "none", label: "0 dB (None)", offsetDB: 0.0),
        Preset(id: "soundid_37", label: "+3.7 dB (SoundID)", offsetDB: 3.7),
        Preset(id: "plus3", label: "+3.0 dB", offsetDB: 3.0),
        Preset(id: "plus6", label: "+6.0 dB", offsetDB: 6.0),
        Preset(id: "k14", label: "-14 dB (K-14)", offsetDB: -14.0),
        Preset(id: "k20", label: "-20 dB (K-20)", offsetDB: -20.0),
    ]
}

/// Observable state manager for Audio Analyzer metering calibration.
@MainActor
final class AnalyzerCalibrationState: ObservableObject {
    static let shared = AnalyzerCalibrationState()

    @AppStorage("analyzerCalibrationOffsetDB") var offsetDB: Double = 0.0 {
        didSet { sync() }
    }

    @AppStorage("analyzerCalibrationEnabled") var isEnabled: Bool = true {
        didSet { sync() }
    }

    @Published private(set) var config: AnalyzerCalibrationConfig = .init()

    init() {
        let storedOffset = UserDefaults.standard.double(forKey: "analyzerCalibrationOffsetDB")
        let storedEnabled = UserDefaults.standard.object(forKey: "analyzerCalibrationEnabled") as? Bool ?? true
        config = AnalyzerCalibrationConfig(offsetDB: storedOffset, isEnabled: storedEnabled)
    }

    func setOffset(_ value: Double) {
        let clamped = min(AnalyzerCalibrationConfig.maximumOffsetDB,
                          max(AnalyzerCalibrationConfig.minimumOffsetDB, (value * 10).rounded() / 10))
        offsetDB = clamped
        isEnabled = true
        sync()
    }

    func toggleEnabled() {
        isEnabled.toggle()
        sync()
    }

    func reset() {
        offsetDB = 0.0
        isEnabled = true
        sync()
    }

    private func sync() {
        config = AnalyzerCalibrationConfig(offsetDB: offsetDB, isEnabled: isEnabled)
    }
}
