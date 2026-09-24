import Foundation
import Testing
@testable import ASFW

struct MCPLogToolsTests {
    @Test func runningDriverVersionIsReadWithoutBusTraffic() async throws {
        var bytes = Data(repeating: 0, count: 280)
        bytes.replaceSubrange(0..<5, with: Data("0.3.0".utf8))
        bytes.replaceSubrange(32..<39, with: Data("bc12d10".utf8))
        bytes.replaceSubrange(40..<47, with: Data("bc12d10".utf8))
        bytes.replaceSubrange(81..<85, with: Data("test".utf8))
        let version = try #require(DriverVersionInfo(data: bytes))
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper,
                               driver: MockASFWDriverControl(driverVersion: version))

        let result = await core.callTool(name: "asfw_get_driver_version")
        guard case .object(let data) = result.data else {
            Issue.record("Expected running-driver metadata.")
            return
        }
        #expect(result.ok)
        #expect(data["gitCommitShort"] == .string("bc12d10"))
        #expect(data["gitBranch"] == .string("test"))
    }

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
