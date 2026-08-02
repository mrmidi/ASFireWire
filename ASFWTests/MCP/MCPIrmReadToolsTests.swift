import Testing
@testable import ASFW

struct MCPIrmReadToolsTests {
    @Test func snapshotReadsAllThreeResourceCsrs() async throws {
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: MockASFWDriverControl())
        let result = await core.callTool(
            name: "asfw_irm_get_state",
            arguments: .object(["generation": .int(17)])
        )

        guard case .object(let data) = result.data else {
            Issue.record("Expected an IRM snapshot object.")
            return
        }
        #expect(result.ok)
        #expect(data["kind"] == .string("irmResourceSnapshot"))
        #expect(data["irmNodeId"] == .int(2))
        #expect(data["bandwidthAvailable"] == .uint64(0x1333))
        #expect(data["channelsAvailable31_0"] == .uint64(0xFFFF_FFFE))
        #expect(data["channelsAvailable63_32"] == .uint64(0xFFFF_FFFF))
        #expect(data["atomic"] == .bool(false))
    }

    @Test func staleSnapshotDoesNotReturnResourceData() async throws {
        let core = ASFWMCPCore(
            configuration: .readOnlyDeveloper,
            driver: MockASFWDriverControl(generation: 18)
        )
        let result = await core.callTool(
            name: "asfw_irm_get_channels",
            arguments: .object(["generation": .int(17)])
        )

        guard case .object(let data) = result.data else {
            Issue.record("Expected an IRM snapshot object.")
            return
        }
        #expect(result.ok == false)
        #expect(data["status"] == .string("staleGeneration"))
        #expect(data["bandwidthAvailable"] == .null)
    }
}
