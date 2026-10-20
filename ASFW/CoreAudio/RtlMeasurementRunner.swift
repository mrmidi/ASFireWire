//
//  RtlMeasurementRunner.swift
//  ASFW
//
//  Swift side of tools/rtl/rtl_session.{h,c}. The real-time part (the IOProc)
//  lives entirely in C and is the same code the rtl_loopback command line runs.
//  This file does the non-real-time work around it: reads the device, creates
//  the session (which preallocates everything the IOProc touches), blocks a
//  background thread in rtl_session_run(), then turns the engine into value
//  types. Nothing here runs on the audio thread.
//

import CoreAudio
import Foundation
import os

// MARK: - Requests and errors

nonisolated struct RtlMeasureRequest: Sendable {
    var deviceID: AudioObjectID
    /// nil leaves the device's buffer size alone.
    var requestedBufferFrames: UInt32?
    var trials: UInt32
    /// nil selects the declared-round-trip based `auto` window.
    var window: UInt32?
    var outChannel: UInt32
    var inChannel: UInt32
    var amplitude: Float
    /// "ASFW 0.5.0 (24)": written to the evidence record.
    var appVersion: String
    /// The options as the user set them, written to the evidence record.
    var optionsDescription: String
}

nonisolated struct RtlToneRequest: Sendable {
    var deviceID: AudioObjectID
    var requestedBufferFrames: UInt32?
    var durationSeconds: Double
    var frequencyHz: Double
    var amplitude: Float
    var outChannel: UInt32
    var inChannel: UInt32
    /// Events kept for display.
    var maxListedEvents: Int
}

nonisolated enum RtlRunnerError: Error, Equatable, Sendable, LocalizedError {
    case sampleRateUnavailable
    case channelOutOfRange(direction: String, requested: UInt32, available: UInt32)
    case invalidConfiguration
    case cannotAllocate
    case cannotCreateIOProc
    case cannotStart
    case cancelled

    var errorDescription: String? {
        switch self {
        case .sampleRateUnavailable:
            return "CoreAudio would not report the device's nominal sample rate, so frames cannot be converted to time."
        case let .channelOutOfRange(direction, requested, available):
            return "The \(direction) channel \(requested + 1) does not exist: the device has \(available)."
        case .invalidConfiguration:
            return "The measurement settings are outside the supported range."
        case .cannotAllocate:
            return "Not enough memory for the capture."
        case .cannotCreateIOProc:
            return "CoreAudio refused to create an IO procedure on the device."
        case .cannotStart:
            return "CoreAudio could not start the device. Another client may be holding it, or microphone access may be missing."
        case .cancelled:
            return "Cancelled."
        }
    }
}

// MARK: - Cancellation and progress

/// Shared between the UI (cancel, progress) and the background thread that runs
/// the session. Holds the session pointer only between attach and detach, under
/// a lock, so cancel() can never touch a destroyed session. The lock is taken on
/// non-real-time threads only; the IOProc never sees it.
nonisolated final class RtlRunControl: @unchecked Sendable {
    private struct State {
        var cancelled = false
        var session: OpaquePointer?
    }
    private let state = OSAllocatedUnfairLock(initialState: State())

    /// Any thread. Idempotent. A cancel that arrives before the session exists is
    /// remembered and honoured by attach().
    func cancel() {
        state.withLock { state in
            state.cancelled = true
            if let session = state.session { rtl_session_cancel(session) }
        }
    }

    var isCancelled: Bool { state.withLock { $0.cancelled } }

    /// Registers the session about to run. Returns false when cancel() already
    /// happened, in which case the caller must not run it.
    fileprivate func attach(_ session: OpaquePointer) -> Bool {
        // Unchecked: the session pointer is not Sendable, but it only ever crosses
        // threads under this lock, and only rtl_session_cancel/progress (atomic
        // by contract) are called through it from other threads.
        state.withLockUnchecked { state in
            state.session = session
            if state.cancelled { rtl_session_cancel(session) }
            return !state.cancelled
        }
    }

    fileprivate func detach() {
        state.withLock { $0.session = nil }
    }

    /// Fraction of the work done, 0...1; 0 before the session runs.
    func fraction() -> Double {
        state.withLock { state in
            guard let session = state.session else { return 0 }
            var done: UInt64 = 0
            var total: UInt64 = 0
            rtl_session_progress(session, &done, &total)
            return total > 0 ? min(1, Double(done) / Double(total)) : 0
        }
    }
}

// MARK: - Runner

nonisolated enum RtlMeasurementRunner {

    // MARK: Declared geometry (no IO)

    /// Reads what the device declares. Property reads only; starts no IO.
    static func readDeclared(deviceID: AudioObjectID) -> RtlDeclared {
        var sampleRate = 0.0
        let rate: Double? = rtl_dev_sample_rate(deviceID, &sampleRate) != 0 ? sampleRate : nil
        var c = rtl_declared_t()
        rtl_dev_read_declared(deviceID, &c)
        return makeDeclared(&c, sampleRate: rate)
    }

    private static func makeDeclared(_ c: inout rtl_declared_t, sampleRate: Double?) -> RtlDeclared {
        var valid: Int32 = 0
        let roundTrip = rtl_declared_round_trip(&c, &valid)
        let names = tupleElements(c.missing, count: min(Int(c.nmissing), Int(RTL_DECLARED_MAX_MISSING)),
                                  as: UnsafePointer<CChar>?.self)
            .compactMap { $0.map { String(cString: $0) } }
        return RtlDeclared(
            sampleRate: sampleRate,
            bufferFrames: c.io,
            safetyIn: c.safIn, safetyOut: c.safOut,
            deviceLatencyIn: c.latIn, deviceLatencyOut: c.latOut,
            streamLatencyIn: c.strIn, streamLatencyOut: c.strOut,
            missing: names,
            schedulingFrames: rtl_declared_sched(&c),
            hardwareFrames: rtl_declared_hw(&c),
            roundTripFrames: valid != 0 ? roundTrip : nil,
            autoWindowFrames: rtl_auto_window(&c))
    }

    static func channelCount(deviceID: AudioObjectID, input: Bool) -> UInt32 {
        rtl_dev_channels(deviceID, input ? 1 : 0)
    }

    /// Nonzero when some client is already running the device. That client, not
    /// this tool, may then set the device's IO cycle.
    static func isRunningElsewhere(deviceID: AudioObjectID) -> Bool {
        rtl_dev_running_somewhere(deviceID) != 0
    }

    // MARK: Self-test (no hardware)

    static func selfTest() -> (failures: Int, log: String) {
        var failures = 0
        let log = withMemoryStream { stream in
            failures = Int(rtl_selftest_all(stream))
        }
        return (failures, String(decoding: log ?? Data(), as: UTF8.self))
    }

    // MARK: Impulse measurement

    /// Blocks until the measurement finishes, is cancelled or times out. Call from
    /// a background thread. The session is created, run and destroyed here; the
    /// IOProc runs on CoreAudio's thread inside rtl_session_run().
    static func measure(_ request: RtlMeasureRequest,
                        control: RtlRunControl) -> Result<RtlMeasurementResult, RtlRunnerError> {
        let dev = request.deviceID
        if control.isCancelled { return .failure(.cancelled) }

        // Buffer size sets the scheduling distance the measurement is reconciled
        // against, so a request that did not take is reported, never hidden.
        let buffer = BufferRequest(device: dev, frames: request.requestedBufferFrames)
        defer { buffer.restore() }
        var bufferActual = buffer.actual

        // No silent 48 kHz fallback: every frame-to-time conversion depends on it.
        var sampleRate = 0.0
        guard rtl_dev_sample_rate(dev, &sampleRate) != 0 else { return .failure(.sampleRateUnavailable) }

        let outChannels = rtl_dev_channels(dev, 0)
        let inChannels = rtl_dev_channels(dev, 1)
        if request.outChannel >= outChannels {
            return .failure(.channelOutOfRange(direction: "output", requested: request.outChannel,
                                               available: outChannels))
        }
        if request.inChannel >= inChannels {
            return .failure(.channelOutOfRange(direction: "input", requested: request.inChannel,
                                               available: inChannels))
        }

        var declared = rtl_declared_t()
        rtl_dev_read_declared(dev, &declared)
        if bufferActual == 0 { bufferActual = declared.io }
        let window = min(request.window ?? rtl_auto_window(&declared), UInt32(RTL_MAX_WINDOW))
        let trials = min(request.trials, UInt32(RTL_MAX_TRIALS))

        var config = rtl_impulse_config_t(
            device: dev, sampleRate: sampleRate,
            outChannel: request.outChannel, inChannel: request.inChannel,
            trials: trials, window: window, amplitude: request.amplitude)
        var status = RTL_SESSION_OK
        guard let session = rtl_session_create_impulse(&config, &status) else {
            return .failure(map(status))
        }
        defer { rtl_session_destroy(session) }

        guard control.attach(session) else { return .failure(.cancelled) }
        let runStatus = rtl_session_run(session)
        control.detach()
        guard runStatus == RTL_SESSION_OK else { return .failure(map(runStatus)) }
        // rtl_session_run has returned: AudioDeviceStop quiesced the IOProc, so
        // the engine below is this thread's.

        let outcome = map(rtl_session_outcome(session))
        if outcome == .cancelled { return .failure(.cancelled) }
        guard let engine = rtl_session_engine(session) else { return .failure(.invalidConfiguration) }

        var reports = [rtl_trial_report_t](repeating: rtl_trial_report_t(), count: Int(RTL_MAX_TRIALS))
        var summary = rtl_summary_t()
        reports.withUnsafeMutableBufferPointer { buffer in
            rtl_summarize(engine, &declared, buffer.baseAddress, &summary)
        }

        let name = deviceString(dev, rtl_dev_name)
        let uid = deviceString(dev, rtl_dev_uid)
        let overloads = rtl_session_overloads(session)
        let declaredModel = makeDeclared(&declared, sampleRate: sampleRate)
        let io = makeAudit(session: session, engine: engine, overloads: overloads)

        let json = makeJSON(
            request: request, name: name, uid: uid, sampleRate: sampleRate,
            bufferActual: bufferActual, window: window, trials: trials,
            overloads: overloads, timedOut: outcome == .timedOut,
            declared: &declared, engine: engine, reports: reports, summary: &summary)

        return .success(makeResult(
            request: request, name: name, uid: uid, sampleRate: sampleRate,
            bufferActual: bufferActual, window: window, trials: trials, outcome: outcome,
            declared: declaredModel, io: io, reports: reports, summary: summary, json: json))
    }

    // MARK: Tone

    static func tone(_ request: RtlToneRequest,
                     control: RtlRunControl) -> Result<RtlToneResult, RtlRunnerError> {
        let dev = request.deviceID
        if control.isCancelled { return .failure(.cancelled) }

        let buffer = BufferRequest(device: dev, frames: request.requestedBufferFrames)
        defer { buffer.restore() }
        var sampleRate = 0.0
        guard rtl_dev_sample_rate(dev, &sampleRate) != 0 else { return .failure(.sampleRateUnavailable) }

        let outChannels = rtl_dev_channels(dev, 0)
        let inChannels = rtl_dev_channels(dev, 1)
        if request.outChannel >= outChannels {
            return .failure(.channelOutOfRange(direction: "output", requested: request.outChannel,
                                               available: outChannels))
        }
        if request.inChannel >= inChannels {
            return .failure(.channelOutOfRange(direction: "input", requested: request.inChannel,
                                               available: inChannels))
        }

        var config = rtl_tone_config_t(
            device: dev, sampleRate: sampleRate,
            outChannel: request.outChannel, inChannel: request.inChannel,
            durationSec: request.durationSeconds, freqHz: request.frequencyHz,
            amplitude: request.amplitude)
        var status = RTL_SESSION_OK
        guard let session = rtl_session_create_tone(&config, &status) else {
            return .failure(map(status))
        }
        defer { rtl_session_destroy(session) }

        guard control.attach(session) else { return .failure(.cancelled) }
        let runStatus = rtl_session_run(session)
        control.detach()
        guard runStatus == RTL_SESSION_OK else { return .failure(map(runStatus)) }

        let outcome = map(rtl_session_outcome(session))
        if outcome == .cancelled { return .failure(.cancelled) }

        var capturedFrames: UInt64 = 0
        let capture = rtl_session_tone_capture(session, &capturedFrames)
        var callbackCount: UInt32 = 0
        let callbacks = rtl_session_tone_callbacks(session, &callbackCount)
        var gaps: UInt32 = 0
        var missing: UInt32 = 0
        tone_io_summary(callbacks, callbackCount, &gaps, &missing)

        let stored = 256
        var events = [tone_event_t](repeating: tone_event_t(), count: stored)
        var analysis = tone_result_t()
        let analysisStatus = events.withUnsafeMutableBufferPointer { buffer in
            tone_analyse(capture, capturedFrames, sampleRate, request.frequencyHz,
                         buffer.baseAddress, UInt32(stored), &analysis)
        }
        if analysisStatus != 0 { return .failure(.cannotAllocate) }

        let analysedFrames = analysis.analysedTo >= analysis.analysedFrom
            ? analysis.analysedTo - analysis.analysedFrom : 0
        let counts = tupleElements(analysis.counts, count: Int(TONE_EVENT_KINDS.rawValue), as: UInt32.self)

        var rows: [RtlToneEventRow] = []
        let listed = min(Int(analysis.storedEvents), request.maxListedEvents)
        for index in 0..<listed {
            let event = events[index]
            let callback = tone_cb_for(callbacks, callbackCount, event.firstSample)
            var inputSample: Double?
            var note = ""
            if let callback {
                let offset = Double(event.firstSample - callback.pointee.captureStart)
                if callback.pointee.itValid != 0 { inputSample = callback.pointee.itSample + offset }
                note = String(cString: tone_io_near(callbacks, callbackCount, callback))
            }
            rows.append(RtlToneEventRow(
                index: index + 1,
                kind: String(cString: tone_event_name(event.kind)),
                inputSample: inputSample,
                seconds: Double(event.firstSample) / sampleRate,
                frames: event.lastSample - event.firstSample + 1,
                silentSamples: event.silentSamples,
                slipFrames: event.kind == TONE_EVENT_SLIP ? event.slipFrames : nil,
                ioNote: note))
        }

        return .success(RtlToneResult(
            deviceName: deviceString(dev, rtl_dev_name),
            sampleRate: sampleRate,
            bufferRequested: request.requestedBufferFrames,
            bufferActual: buffer.actual != 0 ? buffer.actual : currentBufferFrames(dev) ?? 0,
            frequency: request.frequencyHz,
            duration: request.durationSeconds,
            outcome: outcome,
            callbacks: callbackCount,
            capturedFrames: capturedFrames,
            processorOverloads: rtl_session_overloads(session),
            sampleTimeGaps: gaps,
            missingTimestamps: missing,
            isValid: analysis.valid != 0,
            invalidReason: analysis.valid != 0 ? nil : analysis.invalidReason.map { String(cString: $0) },
            toneAmplitude: analysis.amplitude,
            noiseRms: analysis.noiseRms,
            analysedSeconds: Double(analysedFrames) / sampleRate,
            onsetSample: analysis.onsetSample,
            dropouts: counts[Int(TONE_EVENT_DROPOUT.rawValue)],
            slips: counts[Int(TONE_EVENT_SLIP.rawValue)],
            clicks: counts[Int(TONE_EVENT_CLICK.rawValue)],
            eventCount: analysis.eventCount,
            events: rows))
    }

    // MARK: Result construction

    private static func makeAudit(session: OpaquePointer,
                                  engine: UnsafePointer<rtl_engine_t>,
                                  overloads: UInt32) -> RtlIOAudit {
        let spanCount = min(Int(engine.pointee.spanCount), Int(RTL_MAX_SPANS))
        let spans = tupleElements(engine.pointee.spans, count: spanCount, as: UInt32.self)
        let inputChannels = min(Int(rtl_session_input_channels(session)), 64)
        let peaks = (0..<inputChannels).map { rtl_session_channel_peak(session, UInt32($0)) }
        return RtlIOAudit(
            callbacks: engine.pointee.cycles,
            distinctSpans: spans,
            processorOverloads: overloads,
            gapEvents: rtl_engine_gap_events(engine),
            gapFrames: rtl_engine_gap_frames(engine),
            anchorEvents: rtl_engine_anchor_events(engine),
            worstAnchorFrames: rtl_engine_worst_anchor(engine),
            clockDisagreements: rtl_engine_ambiguous(engine),
            missingTimestamps: rtl_engine_missing_ts(engine),
            inputChannelPeaks: peaks)
    }

    private static func stat(_ s: rtl_stat_t, accepted: Int) -> RtlStat? {
        guard accepted > 0 else { return nil }
        return RtlStat(count: Int(s.n), median: s.median, mean: s.mean, standardDeviation: s.sd,
                       minimum: s.min, maximum: s.max)
    }

    private static func makeResult(request: RtlMeasureRequest, name: String, uid: String,
                                   sampleRate: Double, bufferActual: UInt32, window: UInt32,
                                   trials: UInt32, outcome: RtlOutcome, declared: RtlDeclared,
                                   io: RtlIOAudit, reports: [rtl_trial_report_t],
                                   summary: rtl_summary_t, json: Data?) -> RtlMeasurementResult {
        let accepted = Int(summary.accepted)
        let run = Int(summary.trialsRun)

        let tally = tupleElements(summary.tally, count: Int(TRIAL_VERDICTS.rawValue), as: UInt32.self)
        var rejections: [RtlRejection] = []
        for verdict in 1..<Int(TRIAL_VERDICTS.rawValue) where tally[verdict] > 0 {
            let v = rtl_verdict_t(rawValue: UInt32(verdict))
            rejections.append(RtlRejection(id: String(cString: rtl_verdict_id(v)),
                                           name: String(cString: rtl_verdict_name(v)),
                                           count: Int(tally[verdict])))
        }

        let rows: [RtlTrialRow] = (0..<min(run, reports.count)).map { index in
            let report = reports[index]
            let result = report.result
            let found = result.ok != 0
            return RtlTrialRow(
                index: index,
                verdictID: String(cString: rtl_verdict_id(report.verdict)),
                verdictName: String(cString: rtl_verdict_name(report.verdict)),
                isAccepted: report.verdict == TRIAL_ACCEPTED,
                rawFrames: found ? result.rawFrames : nil,
                tsFrames: (found && result.tsValid != 0) ? result.tsFrames : nil,
                onsetFrames: found ? result.onset : nil,
                peak: result.peak,
                noise: result.noise,
                isInverted: result.inverted != 0,
                atWindowEdge: result.atWindowEdge != 0,
                callbacks: report.callbacks,
                overloads: report.overloads)
        }

        return RtlMeasurementResult(
            deviceName: name, deviceUID: uid, sampleRate: sampleRate,
            bufferRequested: request.requestedBufferFrames, bufferActual: bufferActual,
            window: window, trialsRequested: trials, outcome: outcome,
            declared: declared, io: io,
            trialsRun: summary.trialsRun, accepted: accepted, rejections: rejections,
            raw: stat(summary.raw, accepted: accepted),
            onset: stat(summary.onset, accepted: accepted),
            ts: stat(summary.ts, accepted: accepted),
            scheduling: stat(summary.sched, accepted: accepted),
            declaredSchedulingFrames: summary.declaredSched,
            declaredHardwareFrames: summary.declaredHw,
            residualFrames: (summary.residualValid != 0 && accepted > 0) ? summary.residualFrames : nil,
            minPeak: summary.minPeak, worstNoise: summary.worstNoise,
            invertedTrials: Int(summary.inverted),
            trials: rows, json: json)
    }

    /// The same asfw.rtl_loopback.v1 record the command line writes, with
    /// provenance naming the app and its version. Built here, while the engine
    /// is alive, because rtl_write_json reads the engine's audit counters.
    private static func makeJSON(request: RtlMeasureRequest, name: String, uid: String,
                                 sampleRate: Double, bufferActual: UInt32, window: UInt32,
                                 trials: UInt32, overloads: UInt32, timedOut: Bool,
                                 declared: inout rtl_declared_t, engine: UnsafePointer<rtl_engine_t>,
                                 reports: [rtl_trial_report_t], summary: inout rtl_summary_t) -> Data? {
        let timestamp = ISO8601DateFormatter().string(from: Date())
        let os = ProcessInfo.processInfo.operatingSystemVersion
        let osVersion = "\(os.majorVersion).\(os.minorVersion).\(os.patchVersion)"
        let argv = "ASFW.app Round-Trip Latency measure: \(request.optionsDescription)"

        return withCStrings(["ASFW.app/RoundTripLatency", request.appVersion, timestamp,
                             osVersion, name, uid, argv]) { strings in
            var provenance = rtl_provenance_t(
                tool: strings[0], toolVersion: strings[1], timestampUtc: strings[2],
                osVersion: strings[3], deviceName: strings[4], deviceUid: strings[5],
                argv: strings[6], sampleRate: sampleRate,
                bufferRequested: request.requestedBufferFrames ?? 0,
                bufferActual: bufferActual, window: window, trialsRequested: trials,
                overloadsTotal: overloads, timedOut: timedOut ? 1 : 0)
            var status: Int32 = -1
            let data = withMemoryStream { stream in
                reports.withUnsafeBufferPointer { buffer in
                    status = rtl_write_json(stream, &provenance, &declared, engine,
                                            buffer.baseAddress, &summary)
                }
            }
            return status == 0 ? data : nil
        }
    }

    // MARK: Buffer size

    /// A client buffer size requested for one run. The HAL keeps the buffer size
    /// per client process, so the size this app had before the run is put back
    /// afterwards; otherwise every later stream the app opens (the analyzer, the
    /// next run without a request) would silently inherit it.
    private struct BufferRequest {
        let device: AudioObjectID
        /// What the device adopted; 0 when nothing was requested or it could not be read.
        let actual: UInt32
        private let previous: UInt32?

        init(device: AudioObjectID, frames: UInt32?) {
            self.device = device
            guard let frames else {
                actual = 0
                previous = nil
                return
            }
            previous = RtlMeasurementRunner.currentBufferFrames(device)
            var got: UInt32 = 0
            _ = rtl_dev_request_buffer_frames(device, frames, &got)
            actual = got
        }

        func restore() {
            guard let previous, previous != actual else { return }
            var got: UInt32 = 0
            _ = rtl_dev_request_buffer_frames(device, previous, &got)
        }
    }

    static func currentBufferFrames(_ dev: AudioObjectID) -> UInt32? {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyBufferFrameSize,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        var value: UInt32 = 0
        var size = UInt32(MemoryLayout<UInt32>.size)
        guard AudioObjectGetPropertyData(dev, &address, 0, nil, &size, &value) == noErr else { return nil }
        return value
    }

    // MARK: Helpers

    private static func map(_ status: rtl_session_status_t) -> RtlRunnerError {
        switch status {
        case RTL_SESSION_NO_MEMORY: return .cannotAllocate
        case RTL_SESSION_CANNOT_CREATE_IOPROC: return .cannotCreateIOProc
        case RTL_SESSION_CANNOT_START: return .cannotStart
        default: return .invalidConfiguration
        }
    }

    private static func map(_ outcome: rtl_session_outcome_t) -> RtlOutcome {
        switch outcome {
        case RTL_OUTCOME_COMPLETED: return .completed
        case RTL_OUTCOME_CANCELLED: return .cancelled
        default: return .timedOut
        }
    }

    private static func deviceString(
        _ dev: AudioObjectID,
        _ read: (UInt32, UnsafeMutablePointer<CChar>?, Int) -> Int32
    ) -> String {
        var buffer = [CChar](repeating: 0, count: Int(RTL_DEV_STRING_MAX))
        _ = buffer.withUnsafeMutableBufferPointer { read(dev, $0.baseAddress, $0.count) }
        return String(cString: buffer)
    }

    /// The elements of a fixed-size C array, which Swift imports as a tuple.
    private static func tupleElements<Tuple, Element>(_ tuple: Tuple, count: Int,
                                                      as: Element.Type) -> [Element] {
        withUnsafeBytes(of: tuple) { raw in
            Array(raw.bindMemory(to: Element.self).prefix(count))
        }
    }

    /// Runs `body` with C strings that stay valid for the whole call.
    private static func withCStrings<R>(_ strings: [String],
                                        _ body: ([UnsafePointer<CChar>]) -> R) -> R {
        func recurse(_ index: Int, _ acc: [UnsafePointer<CChar>]) -> R {
            if index == strings.count { return body(acc) }
            return strings[index].withCString { recurse(index + 1, acc + [$0]) }
        }
        return recurse(0, [])
    }

    /// Hands `body` a FILE* that collects what the C core prints. Returns the
    /// collected bytes, or nil when the stream could not be created.
    private static func withMemoryStream(_ body: (UnsafeMutablePointer<FILE>) -> Void) -> Data? {
        var buffer: UnsafeMutablePointer<CChar>?
        var size = 0
        guard let stream = open_memstream(&buffer, &size) else { return nil }
        body(stream)
        fclose(stream)  // finalises `buffer` and `size`
        defer { free(buffer) }
        guard let buffer else { return Data() }
        return Data(bytes: buffer, count: size)
    }
}
