//
//  DiceFixtures.swift
//  ASFWTests
//
//  Byte-level fixtures for the DICE register decoders.
//
//  The golden fixture reproduces a Focusrite Saffire Pro 24 DSP as read from
//  real hardware on 2026-07-31, so the decoders are pinned against a device
//  rather than against my reading of the spec.
//

import Foundation
@testable import ASFW

enum DiceFixtures {

    // MARK: - Builders

    /// Big-endian quadlet, as it appears on the 1394 bus.
    static func quad(_ value: UInt32) -> Data {
        Data([UInt8((value >> 24) & 0xFF), UInt8((value >> 16) & 0xFF),
              UInt8((value >> 8) & 0xFF), UInt8(value & 0xFF)])
    }

    /// DICE text: the firmware byte-swaps every quadlet onto the bus, so a
    /// fixture must store each 4-byte group reversed for the decoder to
    /// reassemble it. Padded with NUL to `length`.
    static func text(_ string: String, length: Int) -> Data {
        var bytes = Array(string.utf8)
        while bytes.count % 4 != 0 { bytes.append(0) }
        var out = Data()
        for i in stride(from: 0, to: bytes.count, by: 4) {
            out.append(contentsOf: bytes[i..<(i + 4)].reversed())
        }
        if out.count < length {
            out.append(Data(repeating: 0, count: length - out.count))
        }
        return out.prefix(length)
    }

    /// A `\`-separated, `\\`-terminated name block.
    static func labels(_ names: [String], length: Int = 256) -> Data {
        text(names.joined(separator: "\\") + "\\\\", length: length)
    }

    static func zeros(_ count: Int) -> Data { Data(repeating: 0, count: count) }

    /// Concatenates quadlets. Long `+` chains of `Data` defeat the Swift
    /// type-checker, so fixtures build from an array instead.
    static func quads(_ values: [UInt32]) -> Data {
        var out = Data()
        out.reserveCapacity(values.count * 4)
        for v in values { out.append(quad(v)) }
        return out
    }

    /// Place `chunk` at `offset` inside a buffer of `size` bytes.
    static func place(_ pieces: [(Int, Data)], size: Int) -> Data {
        var buf = [UInt8](repeating: 0, count: size)
        for (offset, chunk) in pieces {
            guard offset >= 0, offset + chunk.count <= size else { continue }
            buf.replaceSubrange(offset..<(offset + chunk.count), with: chunk)
        }
        return Data(buf)
    }

    // MARK: - Golden device: Saffire Pro 24 DSP

    enum Saffire {
        static let guid: UInt64 = 0x0013_0E04_0200_4713

        /// Section table: global 0x00A/0x05F, tx 0x069/0x08E,
        /// rx 0x0F7/0x11A, ext_sync 0x211/0x004, unused2 0/0 (quadlets).
        static var generalSectionTable: Data {
            quads([0x00A, 0x05F,     // global
                   0x069, 0x08E,     // tx
                   0x0F7, 0x11A,     // rx
                   0x211, 0x004,     // ext_sync
                   0x000, 0x000])    // unused2
        }

        /// EAP section table, nine (offset, size) pairs in quadlets.
        static var extensionSectionTable: Data {
            quads([0x013, 0x0004,    // caps
                   0x017, 0x0002,    // cmd
                   0x019, 0x0121,    // mixer
                   0x13A, 0x0080,    // peak
                   0x1BA, 0x0081,    // router
                   0x23B, 0x010E,    // stream_format
                   0x349, 0x1800,    // current_config
                   0x1B49, 0x0010,   // standalone
                   0x1B59, 0x917C])  // application
        }

        static let clockSourceNames = [
            "AES1", "AES2", "SPDIF-OPT", "SPDIF", "AES_ANY", "ADAT", "ADAT_AUX",
            "Word Clock", "Unused", "Unused", "Unused", "Unused", "Internal",
        ]

        /// 380-byte global section as read from the device.
        static var globalSection: Data {
            place([
                (0x00, quad(0xFFC0_0001)),      // owner hi
                (0x04, quad(0x0000_0000)),      // owner lo
                (0x08, quad(0x0100_0000)),      // notification (vendor bit 24)
                (0x0C, text("Pro24DSP-004713", length: 64)),
                (0x4C, quad(0x0000_020C)),      // internal @ 48 kHz
                (0x50, quad(0x0000_0001)),      // enable
                (0x54, quad(0x0000_0201)),      // locked, nominal 48 kHz
                (0x58, quad(0x0000_0040)),      // arx1 locked, no slips
                (0x5C, quad(48_000)),
                (0x60, quad(0x0100_0C00)),      // version 1.0.12.0
                (0x64, quad(0x112C_001E)),      // caps
                (0x68, labels(clockSourceNames)),
            ], size: 380)
        }

        static let txNames = ["IP 1", "IP 2", "IP 3", "IP 4", "SPDIF L", "SPDIF R",
                              "ADAT 1", "ADAT 2", "ADAT 3", "ADAT 4", "ADAT 5",
                              "ADAT 6", "ADAT 7", "ADAT 8", "Loop 1", "Loop 2"]
        static let rxNames = ["Mon 1", "Mon 2", "Line 3", "Line 4", "Line 5",
                              "Line 6", "SPDIF L", "SPDIF R"]

        /// TX section: NUMBER 1, SIZE 70 quadlets. Note the 70-quadlet stream
        /// block ends at 0x118, exactly where AC3_CAPABILITIES would begin, so
        /// this device does not implement the AC3 registers.
        static var txSection: Data {
            place([
                (0x00, quad(1)),        // NUMBER
                (0x04, quad(70)),       // SIZE (quadlets)
                (0x08, quad(1)),        // ISOCHRONOUS
                (0x0C, quad(16)),       // NUMBER_AUDIO
                (0x10, quad(1)),        // NUMBER_MIDI
                (0x14, quad(2)),        // SPEED = S400
                (0x18, labels(txNames)),
            ], size: 568)
        }

        /// RX section: SEQ_START at 0x0C shifts the counts down one quadlet.
        static var rxSection: Data {
            place([
                (0x00, quad(1)),        // NUMBER
                (0x04, quad(70)),       // SIZE
                (0x08, quad(0)),        // ISOCHRONOUS
                (0x0C, quad(0)),        // SEQ_START
                (0x10, quad(8)),        // NUMBER_AUDIO
                (0x14, quad(1)),        // NUMBER_MIDI
                (0x18, labels(rxNames)),
            ], size: 1128)
        }

        static var extSyncSection: Data {
            quads([12, 1, 2, 0x10])
        }

        /// EAP caps: router 0x00800005, mixer 0x10120225, general 0x00011227.
        static var eapCaps: Data {
            quads([0x0080_0005, 0x1012_0225, 0x0001_1227, 0])
        }
    }
}
