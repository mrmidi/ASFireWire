//
//  ASFWMCPDiceRegisterMapTests.swift
//  ASFWTests
//
//  Pins the Swift DICE register mirror against the driver's map.
//
//  The offsets in ASFWMCPDiceRegisterMap mirror
//  ASFWDriver/Audio/Protocols/DICE/Core/DICETypes.hpp because the driver and the
//  control plane are separate processes in separate languages. That mirror is
//  the only duplication in the DICE path, so it is pinned here: if DICETypes.hpp
//  changes, these fail and name what drifted.
//
//  Getting an offset wrong here does not crash — it silently reads the wrong
//  device register and reports a confident, wrong answer.
//

import XCTest
@testable import ASFW

final class ASFWMCPDiceRegisterMapTests: XCTestCase {

    // MARK: - Offsets mirrored from DICETypes.hpp

    func testGlobalOffsetsMatchDriverMap() {
        // DICETypes.hpp namespace GlobalOffset
        let expected: [String: UInt32] = [
            "GLOBAL_OWNER_HI": 0x00,
            "GLOBAL_OWNER_LO": 0x04,
            "GLOBAL_NOTIFICATION": 0x08,
            "GLOBAL_NICKNAME": 0x0C,
            "GLOBAL_CLOCK_SELECT": 0x4C,
            "GLOBAL_ENABLE": 0x50,
            "GLOBAL_STATUS": 0x54,
            "GLOBAL_EXTENDED_STATUS": 0x58,
            "GLOBAL_SAMPLE_RATE": 0x5C,
            "GLOBAL_VERSION": 0x60,
            "GLOBAL_CLOCK_CAPABILITIES": 0x64,
            "GLOBAL_CLOCK_SOURCE_NAMES": 0x68
        ]
        for (name, offset) in expected {
            let register = ASFWMCPDiceRegisterMap.register(named: name)
            XCTAssertNotNil(register, "\(name) missing from the map")
            XCTAssertEqual(register?.offset, offset, "\(name) offset drifted")
            XCTAssertEqual(register?.section, .global, "\(name) is in the global section")
        }
    }

    func testTxOffsetsMatchDriverMap() {
        // DICETypes.hpp namespace TxOffset. TX = device transmits = host capture.
        let expected: [String: UInt32] = [
            "TX_NUMBER": 0x00,
            "TX_SIZE": 0x04,
            "TX_ISOCHRONOUS": 0x08,
            "TX_NUMBER_AUDIO": 0x0C,
            "TX_NUMBER_MIDI": 0x10,
            "TX_SPEED": 0x14,
            "TX_NAMES": 0x18
        ]
        for (name, offset) in expected {
            let register = ASFWMCPDiceRegisterMap.register(named: name)
            XCTAssertEqual(register?.offset, offset, "\(name) offset drifted")
            XCTAssertEqual(register?.section, .txStreamFormat)
        }
    }

    func testRxOffsetsMatchDriverMap() {
        // DICETypes.hpp namespace RxOffset. RX = device receives = host playback.
        // Note RX inserts SEQ_START at 0x0C, so RX_NUMBER_AUDIO sits at 0x10
        // while TX_NUMBER_AUDIO sits at 0x0C. Transcribing one onto the other
        // reads the wrong field.
        let expected: [String: UInt32] = [
            "RX_NUMBER": 0x00,
            "RX_SIZE": 0x04,
            "RX_ISOCHRONOUS": 0x08,
            "RX_SEQ_START": 0x0C,
            "RX_NUMBER_AUDIO": 0x10,
            "RX_NUMBER_MIDI": 0x14,
            "RX_NAMES": 0x18
        ]
        for (name, offset) in expected {
            let register = ASFWMCPDiceRegisterMap.register(named: name)
            XCTAssertEqual(register?.offset, offset, "\(name) offset drifted")
            XCTAssertEqual(register?.section, .rxStreamFormat)
        }
    }

    func testTxAndRxAudioOffsetsDifferAsTheSpecRequires() {
        // Guards the specific confusion the naming invites.
        XCTAssertEqual(ASFWMCPDiceRegisterMap.register(named: "TX_NUMBER_AUDIO")?.offset, 0x0C)
        XCTAssertEqual(ASFWMCPDiceRegisterMap.register(named: "RX_NUMBER_AUDIO")?.offset, 0x10)
    }

    func testLookupIsCaseInsensitive() {
        XCTAssertEqual(ASFWMCPDiceRegisterMap.register(named: "rx_number_audio")?.name,
                       "RX_NUMBER_AUDIO")
        XCTAssertNil(ASFWMCPDiceRegisterMap.register(named: "NOT_A_REGISTER"))
    }

    // MARK: - Section table

    /// Five (offset, size) pairs of quadlet counts, big-endian.
    private func makeSectionTable(globalOffsetQuadlets: UInt32,
                                  txOffsetQuadlets: UInt32,
                                  rxOffsetQuadlets: UInt32) -> [UInt8] {
        var bytes: [UInt8] = []
        func append(_ value: UInt32) {
            bytes.append(UInt8((value >> 24) & 0xFF))
            bytes.append(UInt8((value >> 16) & 0xFF))
            bytes.append(UInt8((value >> 8) & 0xFF))
            bytes.append(UInt8(value & 0xFF))
        }
        append(globalOffsetQuadlets); append(0x20)
        append(txOffsetQuadlets); append(0x20)
        append(rxOffsetQuadlets); append(0x20)
        append(0x0300); append(0x10) // extSync
        append(0x0400); append(0x10) // reserved
        return bytes
    }

    func testSectionTableConvertsQuadletCountsToBytes() {
        let table = ASFWMCPDiceSectionTable.decode(
            makeSectionTable(globalOffsetQuadlets: 0x0A,
                             txOffsetQuadlets: 0x100,
                             rxOffsetQuadlets: 0x200)
        )
        XCTAssertNotNil(table)
        // Section::Deserialize multiplies the stored quadlet count by 4.
        XCTAssertEqual(table?.entry(for: .global)?.offsetBytes, 0x0A * 4)
        XCTAssertEqual(table?.entry(for: .txStreamFormat)?.offsetBytes, 0x100 * 4)
        XCTAssertEqual(table?.entry(for: .rxStreamFormat)?.offsetBytes, 0x200 * 4)
    }

    func testSectionTableRejectsShortPayload() {
        XCTAssertNil(ASFWMCPDiceSectionTable.decode([UInt8](repeating: 0, count: 16)))
    }

    func testSectionTableRejectsImplausibleOffsets() {
        // A non-DICE node would otherwise yield a confident address pointing
        // anywhere in its address space.
        let bogus = [UInt8](repeating: 0xFF, count: 40)
        XCTAssertNil(ASFWMCPDiceSectionTable.decode(bogus))
    }

    // MARK: - Address resolution

    func testPerStreamAddressAddsStreamStride() {
        let register = ASFWMCPDiceRegisterMap.register(named: "RX_NUMBER_AUDIO")!
        let sectionOffset: UInt32 = 0x800
        let strideBytes: UInt32 = 0x40 // 16 quadlets per stream config

        let stream0 = ASFWMCPDiceRegisterMap.addressLow(
            for: register, sectionOffsetBytes: sectionOffset,
            streamIndex: 0, streamConfigSizeBytes: strideBytes)
        let stream1 = ASFWMCPDiceRegisterMap.addressLow(
            for: register, sectionOffsetBytes: sectionOffset,
            streamIndex: 1, streamConfigSizeBytes: strideBytes)

        XCTAssertEqual(stream0, ASFWMCPDiceSpace.baseAddressLow + sectionOffset + 0x10)
        XCTAssertEqual(stream1, ASFWMCPDiceSpace.baseAddressLow + sectionOffset + 0x10 + strideBytes)
    }

    func testPerStreamAddressRequiresAKnownStride() {
        // Without the device's SIZE register there is no defensible stride, so
        // resolution must fail rather than assume one.
        let register = ASFWMCPDiceRegisterMap.register(named: "RX_NUMBER_AUDIO")!
        XCTAssertNil(ASFWMCPDiceRegisterMap.addressLow(
            for: register, sectionOffsetBytes: 0x800,
            streamIndex: 1, streamConfigSizeBytes: 0))
    }

    func testSectionScalarRejectsNonZeroStreamIndex() {
        // RX_NUMBER is one per section, not one per stream; a caller passing a
        // stream index has misunderstood and must be told, not quietly served.
        let register = ASFWMCPDiceRegisterMap.register(named: "RX_NUMBER")!
        XCTAssertNotNil(ASFWMCPDiceRegisterMap.addressLow(
            for: register, sectionOffsetBytes: 0x800,
            streamIndex: 0, streamConfigSizeBytes: 0))
        XCTAssertNil(ASFWMCPDiceRegisterMap.addressLow(
            for: register, sectionOffsetBytes: 0x800,
            streamIndex: 1, streamConfigSizeBytes: 0))
    }

    // MARK: - Decoders

    func testClockSelectDecodesSourceAndRate() {
        // ClockSelect: [rate index << 8 | source]. 0x0201 = 48 kHz, AES 2.
        let decoded = ASFWMCPDiceDecoder.decodeClockSelect(0x0000_0201)
        guard case let .object(fields) = decoded else { return XCTFail("expected object") }
        XCTAssertEqual(fields["sourceIndex"], .int(1))
        XCTAssertEqual(fields["sourceName"], .string("AES 2"))
        XCTAssertEqual(fields["rateHz"], .int(48_000))
    }

    func testClockSelectInternalSourceMatchesTcatNaming() {
        // Index 12 = Internal, matching ClockSourceIndexToDefaultName in the
        // TCAT reference driver.
        XCTAssertEqual(ASFWMCPDiceDecoder.clockSourceName(12), "Internal")
        XCTAssertEqual(ASFWMCPDiceDecoder.clockSourceName(8), "ARX 1")
        XCTAssertEqual(ASFWMCPDiceDecoder.clockSourceName(99), "Unknown")
    }

    func testStatusDecodesLockAndNominalRate() {
        // StatusBits: bit0 = source locked, [15:8] = nominal rate index.
        let decoded = ASFWMCPDiceDecoder.decodeStatus(0x0000_0101)
        guard case let .object(fields) = decoded else { return XCTFail("expected object") }
        XCTAssertEqual(fields["sourceLocked"], .bool(true))
        XCTAssertEqual(fields["nominalRateHz"], .int(44_100))
    }

    func testExtendedStatusSeparatesLockFromSlip() {
        // Lock flags occupy the low half; the matching slip flags sit 16 bits up.
        // ARX1 locked (0x40) and ADAT slipping (0x10 << 16).
        let decoded = ASFWMCPDiceDecoder.decodeExtendedStatus(0x0010_0040)
        guard case let .object(fields) = decoded,
              case let .object(locked) = fields["locked"],
              case let .object(slipping) = fields["slipping"] else {
            return XCTFail("expected nested objects")
        }
        XCTAssertEqual(locked["arx1"], .bool(true))
        XCTAssertEqual(locked["adat"], .bool(false))
        XCTAssertEqual(slipping["adat"], .bool(true))
        XCTAssertEqual(slipping["arx1"], .bool(false))
    }

    func testDecodeByNameCoversOnlyDefinedRegisters() {
        XCTAssertNotNil(ASFWMCPDiceDecoder.decode(registerName: "GLOBAL_STATUS", value: 0))
        XCTAssertNotNil(ASFWMCPDiceDecoder.decode(registerName: "global_clock_select", value: 0))
        // A register with no defined decoding must say so rather than invent one.
        XCTAssertNil(ASFWMCPDiceDecoder.decode(registerName: "RX_NUMBER_AUDIO", value: 4))
    }
}
