//
//  AudioGeometrySnapshot.swift
//  ASFW
//
//  Read-only snapshot and resolution logic for FireWire audio timing,
//  cadence, HAL buffers, and CoreAudio declarations.
//

import Foundation

struct AudioGeometrySnapshot: Equatable, Sendable {
    var endpointId: UInt64 = 0
    var sampleRateHz: UInt32 = 48000
    var inputChannels: UInt32 = 0
    var outputChannels: UInt32 = 0
    var isStreaming: Bool = false
    var isReady: Bool = false

    // Hardware margins
    var preparationLeadPackets: UInt32 = 0
    var hardwareFloorPackets: UInt32 = 0
    var currentCommittedMarginPackets: UInt32 = 0
    var intervalMinimum: UInt32?
    var lifetimeMinimum: UInt32?

    // Isochronous rings
    var txHardwareRingPackets: UInt32 = 504
    var txSharedSlotPackets: UInt32 = 504
    var rxHardwareRingPackets: UInt32 = 504
    var txContentFreezePackets: UInt32 = 2
    var txRepointGuardPackets: UInt32 = 2
    var txRingLapFrames: UInt32 = 3024

    // Cadence
    var isochCyclesPerSecond: UInt32 = 8000
    var microsecondsPerIsochCycle: UInt32 = 125
    var txPacketsPerGroup: UInt32 = 8
    var rxPacketsPerGroup: UInt32 = 8
    var txInterruptIntervalMicroseconds: UInt32 = 1000
    var rxInterruptIntervalMicroseconds: UInt32 = 1000
    var framesPerDataPacket: UInt32 = 8
    var cadenceBlockPackets: UInt32 = 4
    var cadenceBlockFrames: UInt32 = 24
    var framesPerCompletionGroupTx: UInt32 = 48
    var framesPerCompletionGroupRx: UInt32 = 48
    var minFramesPerRxInterrupt: UInt32 = 40
    var maxFramesPerRxInterrupt: UInt32 = 48

    // HAL buffers
    var frameRingFrames: UInt32 = 12288
    var clientIoBudgetFrames: UInt32 = 1024
    var zeroTimestampPeriodFrames: UInt32 = 12288
    var schedulingJitterFrames: UInt32 = 128
    var frameAlignmentFrames: UInt32 = 32
    var maxBlockingFramesPerDataPacket: UInt32 = 32
    var pcmPublicationCacheFrames: UInt32 = 12288

    // CoreAudio declarations
    var outputLatencyFrames: UInt32 = 52
    var outputSafetyOffsetFrames: UInt32 = 128
    var inputLatencyFrames: UInt32 = 53
    var inputSafetyOffsetFrames: UInt32 = 128
    var completionBatchFrames: UInt32 = 48
    var txSafetyOffsetPolicyFrames: UInt32 = 128
    var rxSafetyOffsetPolicyFrames: UInt32 = 128
    var reportedLatencyPolicyFrames: UInt32 = 52
    var txTransferDelayTicks: UInt32 = 12800
    var rxTransferDelayTicks: UInt32 = 12800

    init() {}

    /// Resolves the authoritative timing geometry from a live telemetry endpoint.
    static func resolve(from endpoint: AudioTelemetryEndpoint) -> AudioGeometrySnapshot {
        var s = resolve(
            sampleRateHz: endpoint.sampleRateHz,
            channelsIn: endpoint.inputChannels,
            channelsOut: endpoint.outputChannels,
            isStreaming: endpoint.isStreaming,
            isReady: (endpoint.flags & (1 << 0)) != 0
        )
        s.endpointId = endpoint.guid
        s.preparationLeadPackets = endpoint.preparationLeadPackets
        s.hardwareFloorPackets = endpoint.hardwareFloorPackets
        s.currentCommittedMarginPackets = endpoint.currentCommittedMarginPackets
        s.intervalMinimum = endpoint.intervalMinimum
        s.lifetimeMinimum = endpoint.lifetimeMinimum
        return s
    }

    /// Pure derivation of timing and HAL geometry for a given sample rate and stream state.
    static func resolve(
        sampleRateHz: UInt32,
        channelsIn: UInt32 = 0,
        channelsOut: UInt32 = 0,
        isStreaming: Bool = false,
        isReady: Bool = true
    ) -> AudioGeometrySnapshot {
        var s = AudioGeometrySnapshot()
        s.sampleRateHz = sampleRateHz
        s.inputChannels = channelsIn
        s.outputChannels = channelsOut
        s.isStreaming = isStreaming
        s.isReady = isReady

        // Common compile-time structure
        s.txHardwareRingPackets = 504
        s.txSharedSlotPackets = 504
        s.rxHardwareRingPackets = 504
        s.txContentFreezePackets = 2
        s.txRepointGuardPackets = 2
        s.isochCyclesPerSecond = 8000
        s.microsecondsPerIsochCycle = 125
        s.txPacketsPerGroup = 8
        s.rxPacketsPerGroup = 8
        s.txInterruptIntervalMicroseconds = 1000
        s.rxInterruptIntervalMicroseconds = 1000
        s.cadenceBlockPackets = 4
        s.clientIoBudgetFrames = 1024
        s.schedulingJitterFrames = 128
        s.frameAlignmentFrames = 32
        s.maxBlockingFramesPerDataPacket = 32

        guard sampleRateHz > 0 else {
            s.framesPerDataPacket = 0
            s.zeroTimestampPeriodFrames = 0
            s.frameRingFrames = 0
            s.pcmPublicationCacheFrames = 0
            s.txRingLapFrames = 0
            s.txTransferDelayTicks = 0
            s.rxTransferDelayTicks = 0
            return s
        }

        // Hal rate tier
        let tier: UInt32
        switch sampleRateHz {
        case 32000, 44100, 48000:
            tier = 1
        case 88200, 96000:
            tier = 2
        case 176400, 192000:
            tier = 4
        default:
            tier = 1
        }

        let zts = 12288 * tier
        s.zeroTimestampPeriodFrames = zts
        s.frameRingFrames = zts
        s.pcmPublicationCacheFrames = zts

        // Frames per data packet (blocking SYT interval)
        let fpp: UInt32 = 8 * tier
        s.framesPerDataPacket = fpp

        // Group & ring frame scaling
        s.framesPerCompletionGroupTx = UInt32(UInt64(s.txPacketsPerGroup) * UInt64(sampleRateHz) / 8000)
        s.framesPerCompletionGroupRx = UInt32(UInt64(s.rxPacketsPerGroup) * UInt64(sampleRateHz) / 8000)
        s.cadenceBlockFrames = UInt32(UInt64(s.cadenceBlockPackets) * UInt64(sampleRateHz) / 8000)
        s.txRingLapFrames = UInt32(UInt64(s.txHardwareRingPackets) * UInt64(sampleRateHz) / 8000)

        // Min/max frames per RX interrupt
        if sampleRateHz == 44100 {
            s.minFramesPerRxInterrupt = 40
            s.maxFramesPerRxInterrupt = 48
        } else {
            // Nominal worst-case DATA packet bounds in an 8-packet group:
            // minimum 5 DATA packets, maximum 6 DATA packets.
            s.minFramesPerRxInterrupt = 5 * fpp
            s.maxFramesPerRxInterrupt = 6 * fpp
        }

        // Transfer delays (ticks at 24.576 MHz): Linux / AMDTP blocking formula
        switch sampleRateHz {
        case 32000:
            s.txTransferDelayTicks = 14848
            s.rxTransferDelayTicks = 14848
        case 44100, 88200, 176400:
            s.txTransferDelayTicks = 13162
            s.rxTransferDelayTicks = 13162
        default:
            s.txTransferDelayTicks = 12800
            s.rxTransferDelayTicks = 12800
        }

        // Declarations
        s.completionBatchFrames = 6 * fpp // 48 at 48k, 96 at 96k
        s.outputLatencyFrames = 52
        s.inputLatencyFrames = 53
        s.outputSafetyOffsetFrames = 128
        s.inputSafetyOffsetFrames = 128
        s.txSafetyOffsetPolicyFrames = 128
        s.rxSafetyOffsetPolicyFrames = 128
        s.reportedLatencyPolicyFrames = 52

        return s
    }

    // MARK: - Presentation helpers

    func outputFrames(io: UInt32) -> UInt64 {
        UInt64(io) + UInt64(outputLatencyFrames) + UInt64(outputSafetyOffsetFrames)
    }

    func roundTripFrames(io: UInt32) -> UInt64 {
        outputFrames(io: io) + UInt64(io) + UInt64(inputLatencyFrames) + UInt64(inputSafetyOffsetFrames)
    }

    func milliseconds(packets: UInt64) -> Double? {
        guard microsecondsPerIsochCycle != 0 else { return nil }
        return Double(packets) * Double(microsecondsPerIsochCycle) / 1000.0
    }

    func milliseconds(frames: UInt64) -> Double? {
        guard sampleRateHz != 0 else { return nil }
        return Double(frames) * 1000.0 / Double(sampleRateHz)
    }

    func framesPerCycle() -> Double? {
        guard isochCyclesPerSecond != 0, sampleRateHz != 0 else { return nil }
        return Double(sampleRateHz) / Double(isochCyclesPerSecond)
    }

    static func interruptsPerSecond(intervalMicroseconds: UInt32) -> Double? {
        guard intervalMicroseconds != 0 else { return nil }
        return 1_000_000.0 / Double(intervalMicroseconds)
    }
}
