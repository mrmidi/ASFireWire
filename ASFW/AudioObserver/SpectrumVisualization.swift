nonisolated enum SpectrumVisualization: String, CaseIterable {
    case spectrum = "Spectrum"
    case spectrogram = "2D Spectrogram"
    case waterfall = "3D Waterfall"

    var usesHistory: Bool { self != .spectrum }
}
