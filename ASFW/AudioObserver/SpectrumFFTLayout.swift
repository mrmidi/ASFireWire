/// Shared CPU-side FFT geometry. Real packing needs only N/2 complex values.
nonisolated enum SpectrumFFTLayout {
    static let sizes: [UInt32] = [1024, 2048, 4096, 8192]
    static func binCount(_ size: UInt32) -> Int { Int(size / 2 + 1) }
    static func scratchBytes(_ size: UInt32) -> Int { Int(size / 2) * 8 }
}
