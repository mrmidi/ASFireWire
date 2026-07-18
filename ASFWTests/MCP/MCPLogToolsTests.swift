import Testing
@testable import ASFW

struct MCPLogToolsTests {
    @Test func queryFiltersByCategorySeverityAndSubstring() async {
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: MockASFWDriverControl())
        let result = await core.callTool(
            name: "asfw_log_query",
            arguments: .object([
                "categories": .array([.string("CMP")]),
                "maxLevel": .string("notice"),
                "contains": .string("iPCR"),
                "maxRecords": .int(20),
            ])
        )

        guard case .object(let data) = result.data,
              case .array(let records)? = data["records"],
              case .object(let record)? = records.first else {
            Issue.record("Expected a structured log query result.")
            return
        }
        #expect(result.ok)
        #expect(records.count == 1)
        #expect(record["category"] == .string("CMP"))
        #expect(record["level"] == .string("notice"))
        #expect(record["message"] == .string("[CMP] iPCR connected channel=5"))
        #expect(data["nextSequence"] == .uint64(40))
        #expect(data["cursorReset"] == .bool(false))
    }

    @Test func queryRejectsUnknownCategoryAndOversizedNeedle() async {
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: MockASFWDriverControl())
        let unknown = await core.callTool(
            name: "asfw_log_query",
            arguments: .object(["categories": .array([.string("BeBoB")])])
        )
        #expect(unknown.ok == false)
        #expect(unknown.errors.first?.code == .malformedRequest)

        let oversized = await core.callTool(
            name: "asfw_log_query",
            arguments: .object(["contains": .string(String(repeating: "x", count: 48))])
        )
        #expect(oversized.ok == false)
        #expect(oversized.errors.first?.code == .malformedRequest)
    }

    @Test func statsAreReadOnlyAndExposeLossAccounting() async {
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: MockASFWDriverControl())
        let result = await core.callTool(name: "asfw_log_stats")

        guard case .object(let data) = result.data else {
            Issue.record("Expected a structured log stats result.")
            return
        }
        #expect(result.ok)
        #expect(data["totalEmitted"] == .uint64(42))
        #expect(data["droppedRecords"] == .uint64(0))
        #expect(data["capacityRecords"] == .int(40_000))
    }
}
