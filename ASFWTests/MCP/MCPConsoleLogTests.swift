import Testing
@testable import ASFW

// The console mirror must render deterministic single-line JSON: stable key
// order (log diffing), bounded length (no console floods), and byte arrays
// collapsed to quadlet-grouped hex so payloads are decodable at a glance.
struct MCPConsoleLogTests {
    @Test
    func rendersSortedSingleLineJSON() {
        let value = ASFWMCPValue.object([
            "generation": .int(3),
            "address": .string("0xFFFFF0000984"),
            "verify": .bool(true),
            "extra": .null,
        ])
        #expect(
            ASFWMCPConsoleLog.compactJSON(value, limit: 300) ==
            "{\"address\":\"0xFFFFF0000984\",\"extra\":null,\"generation\":3,\"verify\":true}"
        )
    }

    @Test
    func escapesQuotesAndBackslashes() {
        let value = ASFWMCPValue.object(["reason": .string("say \"hi\" \\ done")])
        #expect(
            ASFWMCPConsoleLog.compactJSON(value, limit: 300) ==
            "{\"reason\":\"say \\\"hi\\\" \\\\ done\"}"
        )
    }

    @Test
    func truncatesAtLimit() {
        let value = ASFWMCPValue.string(String(repeating: "a", count: 50))
        let rendered = ASFWMCPConsoleLog.compactJSON(value, limit: 10)
        #expect(rendered.count == 11)
        #expect(rendered.hasSuffix("…"))
    }

    @Test
    func collapsesByteArraysToQuadletGroupedHex() {
        let cipQuadlets: [ASFWMCPValue] = [0x00, 0x09, 0x00, 0xC8, 0x90, 0x02, 0xA6, 0xB0].map { .int($0) }
        let value = ASFWMCPValue.object(["payload": .array(cipQuadlets)])
        #expect(
            ASFWMCPConsoleLog.compactJSON(value, limit: 300) ==
            "{\"payload\":\"hex:000900C8 9002A6B0\"}"
        )
    }

    @Test
    func keepsShortAndNonByteArraysAsJSON() {
        let short = ASFWMCPValue.array([.int(1), .int(2)])
        #expect(ASFWMCPConsoleLog.compactJSON(short, limit: 300) == "[1,2]")

        let nonByte = ASFWMCPValue.array((0..<8).map { _ in .int(300) })
        #expect(ASFWMCPConsoleLog.compactJSON(nonByte, limit: 300).hasPrefix("[300,"))
    }
}
