import AppKit
import Combine
import SwiftUI
import simd

/// Visual themes for the Audio Analyzer dashboard.
/// Guarantees high-contrast readability regardless of whether macOS is set to Light or Dark appearance.
nonisolated enum AnalyzerThemeMode: String, CaseIterable, Identifiable, Sendable {
    case studioDark = "Studio Dark"
    case studioLight = "Studio Light"
    case highContrast = "High Contrast"
    case oledBlack = "OLED Black"

    var id: String { rawValue }

    var isLight: Bool {
        self == .studioLight
    }

    var readoutPrimary: SIMD4<Float> {
        switch self {
        case .studioDark:
            return SIMD4<Float>(0.96, 0.98, 1.0, 1.0) // Crisp near-white
        case .studioLight:
            return SIMD4<Float>(0.07, 0.08, 0.10, 1.0) // Deep black/charcoal
        case .highContrast:
            return SIMD4<Float>(1.0, 1.0, 1.0, 1.0) // Pure 100% white
        case .oledBlack:
            return SIMD4<Float>(0.98, 0.98, 0.98, 1.0)
        }
    }

    var readoutSecondary: SIMD4<Float> {
        switch self {
        case .studioDark:
            return SIMD4<Float>(0.72, 0.78, 0.84, 1.0) // Legible cool gray
        case .studioLight:
            return SIMD4<Float>(0.38, 0.42, 0.48, 1.0) // Legible dark slate
        case .highContrast:
            return SIMD4<Float>(0.88, 0.92, 0.96, 1.0) // High-luminance secondary
        case .oledBlack:
            return SIMD4<Float>(0.70, 0.74, 0.78, 1.0)
        }
    }

    var primaryTextColor: Color {
        switch self {
        case .studioLight:
            return Color(red: 0.07, green: 0.08, blue: 0.10)
        case .studioDark, .highContrast, .oledBlack:
            return Color.white
        }
    }

    var secondaryTextColor: Color {
        switch self {
        case .studioLight:
            return Color(red: 0.38, green: 0.42, blue: 0.48)
        case .studioDark:
            return Color(red: 0.72, green: 0.78, blue: 0.84)
        case .highContrast:
            return Color(red: 0.88, green: 0.92, blue: 0.96)
        case .oledBlack:
            return Color(red: 0.70, green: 0.74, blue: 0.78)
        }
    }

    var cardBackgroundTop: Color {
        switch self {
        case .studioDark:
            return Color(red: 0.105, green: 0.14, blue: 0.165)
        case .studioLight:
            return Color(red: 0.98, green: 0.985, blue: 0.99)
        case .highContrast:
            return Color(red: 0.08, green: 0.09, blue: 0.11)
        case .oledBlack:
            return Color.black
        }
    }

    var cardBackgroundBottom: Color {
        switch self {
        case .studioDark:
            return Color(red: 0.065, green: 0.085, blue: 0.10)
        case .studioLight:
            return Color(red: 0.94, green: 0.95, blue: 0.96)
        case .highContrast:
            return Color(red: 0.05, green: 0.06, blue: 0.07)
        case .oledBlack:
            return Color.black
        }
    }

    var cardBorder: Color {
        switch self {
        case .studioDark:
            return Color.white.opacity(0.09)
        case .studioLight:
            return Color.black.opacity(0.12)
        case .highContrast:
            return Color.white.opacity(0.25)
        case .oledBlack:
            return Color.white.opacity(0.15)
        }
    }

    var gridLineColor: Color {
        switch self {
        case .studioDark:
            return Color.white.opacity(0.12)
        case .studioLight:
            return Color.black.opacity(0.15)
        case .highContrast:
            return Color.white.opacity(0.35)
        case .oledBlack:
            return Color.white.opacity(0.20)
        }
    }

    var outerBackground: Color {
        switch self {
        case .studioLight:
            return Color(red: 0.92, green: 0.93, blue: 0.94)
        case .studioDark, .highContrast:
            return Color(red: 0.08, green: 0.09, blue: 0.10)
        case .oledBlack:
            return Color.black
        }
    }
}

/// Global shared theme provider for SwiftUI and Metal plot views.
@MainActor
final class AnalyzerThemeState: ObservableObject {
    static let shared = AnalyzerThemeState()

    @AppStorage("analyzerThemeMode") var modeRaw: String = AnalyzerThemeMode.studioDark.rawValue {
        didSet {
            mode = AnalyzerThemeMode(rawValue: modeRaw) ?? .studioDark
        }
    }

    @Published private(set) var mode: AnalyzerThemeMode = .studioDark

    init() {
        if let stored = UserDefaults.standard.string(forKey: "analyzerThemeMode"),
           let parsed = AnalyzerThemeMode(rawValue: stored) {
            mode = parsed
        }
    }

    func setMode(_ next: AnalyzerThemeMode) {
        mode = next
        modeRaw = next.rawValue
    }
}
