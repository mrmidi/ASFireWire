//
//  RoundTripLatencyTests.swift
//  ASFWTests
//
//  Pure logic behind the Round-Trip Latency section: which CoreAudio devices
//  are ASFW's, when the section is listed, and how frames become text. The
//  numbers themselves come from the C core, which tests/tools covers.
//

import Foundation
import Testing
@testable import ASFW

// MARK: - Device filtering

struct RoundTripLatencyDeviceFilterTests {
    @Test(arguments: ["ASFW-0003DB0000000001", "ASFW-130E0402004713B3", "ASFW-1"])
    func asfwDevicesAreRecognised(uid: String) {
        #expect(RtlAvailability.isASFWDevice(uid: uid))
    }

    @Test(arguments: ["BuiltInSpeakerDevice", "AppleUSBAudioEngine:Focusrite:1", "asfw-0003DB0000000001",
                      "ASFW-", "ASFW-not-hex", "XASFW-0003DB0000000001", ""])
    func otherDevicesAreNot(uid: String) {
        #expect(!RtlAvailability.isASFWDevice(uid: uid))
    }

    @Test func measuringNeedsBothDirections() {
        #expect(RtlAvailability.canMeasure(inputChannels: 2, outputChannels: 2))
        #expect(!RtlAvailability.canMeasure(inputChannels: 0, outputChannels: 8))
        #expect(!RtlAvailability.canMeasure(inputChannels: 8, outputChannels: 0))
    }

    @Test func channelsMustExist() {
        #expect(RtlAvailability.channelsAreValid(out: 0, in: 1, outputChannels: 2, inputChannels: 2))
        #expect(!RtlAvailability.channelsAreValid(out: 2, in: 0, outputChannels: 2, inputChannels: 2))
        #expect(!RtlAvailability.channelsAreValid(out: 0, in: -1, outputChannels: 2, inputChannels: 2))
    }

    @MainActor
    @Test func selectionKeepsTheCurrentDeviceThenPrefersDuplex() {
        typealias Choice = RoundTripLatencyViewModel.DeviceChoice
        let outputOnly = Choice(id: 10, uid: "ASFW-A", name: "Out only", inputChannels: 0, outputChannels: 2)
        let duplex = Choice(id: 11, uid: "ASFW-B", name: "Duplex", inputChannels: 2, outputChannels: 2)

        #expect(RoundTripLatencyViewModel.resolveSelection(current: nil, available: [outputOnly, duplex]) == 11)
        #expect(RoundTripLatencyViewModel.resolveSelection(current: 10, available: [outputOnly, duplex]) == 10)
        // The selected device was unplugged.
        #expect(RoundTripLatencyViewModel.resolveSelection(current: 12, available: [outputOnly, duplex]) == 11)
        #expect(RoundTripLatencyViewModel.resolveSelection(current: nil, available: [outputOnly]) == 10)
        #expect(RoundTripLatencyViewModel.resolveSelection(current: 11, available: []) == nil)
    }
}

// MARK: - Sidebar visibility

@MainActor
struct RoundTripLatencyVisibilityTests {
    typealias Section = ModernContentView.SidebarSection

    private func visible(advanced: Bool, asfwDevice: Bool) -> Bool {
        Section.roundTripLatency.isVisible(showAdvancedTools: advanced, hasAVCDevice: false,
                                           hasDICEDevice: false, hasDuetDevice: false,
                                           hasASFWAudioDevice: asfwDevice)
    }

    @Test func sectionIsAnAdvancedTool() {
        #expect(Section.roundTripLatency.group == .advanced)
        #expect(Section.roundTripLatency.requiresAdvancedTools)
    }

    @Test func listedOnlyWithAdvancedToolsAndAnASFWDevice() {
        #expect(visible(advanced: true, asfwDevice: true))
        #expect(!visible(advanced: true, asfwDevice: false))
        #expect(!visible(advanced: false, asfwDevice: true))
        #expect(!visible(advanced: false, asfwDevice: false))
    }

    @Test func unpluggingTheLastDeviceFallsBackToOverview() {
        let visible = Section.allCases.filter {
            $0.isVisible(showAdvancedTools: true, hasAVCDevice: false, hasDICEDevice: false,
                         hasDuetDevice: false, hasASFWAudioDevice: false)
        }
        #expect(!visible.contains(.roundTripLatency))
        #expect(Section.resolvedSelection(.roundTripLatency, visible: visible) == .overview)
        #expect(Section.resolvedSelection(.overview, visible: visible) == .overview)
        #expect(Section.resolvedSelection(nil, visible: visible) == nil)
    }
}

// MARK: - Units and wording

struct RoundTripLatencyFormatTests {
    @Test func framesBecomeMillisecondsAtTheActiveRate() throws {
        #expect(RtlFormat.milliseconds(frames: 48, sampleRate: 48_000) == 1.0)
        #expect(RtlFormat.milliseconds(frames: 441, sampleRate: 44_100) == 10.0)
        let ms = try #require(RtlFormat.milliseconds(frames: 1233, sampleRate: 48_000))
        #expect(abs(ms - 25.6875) < 1e-9)
    }

    @Test func noRateMeansNoMilliseconds() {
        #expect(RtlFormat.milliseconds(frames: 48, sampleRate: nil) == nil)
        #expect(RtlFormat.milliseconds(frames: 48, sampleRate: 0) == nil)
        #expect(RtlFormat.milliseconds(frames: .nan, sampleRate: 48_000) == nil)
        #expect(RtlFormat.millisecondsText(frames: 48, sampleRate: nil) == "rate unknown")
    }

    @Test func framesAndMillisecondsText() {
        #expect(RtlFormat.framesAndMilliseconds(1233, sampleRate: 48_000) == "1233.00 fr  25.688 ms")
        #expect(RtlFormat.framesAndMilliseconds(1233, sampleRate: 48_000, decimals: 0) == "1233 fr  25.688 ms")
        #expect(RtlFormat.framesAndMilliseconds(12.5, sampleRate: 48_000, signed: true) == "+12.50 fr  +0.260 ms")
        #expect(RtlFormat.framesAndMilliseconds(-4, sampleRate: 48_000, signed: true) == "-4.00 fr  -0.083 ms")
    }

    @Test func acceptanceRateIsPartOfTheMeasurement() {
        #expect(RtlFormat.acceptanceRate(accepted: 3, run: 20) == 0.15)
        #expect(RtlFormat.acceptanceRate(accepted: 0, run: 0) == nil)
        #expect(RtlFormat.acceptanceText(accepted: 3, run: 20) == "3 of 20 accepted (15%)")
        #expect(RtlFormat.acceptanceText(accepted: 20, run: 20) == "20 of 20 accepted (100%)")
        #expect(RtlFormat.acceptanceText(accepted: 0, run: 0) == "no trials run")
    }

    @Test func weakSignalBelowThirtyTimes() throws {
        #expect(RtlFormat.isWeakSignal(peak: 0.5, noise: 0.02))      // 25x
        #expect(!RtlFormat.isWeakSignal(peak: 0.5, noise: 0.01))     // 50x
        #expect(!RtlFormat.isWeakSignal(peak: 0.5, noise: 0))        // no noise measured
        #expect(RtlFormat.signalToNoiseRatio(peak: 0.5, noise: 0) == nil)
        let db = try #require(RtlFormat.signalToNoiseDecibels(peak: 0.5, noise: 0.005))
        #expect(abs(db - 40.0) < 1e-9)
    }

    @Test func windowLeavesTheEdgeGuard() {
        #expect(RtlFormat.maximumMeasurableFrames(window: 4096, edgeGuard: 64) == 4032)
        #expect(RtlFormat.maximumMeasurableFrames(window: 32, edgeGuard: 64) == 0)
    }

    @Test func durationWording() {
        #expect(RtlFormat.durationText(seconds: 0.4) == "under a second")
        #expect(RtlFormat.durationText(seconds: 12.4) == "about 12 s")
        #expect(RtlFormat.durationText(seconds: 150) == "about 2.5 min")
    }

    @Test func exportNameIsFilesystemSafeAndUTC() {
        let date = Date(timeIntervalSince1970: 1_792_145_700)  // 2026-10-16 10:15:00 UTC
        #expect(RtlFormat.exportFileName(deviceName: "Saffire PRO 40 (ASFW)", sampleRate: 48_000, date: date)
                == "rtl-Saffire-PRO-40-ASFW-48000-20261016-101500.json")
        #expect(RtlFormat.exportFileName(deviceName: "///", sampleRate: 44_100, date: date)
                == "rtl-device-44100-20261016-101500.json")
    }

    @Test func coercedBufferIsFlagged() {
        var tone = RtlToneResult(
            deviceName: "d", sampleRate: 48_000, bufferRequested: 32, bufferActual: 64,
            frequency: 997, duration: 30, outcome: .completed, callbacks: 0, capturedFrames: 0,
            processorOverloads: 0, sampleTimeGaps: 0, missingTimestamps: 0, isValid: true,
            invalidReason: nil, toneAmplitude: 0.5, noiseRms: 0, analysedSeconds: 0, onsetSample: 0,
            dropouts: 0, slips: 0, clicks: 0, eventCount: 0, events: [])
        #expect(tone.bufferWasCoerced)
        tone.bufferActual = 32
        #expect(!tone.bufferWasCoerced)
        tone.bufferRequested = nil
        #expect(!tone.bufferWasCoerced)
        #expect(tone.eventsPerMinute == nil)
        #expect(tone.averageFramesPerCallback == nil)
    }
}

// MARK: - Permission

struct RoundTripLatencyPermissionTests {
    @Test func deniedAndRestrictedBlockWithAReason() throws {
        let denied = try #require(MicrophoneAccess.denied.blockingExplanation)
        #expect(denied.contains("Microphone"))
        #expect(denied.contains("no signal"))  // says why the result would be wrong
        #expect(MicrophoneAccess.restricted.blockingExplanation != nil)
        #expect(!MicrophoneAccess.denied.allowsCapture)
    }

    @Test func authorizedAndUndeterminedDoNotBlock() {
        #expect(MicrophoneAccess.authorized.blockingExplanation == nil)
        #expect(MicrophoneAccess.authorized.allowsCapture)
        // Undetermined asks at Start; it does not capture until granted.
        #expect(MicrophoneAccess.notDetermined.blockingExplanation == nil)
        #expect(!MicrophoneAccess.notDetermined.allowsCapture)
    }
}
