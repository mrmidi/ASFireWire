import XCTest
@testable import ASFW

final class DARTCandidateSelectionTests: XCTestCase {
    private let candidates: [(name: String, id: UInt32)] = [
        ("dart-apciec1-piodma", 0x61),
        ("dart-apciec0", 0x55),
        ("dart-apciec0-piodma", 0x57),
        ("dart-disp0", 0x41),
    ]

    func testPrefersExactPiodmaMatchForComplex() {
        XCTAssertEqual(
            ASFWDriverConnector.selectDARTCandidate(complex: "apciec0", candidates: candidates),
            0x57)
    }

    func testFallsBackToSameComplexPrefix() {
        let withoutExact = candidates.filter { $0.name != "dart-apciec0-piodma" }
        XCTAssertEqual(
            ASFWDriverConnector.selectDARTCandidate(complex: "apciec0", candidates: withoutExact),
            0x55)
    }

    func testFallsBackToAnyDARTWhenComplexUnknown() {
        XCTAssertEqual(
            ASFWDriverConnector.selectDARTCandidate(complex: nil, candidates: candidates),
            0x61)
    }

    func testComplexPrefixDoesNotCrossNumberBoundary() {
        // apciec1 must not match dart-apciec10-* style names by accident;
        // exact piodma tier guards it.
        XCTAssertEqual(
            ASFWDriverConnector.selectDARTCandidate(complex: "apciec1", candidates: candidates),
            0x61)
    }

    func testEmptyCandidatesYieldNil() {
        XCTAssertNil(ASFWDriverConnector.selectDARTCandidate(complex: "apciec0", candidates: []))
    }
}
