import Foundation
import Testing
@testable import ASFW

/// A device may publish model_id only in a unit directory. Real hardware
/// evidence: RME Fireface 400/800, IEEE 1212 unit directory key 0x17, whose root
/// directory carries no model at all. The summary must fall back to the unit
/// directory rather than reporting that the device advertises no model.
struct RomSummaryTests {
    /// Identity fields taken from a real Fireface 800 Config ROM (vendor OUI
    /// 0x000A35, unit specifier 0x000A35, unit version 0x00000001, unit
    /// model_id 0x101800). The layout is synthetic: this is not a byte-for-byte
    /// capture, and the unit version is what tells a Fireface 800 from a
    /// Fireface 400.
    private func fireface800ROM() -> Data {
        let quadlets: [UInt32] = [
            0x0400_0000,  // q0 BIB header: info_length 4, so the root starts at q5
            0x3133_3934,  // q1 '1394'
            0xC000_8202,  // q2 bus options
            0x000A_3500,  // q3 GUID high: vendor OUI 0x000A35
            0xEFAF_CD8A,  // q4 GUID low
            0x0002_0000,  // q5 root directory: 2 entries
            0x0300_0A35,  // q6 vendor_id (immediate)
            0xD100_0001,  // q7 unit directory, target q8
            0x0003_0000,  // q8 unit directory: 3 entries
            0x1200_0A35,  // q9 specifier_id 0x000A35
            0x1300_0001,  // q10 version 0x00000001 (Fireface 800)
            0x1710_1800,  // q11 model_id 0x101800 (key 0x17, value 24-bit)
        ]
        return Self.data(fromQuadlets: quadlets)
    }

    /// Root: vendor_id and model_id. Unit directory: the same specifier, version
    /// and model triple as the Fireface above.
    private func rootModelROM() -> Data {
        let quadlets: [UInt32] = [
            0x0400_0000,  // q0 BIB header
            0x3133_3934,  // q1 '1394'
            0xC000_8202,  // q2 bus options
            0x000A_3500,  // q3 GUID high
            0xEFAF_CD8A,  // q4 GUID low
            0x0003_0000,  // q5 root directory: 3 entries
            0x0300_0A35,  // q6 vendor_id
            0x1700_1234,  // q7 root model_id 0x001234
            0xD100_0001,  // q8 unit directory, target q9
            0x0003_0000,  // q9 unit directory: 3 entries
            0x1200_0A35,  // q10 specifier_id
            0x1300_0001,  // q11 version
            0x1710_1800,  // q12 unit model_id 0x101800 (key 0x17, value 24-bit)
        ]
        return Self.data(fromQuadlets: quadlets)
    }

    private static func data(fromQuadlets quadlets: [UInt32]) -> Data {
        var data = Data()
        for quadlet in quadlets {
            data.append(UInt8((quadlet >> 24) & 0xff))
            data.append(UInt8((quadlet >> 16) & 0xff))
            data.append(UInt8((quadlet >> 8) & 0xff))
            data.append(UInt8(quadlet & 0xff))
        }
        return data
    }

    @Test func unitDirectoryModelIsReportedWhenTheRootHasNone() throws {
        let tree = try RomParser.parse(data: fireface800ROM())
        let summary = Summarizer.summarize(tree: tree)

        // The fixture parses cleanly, and the root really carries no model: this
        // is the case the summary used to report as no model at all.
        #expect(tree.diagnostics.isEmpty)
        #expect(tree.rootDirectoryStartQ == 5)
        #expect(tree.rootDirectory.contains { $0.keyId == KeyType.model.rawValue } == false)
        #expect(summary.vendorId == 0x000A35)

        #expect(summary.modelId == 0x101800)
        #expect(summary.modelIdSource == RomModelIdSource.unitDirectory)
        #expect(summary.units.count == 1)
        #expect(summary.units.first?.specifierId == 0x000A35)
        #expect(summary.units.first?.version == 0x00000001)
        #expect(summary.units.first?.modelId == 0x101800)

        // The modalias is what a user copies into a report, so it must carry the
        // unit model too.
        #expect(summary.modalias == "ieee1394:ven00000A35mo00101800sp00000A35ver00000001")
    }

    @Test func rootModelStillWinsOverTheUnitDirectory() throws {
        let tree = try RomParser.parse(data: rootModelROM())
        let summary = Summarizer.summarize(tree: tree)

        #expect(summary.modelId == 0x001234)
        #expect(summary.modelIdSource == RomModelIdSource.rootDirectory)
        #expect(summary.units.first?.modelId == 0x101800)
        #expect(summary.modalias == "ieee1394:ven00000A35mo00001234sp00000A35ver00000001")
    }

    @Test func aDeviceWithoutAnyModelReportsNone() throws {
        let tree = try RomParser.parse(data: Self.data(fromQuadlets: [
            0x0400_0000,  // q0 BIB header
            0x3133_3934,  // q1 '1394'
            0xC000_8202,  // q2 bus options
            0x000A_3500,  // q3 GUID high
            0xEFAF_CD8A,  // q4 GUID low
            0x0001_0000,  // q5 root directory: 1 entry
            0x0300_0A35,  // q6 vendor_id, no model anywhere
        ]))
        let summary = Summarizer.summarize(tree: tree)

        // Both are nil, which is what makes the view say "Unknown".
        #expect(summary.modelId == nil)
        #expect(summary.modelName == nil)
        #expect(summary.modelIdSource == nil)
        #expect(summary.units.isEmpty)
    }
}

/// The Raw tab's export exists so a report can carry the ROM itself instead of a
/// transcription of it. Both representations must rebuild the bytes that were
/// read from the device, or the export is worse than no export: it looks like
/// evidence while being a paraphrase.
struct RomRawExportTests {
    /// 56 bytes, the length a Fireface 800 publishes (14 quadlets). Bytes are
    /// distinct enough that a misaligned or big-endian-swapped read cannot pass
    /// the round trip by accident.
    private func firefaceLengthROM() -> Data {
        var data = Data()
        for index in 0..<56 {
            data.append(UInt8((index &* 7 &+ 3) & 0xff))
        }
        return data
    }

    @Test func theExportStatesItsSizeAndEveryQuadlet() {
        let data = firefaceLengthROM()
        let text = RomRawExport.text(for: data)

        #expect(text.hasPrefix("ASFW Config ROM\n"))
        #expect(text.contains("size: 56 bytes (14 quadlets)"))
        #expect(text.contains("quadlets (big-endian wire order):"))
        #expect(text.contains("hex dump:"))

        // A quadlet line is an index and a value. The section label above also
        // begins with a 'q', so the value is what makes a line data.
        let quadletLines = text.split(separator: "\n").filter { $0.hasPrefix("q") && $0.contains("0x") }
        #expect(quadletLines.count == 14)
    }

    /// The quadlet lines are the wire order, big-endian, one per quadlet: a
    /// reader must be able to rebuild the ROM from them alone.
    @Test func theQuadletLinesRebuildTheROM() {
        let data = firefaceLengthROM()
        let rebuilt = RomRawExport.text(for: data)
            .split(separator: "\n")
            .filter { $0.hasPrefix("q") }
            .compactMap { line in UInt32(String(line.suffix(10).dropFirst(2)), radix: 16) }
            .flatMap { quadlet -> [UInt8] in
                [UInt8((quadlet >> 24) & 0xff),
                 UInt8((quadlet >> 16) & 0xff),
                 UInt8((quadlet >> 8) & 0xff),
                 UInt8(quadlet & 0xff)]
            }
        #expect(rebuilt == Array(data))
    }

    /// The hex dump is the same bytes with offsets, so its slots must align on
    /// the device's bytes and stop at the real end of a short line.
    @Test func theHexDumpRebuildsTheROM() {
        let data = firefaceLengthROM()
        let dump = RomRawExport.hexDump(data)

        var rebuilt: [UInt8] = []
        for line in dump.split(separator: "\n") {
            guard let offset = Int(String(line.prefix(8)), radix: 16) else {
                Issue.record("hex dump line without an offset: \(line)")
                continue
            }
            // 16 slots per line, fewer on the last one; each slot is two hex
            // digits padded to three characters.
            let slots = min(16, data.count - offset)
            let region = line.dropFirst(10)
            for slot in 0..<slots {
                guard let start = region.index(region.startIndex, offsetBy: slot * 3, limitedBy: region.endIndex),
                      let end = region.index(start, offsetBy: 3, limitedBy: region.endIndex) else {
                    Issue.record("hex dump line \(offset) ends before slot \(slot)")
                    break
                }
                let digits = String(region[start..<end]).trimmingCharacters(in: .whitespaces)
                guard let byte = UInt8(digits, radix: 16) else {
                    Issue.record("slot \(slot) of line \(offset) is not a byte: '\(digits)'")
                    break
                }
                rebuilt.append(byte)
            }
        }
        #expect(rebuilt == Array(data))
    }

    /// Offsets are per line, so the dump stays readable for a ROM longer than
    /// one line, and a partial quadlet says so instead of printing half a word.
    @Test func offsetsAdvancePerLineAndAPartialQuadletIsTruncated() {
        let data = firefaceLengthROM()
        let dump = RomRawExport.hexDump(data)
        let offsets = dump.split(separator: "\n").compactMap { Int(String($0.prefix(8)), radix: 16) }
        #expect(offsets == [0, 16, 32, 48])

        let partial = Data([0x04, 0x04, 0x00, 0x00, 0x31, 0x33])
        let first = RomRawExport.quadletLine(index: 0, in: partial)
        #expect(first.hasPrefix("q0"))
        #expect(first.hasSuffix("0x04040000"))
        #expect(RomRawExport.quadletLine(index: 1, in: partial).contains("<truncated>"))
    }
}
