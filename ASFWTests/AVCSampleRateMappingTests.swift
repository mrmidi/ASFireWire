
import Testing
@testable import ASFW
import Foundation

struct AVCSampleRateMappingTests {

    /// Helper to create a SupportedFormat with a given rate code
    private func makeFormat(rateCode: UInt8) -> ASFWDriverConnector.AVCMusicCapabilities.SupportedFormat {
        // SupportedFormat is nested in AVCMusicCapabilities
        // and initialized via `init(sampleRateCode:formatCode:channelCount:)` implicitly synthesized or explicit
        // Wait, the struct definition in ASFWDriverConnector.swift has memberwise initializer.
        // Let's rely on that.
        return ASFWDriverConnector.AVCMusicCapabilities.SupportedFormat(
            sampleRateCode: rateCode,
            formatCode: 0x06, // MBLA
            channelCount: 2
        )
    }

    @Test func testSampleRateMappings() {
        // Test Table from Implementation Plan
        let testCases: [(code: UInt8, expected: String)] = [
            (0x00, "22.05 kHz"),
            (0x01, "24 kHz"),
            (0x02, "32 kHz"),
            (0x03, "44.1 kHz"),
            (0x04, "48 kHz"),
            (0x05, "96 kHz"),
            (0x06, "176.4 kHz"),
            (0x07, "192 kHz"),
            (0x0A, "88.2 kHz"),
            (0x0F, "Don't Care"),
            (0xFF, "0xFF") // Default case fallback
        ]

        for testCase in testCases {
            let format = makeFormat(rateCode: testCase.code)
            #expect(format.sampleRateName == testCase.expected, 
                   "Expected code 0x\(String(format: "%02X", testCase.code)) to map to '\(testCase.expected)', but got '\(format.sampleRateName)'")
        }
    }
}
