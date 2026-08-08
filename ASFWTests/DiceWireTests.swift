//
//  DiceWireTests.swift
//  ASFWTests
//
//  Primitives underneath every DICE decode: quadlet extraction, the firmware's
//  per-quadlet byte swap, and name-block splitting.
//

import Foundation
import Testing
@testable import ASFW

struct DiceWireQuadletTests {

    @Test func readsBigEndianQuadlet() {
        let d = Data([0x11, 0x22, 0x33, 0x44])
        #expect(DiceWire.u32(d, 0) == 0x1122_3344)
    }

    @Test func readsAtOffset() {
        let d = Data([0xAA, 0xBB, 0xCC, 0xDD, 0x01, 0x02, 0x03, 0x04])
        #expect(DiceWire.u32(d, 4) == 0x0102_0304)
    }

    @Test(arguments: [-4, -1, 5, 8, 100])
    func rejectsOutOfBoundsOffsets(offset: Int) {
        let d = Data([0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88])
        // 5 would run off the end of an 8-byte buffer (5 + 4 > 8).
        #expect(DiceWire.u32(d, offset) == nil)
    }

    /// `Data` slices keep the parent's indices. Indexing from zero rather than
    /// from `startIndex` would read the wrong bytes or trap.
    @Test func honoursSliceStartIndex() {
        let parent = Data([0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33, 0x44])
        let slice = Data(parent.dropFirst(4))
        #expect(DiceWire.u32(slice, 0) == 0x1122_3344)

        let rawSlice = parent.dropFirst(4)
        #expect(DiceWire.u32(rawSlice, 0) == 0x1122_3344)
    }
}

struct DiceWireTextTests {

    /// Hardware anchor: the first quadlet of the Saffire's CLOCK_SOURCE_NAMES
    /// block read back as 0x31534541, which is "AES1" once the quadlet is
    /// unswapped. This pins the byte order against a real device rather than
    /// against the fixture builder.
    @Test func unswapsQuadletsFromRealDevice() {
        let d = DiceFixtures.quad(0x3153_4541)
        #expect(DiceWire.text(d, 0, 4) == "AES1")
    }

    @Test func decodesMultipleQuadlets() {
        // "ABCD" "EFGH" stored byte-swapped per quadlet.
        let d = Data([0x44, 0x43, 0x42, 0x41, 0x48, 0x47, 0x46, 0x45])
        #expect(DiceWire.text(d, 0, 8) == "ABCDEFGH")
    }

    @Test func returnsEmptyWhenOutOfBounds() {
        #expect(DiceWire.text(Data([0x01, 0x02]), 0, 8).isEmpty)
    }

    @Test func fixtureBuilderRoundTrips() {
        let d = DiceFixtures.text("Pro24DSP-004713", length: 64)
        #expect(DiceWire.nulTerminated(d, 0, 64) == "Pro24DSP-004713")
    }

    @Test func nulTerminatedStopsAtFirstNul() {
        let d = DiceFixtures.text("abc", length: 16)
        #expect(DiceWire.nulTerminated(d, 0, 16) == "abc")
    }
}

struct DiceWireLabelTests {

    @Test func splitsOnSingleBackslashAndStopsAtDouble() {
        let d = DiceFixtures.labels(["one", "two", "three"])
        #expect(DiceWire.labels(d, 0, 256) == ["one", "two", "three"])
    }

    /// An empty name is not representable: joining it produces a doubled
    /// backslash, which *is* the list terminator. The list therefore ends
    /// there. This is why devices write "Unused" rather than leaving a name
    /// blank (cross-checked against tcat.rs:554-557, which stops at the first
    /// zero-length element).
    @Test func stopsAtAnEmptyNameBecauseItIsIndistinguishableFromTheTerminator() {
        let d = DiceFixtures.labels(["AES1", "", "ADAT"])
        #expect(DiceWire.labels(d, 0, 256) == ["AES1"])
    }

    @Test func dropsTrailingPadding() {
        let d = DiceFixtures.labels(["a", "b"])
        #expect(DiceWire.labels(d, 0, 256) == ["a", "b"])
    }

    @Test func decodesSaffireClockSourceNames() {
        let d = DiceFixtures.labels(DiceFixtures.Saffire.clockSourceNames)
        let names = DiceWire.labels(d, 0, 256)
        #expect(names.count == 13)
        #expect(names[0] == "AES1")
        #expect(names[2] == "SPDIF-OPT")
        #expect(names[8] == "Unused")   // arx1 has no user-facing name
        #expect(names[12] == "Internal")
    }
}
