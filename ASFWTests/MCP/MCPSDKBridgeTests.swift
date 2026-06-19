import MCP
import Testing
@testable import ASFW

struct MCPSDKBridgeTests {
    private func bridge(
        configuration: ASFWMCPRuntimeConfiguration = .readOnlyDeveloper,
        driver: MockASFWDriverControl = MockASFWDriverControl()
    ) -> ASFWMCPSDKBridge<MockASFWDriverControl> {
        ASFWMCPSDKBridge(core: ASFWMCPCore(configuration: configuration, driver: driver))
    }

    @Test func toolMetadataMapsCatalogDefinitionToSDKTool() async throws {
        let tools = await bridge().listTools()
        let readQuadlet = try #require(tools.first { $0.name == "asfw_read_quadlet" })

        #expect(readQuadlet.description == "Submit an async quadlet read.")
        #expect(readQuadlet.annotations.readOnlyHint == true)
        #expect(readQuadlet.annotations.idempotentHint == false)
        #expect(readQuadlet.annotations.destructiveHint == false)
        #expect(readQuadlet.annotations.openWorldHint == false)

        guard case .object(let schema) = readQuadlet.inputSchema else {
            Issue.record("Tool input schema should be an object.")
            return
        }
        #expect(schema["type"] == .string("object"))
    }

    @Test func resourceMetadataMapsToJSONResources() async throws {
        let resources = await bridge().listResources()
        let snapshot = try #require(resources.first { $0.uri == "asfw://telemetry/snapshot" })

        #expect(snapshot.name == "asfw://telemetry/snapshot")
        #expect(snapshot.mimeType == "application/json")
        #expect(snapshot.description == "Compact cross-system telemetry overview.")
    }

    @Test func sdkCallToolDelegatesToCoreDispatch() async throws {
        let result = await bridge().callTool(
            CallTool.Parameters(
                name: "asfw_read_quadlet",
                arguments: [
                    "nodeId": .int(1),
                    "generation": .int(17),
                    "addressHigh": .int(0xFFFF),
                    "addressLow": .int(0xF0000400)
                ]
            )
        )

        #expect(result.isError == false)
        guard case .object(let root)? = result.structuredContent,
              case .object(let data)? = root["data"] else {
            Issue.record("Tool result should include structured transaction data.")
            return
        }
        #expect(data["kind"] == .string("readQuadlet"))
        #expect(data["status"] == .string("ok"))
    }

    @Test func sdkCallToolPreservesPolicyRefusalAsErrorResult() async throws {
        let driver = MockASFWDriverControl()
        let result = await bridge(configuration: .readOnlyDeveloper, driver: driver).callTool(
            CallTool.Parameters(
                name: "asfw_write_quadlet",
                arguments: [
                    "nodeId": .int(1),
                    "generation": .int(17),
                    "addressHigh": .int(0xFFFF),
                    "addressLow": .int(0xF0000800),
                    "value": .int(1)
                ]
            )
        )

        #expect(result.isError == true)
        #expect(await driver.unexpectedWriteAttemptCount() == 0)
        guard case .object(let root)? = result.structuredContent,
              case .object(let data)? = root["data"],
              case .object(let policy)? = data["policy"] else {
            Issue.record("Policy refusal should be preserved in structured content.")
            return
        }
        #expect(policy["decision"] == .string("requiresDeveloperMode"))
    }

    @Test func sdkReadResourceReturnsJSONTextContent() async throws {
        let result = await bridge().readResource(uri: "asfw://telemetry/snapshot")
        let content = try #require(result.contents.first)

        #expect(content.uri == "asfw://telemetry/snapshot")
        #expect(content.mimeType == "application/json")
        #expect(content.text?.contains("\"schema\" : \"asfw.telemetry.snapshot.v1\"") == true)
    }
}
