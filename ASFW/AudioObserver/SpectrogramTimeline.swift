/// Audio-frame clock, independent of display cadence and panel pixel width.
nonisolated struct SpectrogramTimeline {
    static let columns = 1024
    static let maximumSlicesPerUpdate = 16
    private(set) var lastSlice: UInt64?
    static func hop(sampleRate: UInt32) -> UInt64 { max(1, UInt64((Double(sampleRate) * 512 / 48000).rounded())) }
    static func duration(sampleRate: UInt32) -> Double {
        Double(columns) * Double(hop(sampleRate: sampleRate)) / Double(max(1, sampleRate))
    }
    mutating func append(writeEnd: UInt64, oldest: UInt64, fftSize: UInt32, sampleRate: UInt32) -> ClosedRange<UInt64>? {
        let hop = Self.hop(sampleRate: sampleRate)
        let latest = writeEnd / hop
        // Reserve 40 ms before the oldest readable window for asynchronous GPU execution.
        let minimumEnd = oldest + UInt64(fftSize) + UInt64(sampleRate / 25)
        let firstValid = (minimumEnd + hop - 1) / hop
        let next = lastSlice.map { $0 + 1 } ?? latest
        let first = max(next, firstValid, latest > UInt64(Self.maximumSlicesPerUpdate - 1) ? latest - UInt64(Self.maximumSlicesPerUpdate - 1) : 0)
        guard first <= latest else { return nil }
        lastSlice = latest
        return first...latest
    }
}
