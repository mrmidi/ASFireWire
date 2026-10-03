import Foundation

// Audio stream metrics capture: bounded, application-owned observation only.
// It polls existing read-only UserClient surfaces; it never starts, stops, or
// reconfigures an audio stream, nor does it send FireWire transactions.

// Two hours at 2 Hz covers an endurance run without making the
// collector unbounded. The samples stay application-owned and read-only.
private let kAudioMetricsCaptureIntervalHz = 2
private let kAudioMetricsCaptureMaximumDurationSeconds = 2 * 60 * 60

struct ASFWAudioStreamMetricsCaptureDelta: Equatable, Sendable {
    let packetsTX: UInt64
    let packetsRX: UInt64
    let discontinuities: UInt64
    let txReplayUnderflows: UInt64
    let txRingOverruns: UInt64
    let rxReplayEpochResets: UInt64
    let rxRingOverruns: UInt64

    static func between(
        previous: ASFWAudioStreamMetricsSnapshot,
        current: ASFWAudioStreamMetricsSnapshot
    ) -> Self? {
        guard previous.status == 0, current.status == 0,
              previous.endpointGeneration == current.endpointGeneration,
              previous.streamGeneration == current.streamGeneration,
              current.tx.packets >= previous.tx.packets,
              current.rx.packets >= previous.rx.packets,
              current.discontinuities >= previous.discontinuities,
              current.tx.replayUnderflows >= previous.tx.replayUnderflows,
              current.tx.ringOverruns >= previous.tx.ringOverruns,
              current.rx.replayEpochResets >= previous.rx.replayEpochResets,
              current.rx.ringOverruns >= previous.rx.ringOverruns else {
            return nil
        }
        return Self(
            packetsTX: current.tx.packets - previous.tx.packets,
            packetsRX: current.rx.packets - previous.rx.packets,
            discontinuities: current.discontinuities - previous.discontinuities,
            txReplayUnderflows: current.tx.replayUnderflows - previous.tx.replayUnderflows,
            txRingOverruns: current.tx.ringOverruns - previous.tx.ringOverruns,
            rxReplayEpochResets: current.rx.replayEpochResets - previous.rx.replayEpochResets,
            rxRingOverruns: current.rx.ringOverruns - previous.rx.ringOverruns
        )
    }

    var mcpValue: ASFWMCPValue {
        .object([
            "packetsTX": .uint64(packetsTX),
            "packetsRX": .uint64(packetsRX),
            "discontinuities": .uint64(discontinuities),
            "txReplayUnderflows": .uint64(txReplayUnderflows),
            "txRingOverruns": .uint64(txRingOverruns),
            "rxReplayEpochResets": .uint64(rxReplayEpochResets),
            "rxRingOverruns": .uint64(rxRingOverruns),
        ])
    }
}

@MainActor
final class ASFWMCPAudioMetricsCaptureSession<Driver: ASFWDriverControlling> {
    private let intervalNs: UInt64 = 500_000_000
    private let capacity = kAudioMetricsCaptureIntervalHz * kAudioMetricsCaptureMaximumDurationSeconds

    private struct Sample: Sendable {
        let snapshot: ASFWAudioStreamMetricsSnapshot
        let logLatestSequence: UInt64
        let logDroppedRecords: UInt64
        let delta: ASFWAudioStreamMetricsCaptureDelta?

        var mcpValue: ASFWMCPValue {
            .object([
                "snapshot": snapshot.mcpValue,
                "logLatestSequence": .uint64(logLatestSequence),
                "logDroppedRecords": .uint64(logDroppedRecords),
                "delta": delta.map(\.mcpValue) ?? .null,
            ])
        }
    }

    private struct Marker: Sendable {
        let timestampNs: UInt64
        let sampleIndex: Int
        let logLatestSequence: UInt64
        let logDroppedRecords: UInt64

        var mcpValue: ASFWMCPValue {
            .object([
                "kind": .string("manualMisframe"),
                "timestampNs": .uint64(timestampNs),
                "sampleIndex": .int(sampleIndex),
                "logLatestSequence": .uint64(logLatestSequence),
                "logDroppedRecords": .uint64(logDroppedRecords),
            ])
        }
    }

    private let driver: Driver
    private var guid: UInt64?
    private var samples: [Sample] = []
    private var markers: [Marker] = []
    private var missedPolls = 0
    private var active = false
    private var samplingTask: Task<Void, Never>?

    init(driver: Driver) {
        self.driver = driver
    }

    deinit {
        samplingTask?.cancel()
    }

    func start(guid: UInt64) async -> ASFWMCPValue? {
        guard !active else { return nil }
        guard let initial = await collectSample(guid: guid, previous: nil) else {
            return nil
        }

        self.guid = guid
        samples = [initial]
        markers = []
        missedPolls = 0
        active = true
        samplingTask = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                try? await Task.sleep(nanoseconds: self.intervalNs)
                guard !Task.isCancelled else { return }
                await self.appendPeriodicSample(for: guid)
            }
        }
        return reportValue()
    }

    func markMisframe() async -> ASFWMCPValue? {
        guard active, let guid else { return nil }
        guard let sample = await collectSample(guid: guid, previous: samples.last?.snapshot) else {
            missedPolls += 1
            return nil
        }
        append(sample)
        markers.append(Marker(
            timestampNs: sample.snapshot.timestampNs,
            sampleIndex: samples.count - 1,
            logLatestSequence: sample.logLatestSequence,
            logDroppedRecords: sample.logDroppedRecords
        ))
        return reportValue()
    }

    func stop() -> ASFWMCPValue? {
        guard guid != nil else { return nil }
        active = false
        samplingTask?.cancel()
        samplingTask = nil
        return reportValue()
    }

    func status() -> ASFWMCPValue? {
        guard guid != nil else { return nil }
        return reportValue(includeSamples: false)
    }

    func export() -> ASFWMCPValue? {
        guard guid != nil else { return nil }
        return reportValue()
    }

    private func appendPeriodicSample(for guid: UInt64) async {
        guard active, self.guid == guid else { return }
        guard let sample = await collectSample(guid: guid, previous: samples.last?.snapshot) else {
            missedPolls += 1
            return
        }
        append(sample)
    }

    private func collectSample(
        guid: UInt64,
        previous: ASFWAudioStreamMetricsSnapshot?
    ) async -> Sample? {
        guard let snapshot = await driver.audioStreamMetricsSnapshot(guid: guid),
              let logStats = await driver.logRingStats() else {
            return nil
        }
        return Sample(
            snapshot: snapshot,
            logLatestSequence: logStats.latestSequence,
            logDroppedRecords: logStats.droppedRecords,
            delta: previous.flatMap {
                ASFWAudioStreamMetricsCaptureDelta.between(previous: $0, current: snapshot)
            }
        )
    }

    private func append(_ sample: Sample) {
        samples.append(sample)
        if samples.count > capacity {
            let removed = samples.count - capacity
            samples.removeFirst(removed)
            markers = markers.compactMap { marker in
                let shifted = marker.sampleIndex - removed
                guard shifted >= 0 else { return nil }
                return Marker(
                    timestampNs: marker.timestampNs,
                    sampleIndex: shifted,
                    logLatestSequence: marker.logLatestSequence,
                    logDroppedRecords: marker.logDroppedRecords
                )
            }
        }
    }

    private func reportValue(includeSamples: Bool = true) -> ASFWMCPValue {
        .object([
            "guid": .string(String(format: "0x%016llX", guid ?? 0)),
            "active": .bool(active),
            "intervalHz": .int(kAudioMetricsCaptureIntervalHz),
            "capacity": .int(capacity),
            "sampleCount": .int(samples.count),
            "missedPolls": .int(missedPolls),
            "markers": .array(markers.map(\.mcpValue)),
            "samples": includeSamples ? .array(samples.map(\.mcpValue)) : .null,
        ])
    }
}
