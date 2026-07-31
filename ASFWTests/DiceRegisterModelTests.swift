//
//  DiceRegisterModelTests.swift
//  ASFWTests
//
//  Decoders for the DICE general and extended-application register spaces.
//
//  Getting an offset or a bit position wrong here does not crash: it produces a
//  confident, wrong description of someone else's hardware. The golden fixture
//  is a Saffire Pro 24 DSP captured from real hardware, so these pin the
//  decoders against a device rather than against a reading of the spec.
//

import Foundation
import Testing
@testable import ASFW

// MARK: - Section tables

struct DiceSectionTableTests {

    @Test func decodesGeneralSectionTable() throws {
        let t = try #require(DiceSectionTable.decode(DiceFixtures.Saffire.generalSectionTable,
                                                     names: DiceSectionTable.generalNames))
        let global = try #require(t["global"])
        #expect(global.offsetQuadlets == 0x00A)
        #expect(global.byteOffset == 0x28, "global is NOT at offset 0 on real hardware")
        #expect(global.sizeQuadlets == 0x05F)
        #expect(global.byteSize == 380)

        #expect(t["tx"]?.byteOffset == 0x1A4)
        #expect(t["rx"]?.byteOffset == 0x3DC)
        #expect(t["ext_sync"]?.byteSize == 16)
    }

    @Test func decodesExtensionSectionTable() throws {
        let t = try #require(DiceSectionTable.decode(DiceFixtures.Saffire.extensionSectionTable,
                                                     names: DiceSectionTable.extensionNames))
        #expect(t.sections.count == 9)
        #expect(t["caps"]?.byteOffset == 0x4C)
        #expect(t["router"]?.sizeQuadlets == 0x81)
        #expect(t["current_config"]?.byteSize == 24576)
        #expect(t["application"]?.byteSize == 148_976)
    }

    @Test func absentSectionIsNotPresent() throws {
        let t = try #require(DiceSectionTable.decode(DiceFixtures.Saffire.generalSectionTable,
                                                     names: DiceSectionTable.generalNames))
        #expect(t["unused2"]?.isPresent == false)
        #expect(t["global"]?.isPresent == true)
    }

    @Test func rejectsTruncatedTable() {
        #expect(DiceSectionTable.decode(DiceFixtures.zeros(12),
                                        names: DiceSectionTable.generalNames) == nil)
    }

    @Test func rejectsAnEmptyGeneralTableWhenADeviceIsBeingClassified() {
        #expect(DiceSectionTable.decode(DiceFixtures.zeros(40),
                                        names: DiceSectionTable.generalNames,
                                        requiresPresentSection: true) == nil)
    }

    @Test func rejectsOutOfRangeAndOverlappingSections() {
        let outOfRange = DiceFixtures.quads([0x0040_0001, 1, 0, 0, 0, 0, 0, 0, 0, 0])
        #expect(DiceSectionTable.decode(outOfRange, names: DiceSectionTable.generalNames) == nil)

        let overlapping = DiceFixtures.quads([10, 8, 17, 2, 0, 0, 0, 0, 0, 0])
        #expect(DiceSectionTable.decode(overlapping, names: DiceSectionTable.generalNames) == nil)
    }

    @Test func unknownSectionNameReturnsNil() throws {
        let t = try #require(DiceSectionTable.decode(DiceFixtures.Saffire.generalSectionTable,
                                                     names: DiceSectionTable.generalNames))
        #expect(t["nonexistent"] == nil)
    }
}

// MARK: - Global

struct DiceGlobalTests {

    private let global = DiceGlobal.decode(DiceFixtures.Saffire.globalSection, size: 380)

    @Test func decodesClockSelect() {
        #expect(global.clockSelect == 0x0000_020C)
        #expect(global.clockSourceIndex == 12)
        #expect(global.clockRateIndex == 2)
        #expect(DiceClockSource.name(global.clockSourceIndex) == "internal")
        #expect(DiceClockRate.name(global.clockRateIndex) == "48000")
    }

    @Test func decodesStatus() {
        #expect(global.status == 0x0000_0201)
        #expect(global.sourceLocked == true)
        #expect(global.nominalRateIndex == 2)
    }

    @Test func decodesIdentityAndVersion() {
        #expect(global.nickname == "Pro24DSP-004713")
        #expect(global.version == 0x0100_0C00)
        #expect(global.measuredRate == 48_000)
        #expect(global.enable == 1)
    }

    @Test func decodesOwner() {
        #expect(global.ownerRaw == 0xFFC0_0001_0000_0000)
        #expect(global.ownerNodeId == 0xFFC0)
        #expect(global.hasOwner == true)
    }

    @Test func detectsUnownedDevice() {
        // OWNER_NO_OWNER is 0xffff000000000000 (dice-interface.h:56).
        let d = DiceFixtures.place([(0x00, DiceFixtures.quad(0xFFFF_0000)),
                                    (0x04, DiceFixtures.quad(0))], size: 380)
        #expect(DiceGlobal.decode(d, size: 380).hasOwner == false)
    }

    @Test func decodesClockSourceNames() {
        #expect(global.clockSourceNames.count == 13)
        #expect(global.clockSourceNames[12] == "Internal")
    }

    /// Old firmware omits VERSION, CLOCK_CAPABILITIES and CLOCK_SOURCE_NAMES.
    /// A register is only supported when its offset lies inside the section's
    /// declared size (dice-interface.h:26-32, 178-182).
    @Test func gatesLateRegistersOnSectionSize() {
        let short = DiceGlobal.decode(DiceFixtures.Saffire.globalSection, size: 0x60)
        #expect(short.version == 0)
        #expect(short.clockCaps == 0)
        #expect(short.clockSourceNames.isEmpty)
        // Registers below the cut still decode.
        #expect(short.clockSelect == 0x0000_020C)
        #expect(short.measuredRate == 48_000)
    }
}

// MARK: - Clock capability and status bitfields

struct DiceClockCapsTests {

    @Test func decodesSaffireCapabilities() {
        let caps: UInt32 = 0x112C_001E
        #expect(DiceClockCaps.rates(caps) == ["44100", "48000", "88200", "96000"])
        #expect(DiceClockCaps.sources(caps) == ["aes3", "aes4", "adat", "arx1", "internal"])
    }

    @Test func arx1CapabilityBitIsBit24() {
        // CLOCK_CAP_SOURCE_ARX1 = 0x01000000 (dice-interface.h:209). This bit
        // decides whether a device can slave its sample clock to our stream.
        #expect(DiceClockCaps.sources(0x0100_0000) == ["arx1"])
    }

    @Test(arguments: zip([UInt32(0x01), 0x02, 0x04, 0x08, 0x10, 0x20, 0x40],
                         ["32000", "44100", "48000", "88200", "96000", "176400", "192000"]))
    func decodesEachRateBit(bit: UInt32, name: String) {
        #expect(DiceClockCaps.rates(bit) == [name])
    }

    @Test func emptyCapabilitiesYieldNothing() {
        #expect(DiceClockCaps.rates(0).isEmpty)
        #expect(DiceClockCaps.sources(0).isEmpty)
    }
}

struct DiceExtStatusTests {

    /// EXT_STATUS orders its flags differently from the clock-source
    /// enumeration: there is no aes_any entry, and wc sits last rather than at
    /// index 7 (dice-interface.h:144-154 vs 92-104).
    @Test func lockOrderDiffersFromClockSourceEnum() {
        #expect(DiceExtStatusBits.order == ["aes1", "aes2", "aes3", "aes4", "adat",
                                            "tdif", "arx1", "arx2", "arx3", "arx4", "wc"])
        #expect(DiceExtStatusBits.locked(0x0000_0040) == ["arx1"])
        #expect(DiceExtStatusBits.locked(0x0000_0400) == ["wc"])
    }

    @Test func slipFlagsLiveInHighHalf() {
        #expect(DiceExtStatusBits.slipped(0x0040_0000) == ["arx1"])
        #expect(DiceExtStatusBits.slipped(0x0000_0040).isEmpty,
                "a lock flag must not be reported as a slip")
    }

    @Test func decodesSaffireExtendedStatus() {
        let v: UInt32 = 0x0000_0040
        #expect(DiceExtStatusBits.locked(v) == ["arx1"])
        #expect(DiceExtStatusBits.slipped(v).isEmpty)
    }
}

struct DiceNotificationTests {

    @Test func decodesKnownBits() {
        #expect(DiceNotificationBits.names(0x01) == ["RX_CFG_CHG"])
        #expect(DiceNotificationBits.names(0x02) == ["TX_CFG_CHG"])
        #expect(DiceNotificationBits.names(0x10) == ["LOCK_CHG"])
        #expect(DiceNotificationBits.names(0x20) == ["CLOCK_ACCEPTED"])
        #expect(DiceNotificationBits.names(0x40) == ["EXT_STATUS"])
    }

    /// "Other bits may be used for device-specific events"
    /// (dice-interface.h:74) — the Saffire sets bit 24.
    @Test func surfacesVendorSpecificBits() {
        let names = DiceNotificationBits.names(0x0100_0000)
        #expect(names.count == 1)
        #expect(names[0].contains("vendor"))
    }

    @Test func emptyNotificationIsEmpty() {
        #expect(DiceNotificationBits.names(0).isEmpty)
    }
}

// MARK: - Stream sections

struct DiceStreamSectionTests {

    @Test func decodesSaffireTxStream() throws {
        let r = DiceStreamSection.decode(DiceFixtures.Saffire.txSection, isTx: true)
        #expect(r.count == 1)
        #expect(r.sizeQuadlets == 70)
        let e = try #require(r.entries.first)
        #expect(e.isoChannel == 1)
        #expect(e.pcmChannels == 16)
        #expect(e.midiPorts == 1)
        #expect(e.speed == 2)
        #expect(e.speedLabel == "S400")
        #expect(e.names.count == 16)
        #expect(e.names.first == "IP 1")
        #expect(e.names.last == "Loop 2")
        #expect(e.sequenceStart == nil, "SEQ_START is an RX-only register")
    }

    /// RX inserts SEQ_START at 0x0C, pushing the channel counts one quadlet
    /// later than TX. Decoding RX with the TX layout silently reports the
    /// sequence start as the PCM channel count.
    @Test func decodesSaffireRxStreamWithSequenceStartShift() throws {
        let r = DiceStreamSection.decode(DiceFixtures.Saffire.rxSection, isTx: false)
        let e = try #require(r.entries.first)
        #expect(e.isoChannel == 0)
        #expect(e.sequenceStart == 0)
        #expect(e.pcmChannels == 8)
        #expect(e.midiPorts == 1)
        #expect(e.speed == nil, "SPEED is a TX-only register")
        #expect(e.names == DiceFixtures.Saffire.rxNames)
    }

    @Test func rxDecodedAsTxMisreadsChannelCount() {
        // Guards the shift explicitly: the TX layout reads SEQ_START (0) where
        // RX keeps its PCM count (8).
        let wrong = DiceStreamSection.decode(DiceFixtures.Saffire.rxSection, isTx: true)
        #expect(wrong.entries.first?.pcmChannels == 0)
    }

    /// A 70-quadlet block ends at 0x118, exactly where AC3_CAPABILITIES starts,
    /// so reading it would return the next stream's data.
    @Test func omitsAc3RegistersWhenStreamBlockTooShort() throws {
        let r = DiceStreamSection.decode(DiceFixtures.Saffire.txSection, isTx: true)
        let e = try #require(r.entries.first)
        #expect(e.ac3Capabilities == nil)
        #expect(e.ac3Enable == nil)
    }

    @Test func decodesAc3RegistersWhenStreamBlockIsLongEnough() throws {
        // 72 quadlets (288 bytes) is the first size that contains 0x11C.
        let section = DiceFixtures.place([
            (0x00, DiceFixtures.quad(1)),
            (0x04, DiceFixtures.quad(72)),
            (0x08, DiceFixtures.quad(3)),
            (0x0C, DiceFixtures.quad(2)),
            (0x10, DiceFixtures.quad(0)),
            (0x14, DiceFixtures.quad(2)),
            (0x118, DiceFixtures.quad(0x0000_0003)),
            (0x11C, DiceFixtures.quad(0x0000_0001)),
        ], size: 8 + 288)
        let e = try #require(DiceStreamSection.decode(section, isTx: true).entries.first)
        #expect(e.ac3Capabilities == 0x0000_0003)
        #expect(e.ac3Enable == 0x0000_0001)
    }

    @Test func stridesAcrossMultipleStreams() throws {
        let section = DiceFixtures.place([
            (0x00, DiceFixtures.quad(2)),
            (0x04, DiceFixtures.quad(70)),
            // stream 0
            (0x08, DiceFixtures.quad(1)),
            (0x0C, DiceFixtures.quad(4)),
            // stream 1 sits one 280-byte stride later
            (0x08 + 280, DiceFixtures.quad(5)),
            (0x0C + 280, DiceFixtures.quad(6)),
        ], size: 8 + 2 * 280)
        let r = DiceStreamSection.decode(section, isTx: true)
        #expect(r.entries.count == 2)
        #expect(r.entries[0].isoChannel == 1)
        #expect(r.entries[0].pcmChannels == 4)
        #expect(r.entries[1].isoChannel == 5)
        #expect(r.entries[1].pcmChannels == 6)
        #expect(r.entries[1].index == 1)
    }

    @Test func treatsMinusOneIsoChannelAsDisabled() throws {
        let section = DiceFixtures.place([
            (0x00, DiceFixtures.quad(1)),
            (0x04, DiceFixtures.quad(70)),
            (0x08, DiceFixtures.quad(0xFFFF_FFFF)),
        ], size: 288)
        let e = try #require(DiceStreamSection.decode(section, isTx: true).entries.first)
        #expect(e.isoChannel == -1)
    }

    @Test func handlesZeroStreamCount() {
        // A unidirectional device reports 0 in one direction. This must decode
        // to an empty list, not a phantom stream.
        let section = DiceFixtures.place([(0x00, DiceFixtures.quad(0)),
                                          (0x04, DiceFixtures.quad(70))], size: 288)
        let r = DiceStreamSection.decode(section, isTx: true)
        #expect(r.count == 0)
        #expect(r.entries.isEmpty)
    }

    @Test func handlesZeroStreamSize() {
        let section = DiceFixtures.place([(0x00, DiceFixtures.quad(1)),
                                          (0x04, DiceFixtures.quad(0))], size: 64)
        #expect(DiceStreamSection.decode(section, isTx: true).entries.isEmpty)
    }
}

// MARK: - Ext sync

struct DiceExtSyncTests {

    @Test func decodesSaffireExtSync() {
        let x = DiceExtSync.decode(DiceFixtures.Saffire.extSyncSection)
        #expect(x.clockSource == 12)
        #expect(x.locked == true)
        #expect(x.rateIndex == 2)
        #expect(x.adatUserData == nil, "bit 4 marks the ADAT user data unavailable")
    }

    @Test func decodesAvailableAdatUserData() {
        let d = DiceFixtures.quads([0, 1, 2, 0x0A])
        #expect(DiceExtSync.decode(d).adatUserData == 0x0A)
    }

    @Test func reportsUnlockedSource() {
        let d = DiceFixtures.quads([5, 0, 0, 0x10])
        let x = DiceExtSync.decode(d)
        #expect(x.locked == false)
        #expect(x.clockSource == 5)
    }
}

// MARK: - EAP capabilities

struct DiceEapCapsTests {

    @Test func decodesSaffireCapabilities() throws {
        let c = try #require(DiceEapCaps.decode(DiceFixtures.Saffire.eapCaps))
        #expect(c.routerExposed == true)
        #expect(c.routerReadOnly == false)
        #expect(c.routerStorable == true)
        #expect(c.routerMaxEntries == 128)

        #expect(c.mixerExposed == true)
        #expect(c.mixerReadOnly == false)
        #expect(c.mixerStorable == true)
        #expect(c.mixerInputDeviceId == 2)
        #expect(c.mixerOutputDeviceId == 2)
        #expect(c.mixerInputCount == 18)
        #expect(c.mixerOutputCount == 16)

        #expect(c.dynamicStreamFormat == true)
        #expect(c.storageAvailable == true)
        #expect(c.peakAvailable == true)
        #expect(c.maxTxStreams == 2)
        #expect(c.maxRxStreams == 2)
        #expect(c.streamFormatStorable == true)
        #expect(c.asicLabel == "TCD2210 (DICE Mini)")
    }

    @Test(arguments: zip([UInt32(0), 1, 2], ["DICE II", "TCD2210 (DICE Mini)", "TCD2220 (DICE Jr)"]))
    func labelsEachAsicType(raw: UInt32, label: String) throws {
        let d = DiceFixtures.quads([0, 0, raw << 16])
        let c = try #require(DiceEapCaps.decode(d))
        #expect(c.asicLabel == label)
    }

    @Test func labelsUnknownAsicType() throws {
        let d = DiceFixtures.quads([0, 0, 0x00FF_0000])
        let c = try #require(DiceEapCaps.decode(d))
        #expect(c.asicLabel == "unknown(255)")
    }

    @Test func preservesTheFullSixteenBitAsicType() throws {
        let d = DiceFixtures.quads([0, 0, 0xABCD_0000])
        let c = try #require(DiceEapCaps.decode(d))
        #expect(c.asicLabel == "unknown(43981)")
    }

    @Test func rejectsTruncatedCapabilities() {
        #expect(DiceEapCaps.decode(DiceFixtures.zeros(8)) == nil)
    }
}

// MARK: - Router entries

struct DiceRouterEntryTests {

    @Test func decodesPackedEntry() {
        // Peak reserves its upper nibble, so 0x1234 decodes as 0x234.
        // src Mixer(2) ch 5 -> 0x25, dst Avs0(11) ch 3 -> 0xB3.
        let e = DiceRouterEntry.decode(0x1234_25B3)
        #expect(e.dstId == 11)
        #expect(e.dstChannel == 3)
        #expect(e.srcId == 2)
        #expect(e.srcChannel == 5)
        #expect(e.peak == 0x0234)
        #expect(e.dstLabel == "Avs0:3")
        #expect(e.srcLabel == "Mixer:5")
    }

    @Test func namesDistinguishSourceAndDestinationBlocks() {
        // id 2 is MixerTx0 as a destination but Mixer as a source, and id 15 is
        // Mute only on the source side (router_entry.rs:11-20, 85-96).
        #expect(DiceRouterEntry.dstName(2) == "MixerTx0")
        #expect(DiceRouterEntry.srcName(2) == "Mixer")
        #expect(DiceRouterEntry.srcName(15) == "Mute")
        #expect(DiceRouterEntry.dstName(15) == "reserved(15)")
    }

    @Test func labelsUnknownBlockIds() {
        #expect(DiceRouterEntry.dstName(7) == "reserved(7)")
        #expect(DiceRouterEntry.srcName(3) == "reserved(3)")
    }

    /// The router section prefixes its entries with a count; the peak section
    /// does not and is sized from routerMaxEntries instead.
    @Test func decodesRouterSectionWithLeadingCount() {
        // entry 0: dst Adat(1) ch 0 = 0x10, src Ins0(4) ch 2 = 0x42
        // entry 1: dst MixerTx0(2) ch 1 = 0x21, src Mixer(2) ch 0 = 0x20
        let d = DiceFixtures.quads([2, 0x0000_4210, 0x0000_2021, 0xDEAD_BEEF])
        let entries = DiceRouterEntry.decodeAll(d, hasLeadingCount: true, maxEntries: 128)
        #expect(entries.count == 2, "the trailing quadlet is beyond the declared count")
        #expect(entries[0].dstId == 1)
        #expect(entries[0].dstChannel == 0)
        #expect(entries[0].srcId == 4)
        #expect(entries[0].srcChannel == 2)
        #expect(entries[1].dstId == 2)
        #expect(entries[1].dstChannel == 1)
        #expect(entries[1].srcId == 2)
        #expect(entries[1].srcChannel == 0)
    }

    @Test func decodesPeakSectionWithoutLeadingCount() {
        let d = DiceFixtures.quads([0x0001_0110, 0x0002_0221])
        let entries = DiceRouterEntry.decodeAll(d, hasLeadingCount: false, maxEntries: 2)
        #expect(entries.count == 2)
        #expect(entries[0].peak == 1)
        #expect(entries[1].peak == 2)
    }

    @Test func clampsToAvailableBytes() {
        // Claims 100 entries but only supplies two.
        let d = DiceFixtures.quads([100, 0, 0])
        #expect(DiceRouterEntry.decodeAll(d, hasLeadingCount: true, maxEntries: 128).count == 2)
    }

    @Test func handlesEmptyRouter() {
        let d = DiceFixtures.quad(0)
        #expect(DiceRouterEntry.decodeAll(d, hasLeadingCount: true, maxEntries: 128).isEmpty)
    }
}

// MARK: - EAP stream formats

struct DiceEapStreamFormatTests {

    private func entry(pcm: UInt32, midi: UInt32, names: [String], ac3: UInt32) -> Data {
        DiceFixtures.quads([pcm, midi]) + DiceFixtures.labels(names)
            + DiceFixtures.quad(ac3)
    }

    @Test func entrySizeIs268Bytes() {
        #expect(DiceEapFormatEntry.size == 268)
        #expect(entry(pcm: 1, midi: 0, names: ["a"], ac3: 0).count == 268)
    }

    @Test func decodesSingleEntry() throws {
        let d = entry(pcm: 16, midi: 1, names: ["IP 1", "IP 2"], ac3: 0x0000_0003)
        let e = try #require(DiceEapFormatEntry.decode(d, at: 0))
        #expect(e.pcmChannels == 16)
        #expect(e.midiPorts == 1)
        #expect(e.names == ["IP 1", "IP 2"])
        #expect(e.ac3Mask == 0x0000_0003)
    }

    @Test func decodesTxThenRxEntries() {
        var d = DiceFixtures.quads([1, 1])
        d.append(entry(pcm: 16, midi: 1, names: ["tx"], ac3: 0))
        d.append(entry(pcm: 8, midi: 1, names: ["rx"], ac3: 0))
        let f = DiceEapStreamFormats.decode(d)
        #expect(f.txEntries.count == 1)
        #expect(f.rxEntries.count == 1)
        #expect(f.txEntries[0].pcmChannels == 16)
        #expect(f.rxEntries[0].pcmChannels == 8)
    }

    @Test func stopsWhenDataRunsOut() {
        // Claims two entries per direction but supplies one entry's worth.
        var d = DiceFixtures.quads([2, 2])
        d.append(entry(pcm: 4, midi: 0, names: ["x"], ac3: 0))
        let f = DiceEapStreamFormats.decode(d)
        #expect(f.txEntries.count == 1)
        #expect(f.rxEntries.isEmpty)
    }

    @Test func decodesZeroEntriesForUnidirectionalDevice() {
        var d = DiceFixtures.quads([0, 1])
        d.append(entry(pcm: 2, midi: 0, names: ["L", "R"], ac3: 0))
        let f = DiceEapStreamFormats.decode(d)
        #expect(f.txEntries.isEmpty)
        #expect(f.rxEntries.count == 1)
        #expect(f.rxEntries[0].pcmChannels == 2)
    }
}

// MARK: - Rate modes

struct DiceRateModeTests {

    /// current_config_section.rs:29-34 — router and stream configuration for
    /// each rate mode live at fixed sub-offsets inside the section.
    @Test func exposesFixedSubOffsets() {
        #expect(DiceRateMode.low.routerOffset == 0x0000)
        #expect(DiceRateMode.low.streamOffset == 0x1000)
        #expect(DiceRateMode.middle.routerOffset == 0x2000)
        #expect(DiceRateMode.middle.streamOffset == 0x3000)
        #expect(DiceRateMode.high.routerOffset == 0x4000)
        #expect(DiceRateMode.high.streamOffset == 0x5000)
    }

    @Test func coversAllThreeModes() {
        #expect(DiceRateMode.allCases.count == 3)
    }
}

// MARK: - Mixer

struct DiceMixerTests {

    private func mixerCaps(inputs: UInt8, outputs: UInt8) -> DiceEapCaps {
        var c = DiceEapCaps()
        c.mixerExposed = true
        c.mixerInputCount = inputs
        c.mixerOutputCount = outputs
        return c
    }

    /// Coefficients are stored on a fixed 16x18 grid regardless of the
    /// advertised counts, so the row stride is MAX_INPUTS and not the device's
    /// own input count (mixer_section.rs:21-25, 72).
    @Test func indexesCoefficientsOnTheFixedGridStride() {
        var pieces: [(Int, Data)] = [(0x00, DiceFixtures.quad(0))]
        // Mark output 1, input 0 -- at 4 * (1 * 18 + 0) = 72 past the coeffs.
        pieces.append((0x04 + 72, DiceFixtures.quad(0x0000_1234)))
        // Output 0, input 2 -- at 4 * (0 * 18 + 2) = 8.
        pieces.append((0x04 + 8, DiceFixtures.quad(0x0000_5678)))
        let d = DiceFixtures.place(pieces, size: 4 + 4 * 16 * 18)

        let m = DiceMixer.decode(d, caps: mixerCaps(inputs: 18, outputs: 16))
        #expect(m.coefficients[1][0] == 0x1234)
        #expect(m.coefficients[0][2] == 0x5678)
        #expect(m.coefficients[0][0] == 0)
    }

    @Test func clampsToAdvertisedCounts() {
        let d = DiceFixtures.place([(0x00, DiceFixtures.quad(0xFF))],
                                   size: 4 + 4 * 16 * 18)
        let m = DiceMixer.decode(d, caps: mixerCaps(inputs: 4, outputs: 2))
        #expect(m.coefficients.count == 2)
        #expect(m.coefficients[0].count == 4)
        #expect(m.saturation == 0xFF)
    }

    @Test func handlesZeroSizedMixer() {
        let d = DiceFixtures.zeros(4 + 4 * 16 * 18)
        let m = DiceMixer.decode(d, caps: mixerCaps(inputs: 0, outputs: 0))
        #expect(m.coefficients.isEmpty)
    }

    @Test func neverExceedsTheHardwareGrid() {
        // A device claiming more than the grid must not read out of bounds.
        let d = DiceFixtures.zeros(4 + 4 * 16 * 18)
        let m = DiceMixer.decode(d, caps: mixerCaps(inputs: 200, outputs: 200))
        #expect(m.coefficients.count == DiceMixer.maxOutputs)
        #expect(m.coefficients[0].count == DiceMixer.maxInputs)
    }
}

// MARK: - Standalone

struct DiceStandaloneTests {

    @Test func decodesFields() {
        let d = DiceFixtures.quads([12, 1, 2, 0x0001_0021, 4])
        let s = DiceStandalone.decode(d)
        #expect(s.clockSource == 12)
        #expect(s.aesHighRate == true)
        #expect(s.adatModeLabel == "SMUX4")
        #expect(s.wordClockModeLabel == "Low")
        #expect(s.wordClockNumerator == 3)
        #expect(s.wordClockDenominator == 2)
        #expect(DiceClockRate.name(s.internalRateIndex) == "96000")
    }

    @Test func decodesZeroEncodedWordClockRatioAsOneOverOne() {
        let s = DiceStandalone.decode(DiceFixtures.quads([0, 0, 0, 0, 0]))
        #expect(s.wordClockNumerator == 1)
        #expect(s.wordClockDenominator == 1)
    }

    @Test(arguments: zip([UInt32(0), 1, 2, 3], ["Normal", "SMUX2", "SMUX4", "Auto"]))
    func labelsAdatModes(raw: UInt32, label: String) {
        var s = DiceStandalone()
        s.adatModeRaw = raw
        #expect(s.adatModeLabel == label)
    }

    @Test(arguments: zip([UInt32(0), 1, 2, 3], ["Normal", "Low", "Middle", "High"]))
    func labelsWordClockModes(raw: UInt32, label: String) {
        var s = DiceStandalone()
        s.wordClockRaw = raw
        #expect(s.wordClockModeLabel == label)
    }
}

// MARK: - TCAT identity

struct DiceTcatIdentityTests {

    /// Linux dice.c:66-67 checks config_rom[3] == (vendor << 8 | category) and
    /// config_rom[4] >> 22 == model, i.e. the GUID's high quadlet carries
    /// vendor+category and the low quadlet carries product+serial.
    @Test func decodesSaffireIdentity() {
        var s = DiceSnapshot()
        s.guid = DiceFixtures.Saffire.guid
        #expect(s.tcatVendorId == 0x0013_0E)
        #expect(s.tcatCategory == 0x04, "standard DICE category")
        #expect(s.tcatProductId == 8)
        #expect(s.tcatSerial == 0x4713)
    }

    /// Weiss uses category 0x00 where the rest of DICE uses 0x04
    /// (dice.c:27, 58-59). The report has to surface that.
    @Test func decodesWeissIdentityWithNonStandardCategory() {
        var s = DiceSnapshot()
        // vendor 0x001C6A, category 0x00, product 6 (INT 202), serial 12345.
        s.guid = (UInt64(0x001C_6A00) << 32) | (UInt64(6) << 22) | 12_345
        #expect(s.tcatVendorId == 0x001C_6A)
        #expect(s.tcatCategory == 0x00)
        #expect(s.tcatProductId == 6)
        #expect(s.tcatSerial == 12_345)
    }

    /// The product field is 10 bits taken from the low quadlet. Vendor bits
    /// from the high quadlet must not leak into it.
    @Test func productFieldIsolatesLowQuadlet() {
        var s = DiceSnapshot()
        s.guid = (UInt64(0xFFFF_FFFF) << 32) | (UInt64(0x3FF) << 22) | 0x3F_FFFF
        #expect(s.tcatProductId == 0x3FF)
        #expect(s.tcatSerial == 0x3F_FFFF)
    }
}

// MARK: - Active rate mode and peak clamping

struct DiceActiveRateModeTests {

    private func snapshot(nominalRateIndex: UInt32) -> DiceSnapshot {
        var s = DiceSnapshot()
        s.global = DiceGlobal.decode(
            DiceFixtures.place([(0x54, DiceFixtures.quad(nominalRateIndex << 8 | 1))],
                               size: 380),
            size: 380)
        return s
    }

    @Test(arguments: zip([UInt32(0), 1, 2, 3, 4, 5, 6],
                         [DiceRateMode.low, .low, .low, .middle, .middle, .high, .high]))
    func derivesRateModeFromNominalRate(index: UInt32, mode: DiceRateMode) {
        #expect(snapshot(nominalRateIndex: index).activeRateMode == mode)
    }

    @Test func reportsNoRateModeWhenTheClockIsUnlocked() {
        // CLOCK_RATE_NONE is index 10 (dice-interface.h:116).
        #expect(snapshot(nominalRateIndex: 10).activeRateMode == nil)
    }

    @Test func reportsNoRateModeWithoutAGlobalSection() {
        #expect(DiceSnapshot().activeRateMode == nil)
    }

    /// The peak section is sized by routerMaxEntries, so slots past the active
    /// router's entry count hold uninitialised data that reads as plausible
    /// routes. They must not be presented as measurements.
    @Test func clampsPeakEntriesToTheActiveRouter() {
        var s = snapshot(nominalRateIndex: 2)   // low
        s.eapPeak = (0..<128).map { _ in DiceRouterEntry.decode(0) }
        s.eapCurrentRouters[.low] = (0..<48).map { _ in DiceRouterEntry.decode(0) }
        #expect(s.meaningfulPeakCount == 48)
    }

    @Test func doesNotClampWhenTheActiveRouterIsUnknown() {
        var s = snapshot(nominalRateIndex: 2)
        s.eapPeak = (0..<128).map { _ in DiceRouterEntry.decode(0) }
        #expect(s.meaningfulPeakCount == 128)
    }

    @Test func neverExceedsThePeakEntriesActuallyRead() {
        var s = snapshot(nominalRateIndex: 2)
        s.eapPeak = (0..<10).map { _ in DiceRouterEntry.decode(0) }
        s.eapCurrentRouters[.low] = (0..<48).map { _ in DiceRouterEntry.decode(0) }
        #expect(s.meaningfulPeakCount == 10)
    }
}
