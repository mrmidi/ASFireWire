import Foundation
#if canImport(XCTest)
import XCTest

final class TestObserver: NSObject, XCTestObservation {
    func testBundleDidFinish(_ testBundle: Bundle) {
        // Exit with code 0 if all tests passed, 1 otherwise
        let failed = XCTestSuite.default.testRun?.hasSucceeded == false
        exit(failed ? 1 : 0)
    }
}

XCTestObservationCenter.shared.addTestObserver(TestObserver())

// MARK: - Tests
final class RomParserTests: XCTestCase {
    func testBusInfoParsingMinimum() throws {
        // Build a minimal synthetic ROM: header quadlet + 4 quadlets of bus info
        // Header: busInfoQuadlets=4, crcQuadlets=ignored, crc=dummy
        var rom = Data()
        // First quadlet: [busInfoQuadlets=4][crcQuadlets=0x00][crc=0x0000]
        rom.append(Data([0x04, 0x00, 0x00, 0x00]))
        // bus_name '1394'
        rom.append(Data([0x31, 0x33, 0x39, 0x34]))
        // meta1 (set irmc=1)
        rom.append(Data([0x80, 0x00, 0x00, 0x00]))
        // meta2: vendor id 0x0000ab, chip id high 0xcd
        rom.append(Data([0x00, 0x00, 0xab, 0xcd]))
        // meta3: chip id low 0x01234567
        rom.append(Data([0x01, 0x23, 0x45, 0x67]))
        // Root directory header: 0 quadlets
        rom.append(Data([0x00, 0x00, 0x00, 0x00]))

        let tmp = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("rom-test.bin")
        try? rom.write(to: tmp)

        let tree = try RomParser.parse(fileURL: tmp)
        XCTAssertEqual(tree.busInfo.irmc, 1)
        XCTAssertEqual(tree.busInfo.nodeVendorID, 0x0000ab)
        XCTAssertEqual(tree.busInfo.chipID, 0xcd01234567)
    }
}

#if !os(macOS)
// On non-macOS environments, skip running.
exit(0)
#endif

XCTMain([
    testCase([
        ("testBusInfoParsingMinimum", RomParserTests.testBusInfoParsingMinimum)
    ])
])
#else
// XCTest not available; do nothing so build passes
print("XCTest unavailable; skipping tests.")
#endif
