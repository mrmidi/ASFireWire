//
//  DiceReportTextFormatterTests.swift
//  ASFWTests
//
//  The report is the deliverable a device owner pastes into an issue, so the
//  facts a maintainer needs must actually reach the text. These check content,
//  not layout.
//

import Foundation
import Testing
@testable import ASFW

struct DiceReportTextFormatterTests {

    /// A snapshot standing in for a fully-populated Saffire Pro 24 DSP.
    private static func saffireSnapshot() -> DiceSnapshot {
        var s = DiceSnapshot()
        s.nodeId = 1
        s.generation = 7
        s.guid = DiceFixtures.Saffire.guid
        s.vendorName = "Focusrite"
        s.modelName = "Saffire Pro 24 DSP"
        s.generalSections = DiceSectionTable.decode(DiceFixtures.Saffire.generalSectionTable,
                                                    names: DiceSectionTable.generalNames)
        s.global = DiceGlobal.decode(DiceFixtures.Saffire.globalSection, size: 380)

        let tx = DiceStreamSection.decode(DiceFixtures.Saffire.txSection, isTx: true)
        s.txCount = tx.count
        s.txSizeQuadlets = tx.sizeQuadlets
        s.txStreams = tx.entries

        let rx = DiceStreamSection.decode(DiceFixtures.Saffire.rxSection, isTx: false)
        s.rxCount = rx.count
        s.rxSizeQuadlets = rx.sizeQuadlets
        s.rxStreams = rx.entries

        s.extSync = DiceExtSync.decode(DiceFixtures.Saffire.extSyncSection)
        s.extensionSections = DiceSectionTable.decode(DiceFixtures.Saffire.extensionSectionTable,
                                                      names: DiceSectionTable.extensionNames)
        s.eapCaps = DiceEapCaps.decode(DiceFixtures.Saffire.eapCaps)
        return s
    }

    private let report = DiceReportTextFormatter.format(snapshot: saffireSnapshot(),
                                                        appVersion: "1.2.3")

    @Test func includesIdentityAndVersion() {
        #expect(report.contains("0x00130E0402004713"))
        #expect(report.contains("Focusrite"))
        #expect(report.contains("Saffire Pro 24 DSP"))
        #expect(report.contains("1.2.3"))
        #expect(report.contains("Report app:"))
    }

    @Test func distinguishesUnreadableSectionsFromReadEmptySections() {
        var s = Self.saffireSnapshot()
        s.mark("eap.stream_format", .unreadable)
        s.mark("eap.router", .decoded)
        let text = DiceReportTextFormatter.format(snapshot: s, appVersion: nil)
        #expect(text.contains("<unreadable>"))
        #expect(text.contains("<empty — nothing staged>"))
    }

    @Test func reportsClockStateInDecodedForm() {
        #expect(report.contains("0x0000020C"))
        #expect(report.contains("internal"))
        #expect(report.contains("48000"))
    }

    @Test func reportsClockCapabilitiesIncludingArx1() {
        #expect(report.contains("0x112C001E"))
        #expect(report.contains("arx1"), "arx1 availability decides whether a device can slave to the host")
        #expect(report.contains("44100 48000 88200 96000"))
    }

    @Test func reportsStreamGeometryForBothDirections() {
        #expect(report.contains("TX STREAMS"))
        #expect(report.contains("RX STREAMS"))
        #expect(report.contains("PCM channels = 16"))
        #expect(report.contains("PCM channels = 8"))
        #expect(report.contains("S400"))
    }

    @Test func reportsChannelNames() {
        #expect(report.contains("IP 1"))
        #expect(report.contains("Loop 2"))
        #expect(report.contains("Mon 1"))
        #expect(report.contains("SPDIF R"))
    }

    /// A missing register must be visibly absent rather than printed as zero.
    @Test func marksUnimplementedAc3RegistersExplicitly() {
        #expect(report.contains("not implemented"))
        #expect(report.contains("AC3_CAPS     = 0x00000000") == false,
                "a short stream block must not yield a fabricated zero")
    }

    @Test func reportDoesNotReadTheSlipRegister() {
        #expect(report.contains("EXT_STATUS is not read by this report"))
        #expect(report.contains("slip bits clear on quadlet reads"))
        #expect(report.contains("No cached sample available"))
    }

    @Test func reportsEapCapabilities() {
        #expect(report.contains("TCD2210 (DICE Mini)"))
        #expect(report.contains("maxEntries=128"))
        #expect(report.contains("inputs=18"))
        #expect(report.contains("outputs=16"))
    }

    @Test func reportsBothSectionTables() {
        #expect(report.contains("0xFFFFE0000000"))
        #expect(report.contains("0xFFFFE0200000"))
        #expect(report.contains("current_config"))
    }

    @Test func flagsNonStandardTcatCategory() {
        var weiss = DiceSnapshot()
        weiss.guid = (UInt64(0x001C_6A00) << 32) | (UInt64(6) << 22) | 12_345
        let text = DiceReportTextFormatter.format(snapshot: weiss, appVersion: nil)
        #expect(text.contains("0x001C6A"))
        #expect(text.contains("identifies the device family"),
                "a category other than 0x04 must be called out, not silently printed")
    }

    @Test func standardCategoryIsNotFlagged() {
        #expect(report.contains("standard DICE"))
    }

    /// An unreadable device still has to produce something a user can send.
    @Test func degradesGracefullyOnAnEmptySnapshot() {
        let text = DiceReportTextFormatter.format(snapshot: DiceSnapshot(), appVersion: nil)
        #expect(text.contains("ASFW DICE DEVICE REPORT"))
        #expect(text.contains("<unreadable>"))
        #expect(text.contains("<absent>"))
        #expect(text.isEmpty == false)
    }

    @Test func surfacesCollectionNotes() {
        var s = Self.saffireSnapshot()
        s.notes = ["RX section is allocated for 4 stream block(s) but NUMBER reports 1."]
        let text = DiceReportTextFormatter.format(snapshot: s, appVersion: nil)
        #expect(text.contains("NOTES"))
        #expect(text.contains("allocated for 4 stream block(s)"))
    }

    @Test func reportsPerRateModeFormatsWhenPresent() {
        var s = Self.saffireSnapshot()
        var formats = DiceEapStreamFormats()
        var entry = DiceEapFormatEntry()
        entry.pcmChannels = 2
        entry.midiPorts = 0
        entry.names = ["L", "R"]
        formats.rxEntries = [entry]
        s.eapCurrentFormats = [.low: formats, .middle: formats, .high: formats]

        let text = DiceReportTextFormatter.format(snapshot: s, appVersion: nil)
        #expect(text.contains("PER RATE MODE"))
        #expect(text.contains("low (32-48k)"))
        #expect(text.contains("middle (88.2-96k)"))
        #expect(text.contains("high (176.4-192k)"))
    }

    @Test func rendersRouterEntriesWithBlockNames() {
        var s = Self.saffireSnapshot()
        s.eapRouter = [DiceRouterEntry.decode(0x0000_25B3)]
        let text = DiceReportTextFormatter.format(snapshot: s, appVersion: nil)
        #expect(text.contains("EAP ROUTER"))
        #expect(text.contains("Avs0:3"))
        #expect(text.contains("Mixer:5"))
    }

    @Test func rendersMixerMatrix() {
        var s = Self.saffireSnapshot()
        var m = DiceMixer()
        m.saturation = 0x0000_00FF
        m.coefficients = [[1, 2], [3, 4]]
        s.eapMixer = m
        let text = DiceReportTextFormatter.format(snapshot: s, appVersion: nil)
        #expect(text.contains("EAP MIXER"))
        #expect(text.contains("2 outputs x 2 inputs"))
        #expect(text.contains("0x000000FF"))
        #expect(text.contains("mute"))
        #expect(text.contains("-84.3"))
    }
}

// MARK: - Staging vs live configuration, and peak clamping

struct DiceReportStagingAndPeakTests {

    private func base() -> DiceSnapshot {
        var s = DiceSnapshot()
        s.global = DiceGlobal.decode(DiceFixtures.Saffire.globalSection, size: 380)
        s.eapCaps = DiceEapCaps.decode(DiceFixtures.Saffire.eapCaps)
        return s
    }

    /// The router and stream_format sections are written and then LOADed via
    /// the cmd section; current_config is what runs. Labelling the staging area
    /// as the live configuration would mislead a maintainer reading the report.
    @Test func labelsStagingAreasAsStagingNotLive() {
        let text = DiceReportTextFormatter.format(snapshot: base(), appVersion: nil)
        #expect(text.contains("STREAM FORMAT STAGING AREA"))
        #expect(text.contains("ROUTER STAGING AREA"))
        #expect(text.contains("not the live config"))
        #expect(text.contains("EAP STREAM FORMATS  (current rate mode)") == false)
    }

    @Test func showsEmptyStagingExplicitly() {
        let text = DiceReportTextFormatter.format(snapshot: base(), appVersion: nil)
        #expect(text.contains("nothing staged"))
    }

    /// "read and empty" must not look like "not read" on an unknown device.
    @Test func printsEveryRateModeIncludingEmptyOnes() {
        var s = base()
        s.eapCurrentRouters[.low] = [DiceRouterEntry.decode(0x0000_4210)]
        s.eapCurrentRouters[.high] = []
        let text = DiceReportTextFormatter.format(snapshot: s, appVersion: nil)
        #expect(text.contains("low (32-48k)"))
        #expect(text.contains("middle (88.2-96k)"))
        #expect(text.contains("high (176.4-192k)"))
        #expect(text.contains("<not read>"), "middle was never populated")
        #expect(text.contains("advertises no rates in this mode"))
    }

    @Test func marksTheActiveRateMode() {
        var s = base()   // Saffire fixture is locked at 48 kHz -> low
        s.eapCurrentRouters[.low] = [DiceRouterEntry.decode(0x0000_4210)]
        let text = DiceReportTextFormatter.format(snapshot: s, appVersion: nil)
        #expect(text.contains("[ACTIVE]"))
    }

    @Test func omitsUninitialisedPeakSlotsAndSaysSo() {
        var s = base()
        s.eapCurrentRouters[.low] = (0..<3).map { _ in DiceRouterEntry.decode(0x0000_4210) }
        s.eapPeak = (0..<128).map { _ in DiceRouterEntry.decode(0x0BB8_4210) }
        let text = DiceReportTextFormatter.format(snapshot: s, appVersion: nil)
        #expect(text.contains("3 of 128 entries carry data"))
        #expect(text.contains("125 further slots"))
        #expect(text.contains("uninitialised"))
    }

    @Test func reportsActiveRateModeInTheHeader() {
        let text = DiceReportTextFormatter.format(snapshot: base(), appVersion: nil)
        #expect(text.contains("Rate mode: low (32-48k)"))
    }
}

// MARK: - Numeric scales

/// The mixer and peak sections use different fixed-point scales. A reader who
/// assumes the wrong one misreads levels by tens of dB, so the report states
/// both. Scales cross-checked against snd-firewire-ctl-services
/// tcd22xx_ctl.rs:700-702 (mixer, "2:14 Fixed-point") and :1195-1196
/// (peak, "Upper 12 bits of each sample").
struct DiceReportScaleTests {

    private func snapshotWithMixerAndPeak() -> DiceSnapshot {
        var s = DiceSnapshot()
        s.global = DiceGlobal.decode(DiceFixtures.Saffire.globalSection, size: 380)
        s.eapCaps = DiceEapCaps.decode(DiceFixtures.Saffire.eapCaps)
        var m = DiceMixer()
        m.saturation = 0x0000_0303
        m.coefficients = [[16384, 8208], [4861, 0]]
        s.eapMixer = m
        s.eapCurrentRouters[.low] = [DiceRouterEntry.decode(0x0000_4210)]
        s.eapPeak = [DiceRouterEntry.decode(0x0AAA_4210)]
        return s
    }

    @Test func documentsTheMixerFixedPointScale() {
        let text = DiceReportTextFormatter.format(snapshot: snapshotWithMixerAndPeak(),
                                                  appVersion: nil)
        #expect(text.contains("2:14 fixed-point"))
        #expect(text.contains("0.0 dB"))
        #expect(text.contains("-6.0"))
        #expect(text.contains("mute"))
    }

    @Test func explainsTheSaturationBitmask() {
        let text = DiceReportTextFormatter.format(snapshot: snapshotWithMixerAndPeak(),
                                                  appVersion: nil)
        #expect(text.contains("output n clipped"))
    }

    @Test func documentsThePeakScaleAsTwelveBit() {
        let text = DiceReportTextFormatter.format(snapshot: snapshotWithMixerAndPeak(),
                                                  appVersion: nil)
        #expect(text.contains("12-bit"))
        #expect(text.contains("4095"))
        #expect(text.contains("24 dB"), "the 16-bit misreading is worth naming explicitly")
    }

    /// Peak occupies the high half of the router quadlet; only its low 12 bits
    /// carry signal, so a full-scale reading must not exceed 4095.
    @Test func decodesPeakFromTheHighHalfOfTheEntry() {
        #expect(DiceRouterEntry.decode(0x0FFF_0000).peak == 4095)
        #expect(DiceRouterEntry.decode(0x0AAA_4210).peak == 0x0AAA)
        #expect(DiceRouterEntry.decode(0x0000_4210).peak == 0)
    }

    @Test func mixerCoefficientsSurviveDecodeUnscaled() {
        // The report prints raw values; conversion is the reader's, so the
        // decoder must not helpfully rescale anything.
        let d = DiceFixtures.place([(0x04, DiceFixtures.quad(16384)),
                                    (0x08, DiceFixtures.quad(8208))],
                                   size: 4 + 4 * 16 * 18)
        var caps = DiceEapCaps()
        caps.mixerExposed = true
        caps.mixerInputCount = 2
        caps.mixerOutputCount = 1
        let m = DiceMixer.decode(d, caps: caps)
        #expect(m.coefficients[0][0] == 16384)
        #expect(m.coefficients[0][1] == 8208)
    }
}

/// The DCP window header (DICE III control RPC) as the report reads and shows it.
struct DiceReportDcpTests {

    /// A header in the order the TCAT kext reads it (each quadlet to host
    /// order, then the two quadlets of a 64-bit field low word first):
    /// quadlet 0 carries the low half of the magic.
    private static let dcpHeader = DiceFixtures.quads([
        0x0406_1973, 0x1810_1966,      // magic 0x1810196604061973
        0x0000_0000, 0xFFC0_0001,      // owner: node 0xFFC0, offset 0x0001_0000_0000
        0x0000_0000, 0x0000_0002,      // +0x10, version 2
        0x0000_0400, 0x0000_0190,      // max request 1024, response area 400
        0xE040_01C0, 0x0000_FFFF,      // +0x20: request address
        0xE040_0030, 0x0000_FFFF,      // +0x28: response address
    ])

    @Test func dcpHeaderDecodesInTheKextWordOrder() throws {
        let h = try #require(DiceDcpHeader.decode(Self.dcpHeader))
        #expect(h.order == .lowFirst)
        #expect(h.owner == 0xFFC0_0001_0000_0000)
        #expect(h.version == 2)
        #expect(h.maxRequestBytes == 1024)
        #expect(h.responseAreaBytes == 400)
        #expect(DiceDcpHeader.windowOffset(of: h.field20) == 0x1C0)
        #expect(DiceDcpHeader.windowOffset(of: h.field28) == 0x30)
    }

    @Test func dcpHeaderMatchesTheMagicInAnyOrder() throws {
        let highFirst = DiceFixtures.quads([0x1810_1966, 0x0406_1973] + Array(repeating: 0, count: 10))
        #expect(try #require(DiceDcpHeader.decode(highFirst)).order == .highFirst)
        let swapped = DiceFixtures.quads([UInt32(0x0406_1973).byteSwapped, UInt32(0x1810_1966).byteSwapped]
                                         + Array(repeating: 0, count: 10))
        #expect(try #require(DiceDcpHeader.decode(swapped)).order == .byteSwappedLowFirst)
        #expect(DiceDcpHeader.decode(DiceFixtures.zeros(0x2C)) == nil, "short reads do not decode")
    }

    @Test func reportShowsTheDecodedDcpHeader() throws {
        var s = DiceSnapshot()
        s.dcp = try #require(DiceDcpHeader.decode(Self.dcpHeader))
        let text = DiceReportTextFormatter.format(snapshot: s, appVersion: nil)
        #expect(text.contains("DCP window @ 0xFFFFE0400000"))
        #expect(text.contains("+0x00  04061973 18101966 00000000 FFC00001"))
        #expect(text.contains("matches (quadlet 0 low)"))
        #expect(text.contains("node=0xFFC0 addr=0x000100000000"))
        #expect(text.contains("version        = 2"))
        #expect(text.contains("(window +0x1C0)"))
        #expect(text.contains("(window +0x030)"))
    }

    @Test func reportSaysWhenThereIsNoDcpWindowOrNoMagic() {
        var absent = DiceSnapshot()
        absent.mark("dcp.header", .absent)
        let absentText = DiceReportTextFormatter.format(snapshot: absent, appVersion: nil)
        let section = absentText.components(separatedBy: "DCP window @ 0xFFFFE0400000")
        #expect(section.count == 2)
        #expect(section.last?.contains("<absent>") == true)

        var foreign = DiceSnapshot()
        foreign.dcp = DiceDcpHeader.decode(DiceFixtures.quads(Array(repeating: 0x1234_5678, count: 12)))
        let foreignText = DiceReportTextFormatter.format(snapshot: foreign, appVersion: nil)
        #expect(foreignText.contains("not a DCP header"))
    }
}
