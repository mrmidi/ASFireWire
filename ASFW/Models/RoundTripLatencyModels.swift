//
//  RoundTripLatencyModels.swift
//  ASFW
//
//  Plain value types for the Round-Trip Latency tool. Every number here was
//  produced by the platform-neutral C core (tools/rtl/rtl_core.c, tone_core.c);
//  Swift only carries and formats it. Names and meanings follow
//  tools/rtl/README.md: RTL_raw, RTL_ts, scheduling distance, residual,
//  declared round trip. None of these types mentions a C struct, so they can be
//  built directly in tests.
//

import Foundation

// MARK: - Declared geometry

/// What the device declares to CoreAudio, in frames.
nonisolated struct RtlDeclared: Equatable, Sendable {
    /// Nominal sample rate; nil when CoreAudio would not report one. There is no
    /// silent fallback rate: without it frames cannot be shown as milliseconds.
    var sampleRate: Double?
    var bufferFrames: UInt32
    var safetyIn: UInt32
    var safetyOut: UInt32
    var deviceLatencyIn: UInt32
    var deviceLatencyOut: UInt32
    var streamLatencyIn: UInt32
    var streamLatencyOut: UInt32
    /// Properties CoreAudio would not return. A failed read leaves a zero behind,
    /// which is a legal value, so absence is listed explicitly.
    var missing: [String]
    /// 2 x buffer + both safety offsets (rtl_declared_sched).
    var schedulingFrames: UInt32
    /// Device plus stream latency, both directions (rtl_declared_hw).
    var hardwareFrames: UInt32
    /// Scheduling plus hardware; nil when any declaration is missing
    /// (rtl_declared_round_trip never returns a partial sum).
    var roundTripFrames: UInt32?
    /// The capture window `auto` selects: 4x the declared round trip, at least 1024.
    var autoWindowFrames: UInt32
}

// MARK: - Statistics and trials

nonisolated struct RtlStat: Equatable, Sendable {
    var count: Int
    var median: Double
    var mean: Double
    var standardDeviation: Double
    var minimum: Double
    var maximum: Double
}

nonisolated struct RtlTrialRow: Identifiable, Equatable, Sendable {
    var index: Int
    var verdictID: String
    var verdictName: String
    var isAccepted: Bool
    /// nil when no impulse was found in the window.
    var rawFrames: Double?
    /// nil when no impulse was found or a timestamp was missing.
    var tsFrames: Double?
    var onsetFrames: Double?
    var peak: Double
    var noise: Double
    var isInverted: Bool
    var atWindowEdge: Bool
    var callbacks: UInt32
    var overloads: UInt32

    var id: Int { index }
}

nonisolated struct RtlRejection: Identifiable, Equatable, Sendable {
    var id: String
    var name: String
    var count: Int
}

/// The IO audit of one run (what the HAL did while the trials ran).
nonisolated struct RtlIOAudit: Equatable, Sendable {
    var callbacks: UInt32
    /// Distinct callback sizes seen. More than one means any reasoning that
    /// assumed a fixed span is wrong.
    var distinctSpans: [UInt32]
    var processorOverloads: UInt32
    var gapEvents: UInt32
    var gapFrames: Double
    var anchorEvents: UInt32
    var worstAnchorFrames: Double
    var clockDisagreements: UInt32
    var missingTimestamps: UInt32
    /// Peak |sample| per input channel; shows where signal actually landed.
    var inputChannelPeaks: [Float]

    var hasAnomaly: Bool {
        gapEvents > 0 || anchorEvents > 0 || clockDisagreements > 0
            || missingTimestamps > 0 || processorOverloads > 0
    }
}

nonisolated enum RtlOutcome: String, Equatable, Sendable {
    case completed, timedOut, cancelled
}

// MARK: - Measurement result

nonisolated struct RtlMeasurementResult: Equatable, Sendable {
    var deviceName: String
    var deviceUID: String
    var sampleRate: Double
    /// nil when the buffer size was not requested.
    var bufferRequested: UInt32?
    var bufferActual: UInt32
    var window: UInt32
    var trialsRequested: UInt32
    var outcome: RtlOutcome
    var declared: RtlDeclared
    var io: RtlIOAudit

    var trialsRun: UInt32
    var accepted: Int
    /// Rejected trials per verdict; verdicts with no rejections are omitted.
    var rejections: [RtlRejection]
    /// Statistics over accepted trials only; nil when no trial was accepted.
    var raw: RtlStat?
    var onset: RtlStat?
    var ts: RtlStat?
    var scheduling: RtlStat?
    var declaredSchedulingFrames: UInt32
    var declaredHardwareFrames: UInt32
    /// median(RTL_ts) - declared hardware latency; nil when a declaration is
    /// missing or nothing was accepted.
    var residualFrames: Double?
    var minPeak: Double
    var worstNoise: Double
    var invertedTrials: Int
    var trials: [RtlTrialRow]
    /// The asfw.rtl_loopback.v1 record, ready to save.
    var json: Data?

    var acceptanceRate: Double? {
        RtlFormat.acceptanceRate(accepted: accepted, run: Int(trialsRun))
    }
    var signalToNoiseRatio: Double? {
        RtlFormat.signalToNoiseRatio(peak: minPeak, noise: worstNoise)
    }
    var isWeakSignal: Bool {
        RtlFormat.isWeakSignal(peak: minPeak, noise: worstNoise)
    }
    /// A bufferRequested the device did not adopt.
    var bufferWasCoerced: Bool {
        if let requested = bufferRequested { return requested != bufferActual }
        return false
    }
}

// MARK: - Tone result

nonisolated struct RtlToneEventRow: Identifiable, Equatable, Sendable {
    var index: Int
    var kind: String
    /// HAL input sample time of the event's first frame; nil without a timestamp.
    var inputSample: Double?
    var seconds: Double
    var frames: UInt64
    var silentSamples: UInt32
    /// Only for slips: + frames lost, - frames repeated (modulo one tone period).
    var slipFrames: Double?
    /// An overload, sample-time gap or missing timestamp near the event; empty
    /// when the IO was clean. A glitch next to one of these may be CoreAudio's.
    var ioNote: String

    var id: Int { index }
}

nonisolated struct RtlToneResult: Equatable, Sendable {
    var deviceName: String
    var sampleRate: Double
    /// nil when the buffer size was not requested.
    var bufferRequested: UInt32?
    /// The client buffer size the run used (0 when it could not be read).
    var bufferActual: UInt32
    var frequency: Double
    var duration: Double
    var outcome: RtlOutcome
    var callbacks: UInt32
    var capturedFrames: UInt64
    var processorOverloads: UInt32
    var sampleTimeGaps: UInt32
    var missingTimestamps: UInt32
    /// False when the capture could not be judged; `invalidReason` says why.
    var isValid: Bool
    var invalidReason: String?
    var toneAmplitude: Double
    var noiseRms: Double
    var analysedSeconds: Double
    var onsetSample: UInt64
    var dropouts: UInt32
    var slips: UInt32
    var clicks: UInt32
    var eventCount: UInt32
    var events: [RtlToneEventRow]

    var eventsPerMinute: Double? {
        analysedSeconds > 0 ? Double(eventCount) * 60.0 / analysedSeconds : nil
    }
    var averageFramesPerCallback: Double? {
        callbacks > 0 ? Double(capturedFrames) / Double(callbacks) : nil
    }
    /// A bufferRequested the device did not adopt.
    var bufferWasCoerced: Bool {
        if let requested = bufferRequested { return requested != bufferActual }
        return false
    }
}

// MARK: - Formatting

/// Unit conversion and wording shared by the view and the tests. Frames are the
/// measurement; milliseconds are only a view of it at the active sample rate.
nonisolated enum RtlFormat {
    /// A signal-to-noise ratio below this (30x, about 29.5 dB) is flagged weak.
    static let weakSignalRatio = 30.0

    static func milliseconds(frames: Double, sampleRate: Double?) -> Double? {
        guard let sampleRate, sampleRate > 0, frames.isFinite else { return nil }
        return frames * 1000.0 / sampleRate
    }

    /// "1233 fr" with no milliseconds when the rate is unknown.
    static func frames(_ value: Double, decimals: Int = 2) -> String {
        String(format: "%.\(decimals)f fr", value)
    }

    static func wholeFrames(_ value: UInt32) -> String { "\(value) fr" }

    static func millisecondsText(frames: Double, sampleRate: Double?,
                                 decimals: Int = 3, signed: Bool = false) -> String {
        guard let ms = milliseconds(frames: frames, sampleRate: sampleRate) else { return "rate unknown" }
        return String(format: signed ? "%+.\(decimals)f ms" : "%.\(decimals)f ms", ms)
    }

    /// "25.688 ms" next to the frame count, e.g. "1233 fr  25.688 ms".
    static func framesAndMilliseconds(_ value: Double, sampleRate: Double?,
                                      decimals: Int = 2, signed: Bool = false) -> String {
        let frameText = signed ? String(format: "%+.\(decimals)f fr", value) : frames(value, decimals: decimals)
        return "\(frameText)  \(millisecondsText(frames: value, sampleRate: sampleRate, signed: signed))"
    }

    static func acceptanceRate(accepted: Int, run: Int) -> Double? {
        run > 0 ? Double(accepted) / Double(run) : nil
    }

    /// "3 of 20 accepted (15%)". The rate is part of the measurement: 3 of 20
    /// is a different claim from 20 of 20.
    static func acceptanceText(accepted: Int, run: Int) -> String {
        guard let rate = acceptanceRate(accepted: accepted, run: run) else { return "no trials run" }
        return "\(accepted) of \(run) accepted (\(Int((rate * 100).rounded()))%)"
    }

    /// peak / noise; nil when no noise was measured.
    static func signalToNoiseRatio(peak: Double, noise: Double) -> Double? {
        noise > 0 ? peak / noise : nil
    }

    static func signalToNoiseDecibels(peak: Double, noise: Double) -> Double? {
        signalToNoiseRatio(peak: peak, noise: noise).map { 20.0 * log10($0) }
    }

    static func isWeakSignal(peak: Double, noise: Double) -> Bool {
        guard let ratio = signalToNoiseRatio(peak: peak, noise: noise) else { return false }
        return ratio < weakSignalRatio
    }

    /// The longest round trip a capture window can hold: the last
    /// RTL_EDGE_GUARD_FRAMES frames are reserved for the window-edge check.
    static func maximumMeasurableFrames(window: UInt32, edgeGuard: UInt32) -> UInt32 {
        window > edgeGuard ? window - edgeGuard : 0
    }

    /// Wording for a run time in seconds (from rtl_impulse_expected_seconds).
    static func durationText(seconds: Double) -> String {
        if seconds < 1 { return "under a second" }
        if seconds < 90 { return String(format: "about %.0f s", seconds.rounded()) }
        return String(format: "about %.1f min", seconds / 60.0)
    }

    /// File name for an exported evidence record, e.g. "rtl-Saffire-48000-20261016-101500.json".
    static func exportFileName(deviceName: String, sampleRate: Double, date: Date) -> String {
        let allowed = CharacterSet.alphanumerics
        var slug = ""
        var lastWasSeparator = true
        for scalar in deviceName.unicodeScalars {
            if allowed.contains(scalar) {
                slug.unicodeScalars.append(scalar)
                lastWasSeparator = false
            } else if !lastWasSeparator {
                slug.append("-")
                lastWasSeparator = true
            }
        }
        while slug.hasSuffix("-") { slug.removeLast() }
        if slug.isEmpty { slug = "device" }
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(identifier: "UTC")
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        return "rtl-\(slug)-\(Int(sampleRate.rounded()))-\(formatter.string(from: date)).json"
    }
}

// MARK: - Microphone permission

/// Microphone (audio capture) authorization, independent of AVFoundation so the
/// wording is testable. The loopback needs CoreAudio *input*; without
/// permission the HAL delivers silence, which must never be reported as
/// "no signal".
nonisolated enum MicrophoneAccess: Equatable, Sendable {
    case authorized
    case notDetermined
    case denied
    case restricted

    var allowsCapture: Bool { self == .authorized }

    /// Why a measurement cannot start; nil when it can (or when asking will tell).
    var blockingExplanation: String? {
        switch self {
        case .authorized, .notDetermined:
            return nil
        case .denied:
            return "Microphone access is denied for ASFW. macOS delivers silence to the input, so a measurement would read \"no signal\" for the wrong reason. Allow ASFW under System Settings > Privacy & Security > Microphone, then try again."
        case .restricted:
            return "Microphone access is restricted on this Mac (for example by a device-management profile), so ASFW cannot record the looped-back signal."
        }
    }
}

/// Where the declared-latency tab, the measurement and the tone test stand with
/// respect to the sidebar. Kept apart from the view so the rule is testable.
nonisolated enum RtlAvailability {
    /// Whether a CoreAudio device is one ASFW publishes. The driver names every
    /// device it publishes "ASFW-<GUID in hex>"; the sidebar's audio sections,
    /// including this one, appear only while at least one such device is listed.
    static func isASFWDevice(uid: String) -> Bool {
        ASFWAudioObserverClient.guid(fromDeviceUID: uid) != nil
    }

    /// One duplex device is enough to run; a device with only one direction can
    /// still show its declarations.
    static func canMeasure(inputChannels: UInt32, outputChannels: UInt32) -> Bool {
        inputChannels > 0 && outputChannels > 0
    }

    /// Selected channel indices must exist on the device.
    static func channelsAreValid(out: Int, in input: Int,
                                 outputChannels: UInt32, inputChannels: UInt32) -> Bool {
        out >= 0 && input >= 0 && out < Int(outputChannels) && input < Int(inputChannels)
    }
}
