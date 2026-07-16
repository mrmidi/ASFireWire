import Foundation
import MCP
import Testing
@testable import ASFW

@MainActor
struct MCPHostedEndToEndTests {
    @Test func sdkClientListsToolsCallsReadToolAndReadsResourceOverHostedHTTP() async throws {
        let driver = MockASFWDriverControl()
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver)
        let host = ASFWMCPHost(core: core)
        let status = try await host.start(configuration: ASFWMCPHostConfiguration(port: 0))
        let client = Client(name: "ASFWTests", version: "1.0")

        do {
            let endpoint = try #require(status.endpointURL)
            let transport = HTTPClientTransport(endpoint: endpoint, streaming: false)
            let initialize = try await client.connect(transport: transport)
            #expect(initialize.serverInfo.name == "ASFW MCP Control Plane")

            let listedTools = try await client.listTools()
            #expect(listedTools.tools.contains { $0.name == "asfw_read_quadlet" })

            let readResult = try await client.callTool(
                name: "asfw_read_quadlet",
                arguments: [
                    "nodeId": .int(1),
                    "generation": .int(17),
                    "addressHigh": .int(0xFFFF),
                    "addressLow": .int(0xF0000400)
                ]
            )
            #expect(readResult.isError == false)
            let readText = try #require(readResult.content.compactMap(\.textContent).first)
            #expect(readText.contains("\"status\" : \"ok\""))
            #expect(readText.contains("\"kind\" : \"readQuadlet\""))

            let contents = try await client.readResource(uri: "asfw://telemetry/snapshot")
            let telemetryText = try #require(contents.first?.text)
            #expect(telemetryText.contains("\"schema\" : \"asfw.telemetry.snapshot.v1\""))
            #expect(telemetryText.contains("\"driverConnected\" : true"))

            let healthContents = try await client.readResource(uri: "asfw://control-plane/health")
            let healthText = try #require(healthContents.first?.text)
            #expect(healthText.contains("\"schema\" : \"asfw.control_plane.health.v1\""))
            #expect(healthText.contains("\"status\" : \"ready\""))
        } catch {
            await client.disconnect()
            await host.stop()
            throw error
        }

        await client.disconnect()
        await host.stop()
    }
}

private extension Tool.Content {
    var textContent: String? {
        if case .text(let text, _, _) = self {
            return text
        }
        return nil
    }
}
