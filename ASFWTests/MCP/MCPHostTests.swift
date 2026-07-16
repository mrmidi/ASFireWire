import Foundation
import Testing
@testable import ASFW

@MainActor
struct MCPHostTests {
    @Test func hostedStatefulHTTPInitializesSessionOverSSE() async throws {
        let driver = MockASFWDriverControl()
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver)
        let host = ASFWMCPHost(core: core)

        let status = try await host.start(configuration: ASFWMCPHostConfiguration(port: 0))
        do {
            let endpoint = try #require(status.endpointURL)
            #expect(status.isRunning == true)
            #expect(endpoint.host() == "127.0.0.1")

            var request = URLRequest(url: endpoint)
            request.httpMethod = "POST"
            request.addValue("application/json, text/event-stream", forHTTPHeaderField: "Accept")
            request.addValue("application/json", forHTTPHeaderField: "Content-Type")
            request.addValue("2025-11-25", forHTTPHeaderField: "MCP-Protocol-Version")
            request.httpBody = Data("""
            {
              "jsonrpc": "2.0",
              "id": 1,
              "method": "initialize",
              "params": {
                "protocolVersion": "2025-11-25",
                "capabilities": {},
                "clientInfo": {
                  "name": "ASFWTests",
                  "version": "1.0"
                }
              }
            }
            """.utf8)

            let (body, response) = try await URLSession.shared.data(for: request)
            let http = try #require(response as? HTTPURLResponse)
            let text = String(decoding: body, as: UTF8.self)

            #expect(http.statusCode == 200)
            #expect(http.value(forHTTPHeaderField: "Content-Type")?.contains("text/event-stream") == true)
            #expect(http.value(forHTTPHeaderField: "MCP-Session-Id")?.isEmpty == false)
            #expect(text.contains("event: message"))
            #expect(text.contains("\"protocolVersion\""))
        } catch {
            await host.stop()
            throw error
        }

        await host.stop()
        #expect(host.status == .stopped)
    }

    @Test func hostedEndpointRejectsUnexpectedPath() async throws {
        let driver = MockASFWDriverControl()
        let core = ASFWMCPCore(configuration: .readOnlyDeveloper, driver: driver)
        let host = ASFWMCPHost(core: core)

        let status = try await host.start(configuration: ASFWMCPHostConfiguration(port: 0))
        do {
            let endpoint = try #require(status.endpointURL)
            let badURL = endpoint.deletingLastPathComponent().appending(path: "nope")
            let (_, response) = try await URLSession.shared.data(from: badURL)
            let http = try #require(response as? HTTPURLResponse)

            #expect(http.statusCode == 404)
        } catch {
            await host.stop()
            throw error
        }

        await host.stop()
    }
}
