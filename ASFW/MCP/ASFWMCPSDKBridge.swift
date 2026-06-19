import Foundation
import MCP

// FW-95: Swift MCP SDK binding.
//
// This adapter is intentionally thin: ASFWMCPCore owns tool/resource semantics,
// policy, and driver access. The SDK bridge only translates ASFW's value model
// into MCP SDK declarations/results and registers handlers on an MCP Server.

struct ASFWMCPSDKBridge<Driver: ASFWDriverControlling> {
    let core: ASFWMCPCore<Driver>

    func registerHandlers(on server: Server) async {
        await server.withMethodHandler(ListTools.self) { _ in
            ListTools.Result(tools: await listTools())
        }

        await server.withMethodHandler(CallTool.self) { params in
            await callTool(params)
        }

        await server.withMethodHandler(ListResources.self) { _ in
            ListResources.Result(resources: await listResources())
        }

        await server.withMethodHandler(ReadResource.self) { params in
            await readResource(uri: params.uri)
        }
    }

    func listTools() async -> [Tool] {
        await core.listTools().map(\.mcpTool)
    }

    func listResources() async -> [Resource] {
        await core.listResources().map(\.mcpResource)
    }

    func callTool(_ params: CallTool.Parameters) async -> CallTool.Result {
        let arguments = params.arguments.map(ASFWMCPValue.init(mcpObject:)) ?? .object([:])
        let result = await core.callTool(name: params.name, arguments: arguments)
        let structuredContent = result.mcpValue
        let text = structuredContent.prettyJSONString ?? "\(structuredContent)"
        return CallTool.Result(
            content: [.text(text: text, annotations: nil, _meta: nil)],
            structuredContent: Optional<MCP.Value>.some(structuredContent),
            isError: result.ok == false
        )
    }

    func readResource(uri: String) async -> ReadResource.Result {
        let envelope = await core.readResource(uri: uri)
        let value = envelope.mcpValue
        let text = value.prettyJSONString ?? "\(value)"
        return ReadResource.Result(contents: [
            Resource.Content.text(text, uri: uri, mimeType: "application/json")
        ])
    }
}

extension ASFWMCPToolDefinition {
    var mcpTool: Tool {
        Tool(
            name: name,
            description: summary,
            inputSchema: .object([
                "type": .string("object"),
                "additionalProperties": .bool(true)
            ]),
            annotations: Tool.Annotations(
                readOnlyHint: readOnly,
                destructiveHint: readOnly ? false : true,
                idempotentHint: idempotent,
                openWorldHint: false
            )
        )
    }
}

extension ASFWMCPResourceDefinition {
    var mcpResource: Resource {
        Resource(
            name: uri,
            uri: uri,
            description: summary,
            mimeType: "application/json"
        )
    }
}

extension ASFWMCPValue {
    nonisolated init(mcpObject: [String: MCP.Value]) {
        self = .object(mcpObject.mapValues(ASFWMCPValue.init(mcpValue:)))
    }

    nonisolated init(mcpValue: MCP.Value) {
        switch mcpValue {
        case .null:
            self = .null
        case .bool(let value):
            self = .bool(value)
        case .int(let value):
            self = .int(value)
        case .double(let value):
            if value.isFinite,
               value.rounded(.towardZero) == value,
               value >= Double(Int.min),
               value <= Double(Int.max) {
                self = .int(Int(value))
            } else {
                self = .string(String(value))
            }
        case .string(let value):
            self = .string(value)
        case .data(_, let data):
            self = .array(data.map { .int(Int($0)) })
        case .array(let values):
            self = .array(values.map(ASFWMCPValue.init(mcpValue:)))
        case .object(let object):
            self = .object(object.mapValues(ASFWMCPValue.init(mcpValue:)))
        }
    }

    var mcpValue: MCP.Value {
        switch self {
        case .null:
            return .null
        case .bool(let value):
            return .bool(value)
        case .int(let value):
            return .int(value)
        case .uint64(let value):
            if value <= UInt64(Int.max) {
                return .int(Int(value))
            }
            return .string(String(value))
        case .string(let value):
            return .string(value)
        case .array(let values):
            return .array(values.map(\.mcpValue))
        case .object(let object):
            return .object(object.mapValues(\.mcpValue))
        }
    }
}

extension ASFWMCPToolCallResult {
    var mcpValue: MCP.Value {
        .object([
            "toolName": .string(toolName),
            "ok": .bool(ok),
            "data": data.mcpValue,
            "errors": .array(errors.map(\.mcpValue))
        ])
    }
}

extension ASFWMCPResourceError {
    var mcpValue: MCP.Value {
        .object([
            "code": .string(code.rawValue),
            "reason": .string(reason)
        ])
    }
}

extension ASFWMCPResourceEnvelope {
    var mcpValue: MCP.Value {
        .object([
            "schema": .string(schema),
            "uri": .string(uri),
            "snapshotId": .string(snapshotId),
            "capturedAt": capturedAt.map { .string($0.ISO8601Format()) } ?? .null,
            "monotonicNs": monotonicNs.map { ASFWMCPValue.uint64($0).mcpValue } ?? .null,
            "generation": generation.map { .int(Int($0)) } ?? .null,
            "driverConnected": .bool(driverConnected),
            "stale": .bool(stale),
            "truncated": .bool(truncated),
            "data": data.mcpValue,
            "links": .array(links.map { .string($0) }),
            "errors": .array(errors.map(\.mcpValue))
        ])
    }
}

private extension MCP.Value {
    var jsonCompatibleObject: Any {
        switch self {
        case .null:
            return NSNull()
        case .bool(let value):
            return value
        case .int(let value):
            return value
        case .double(let value):
            return value
        case .string(let value):
            return value
        case .data(_, let data):
            return data.map { Int($0) }
        case .array(let values):
            return values.map(\.jsonCompatibleObject)
        case .object(let object):
            return object.mapValues(\.jsonCompatibleObject)
        }
    }

    var prettyJSONString: String? {
        guard JSONSerialization.isValidJSONObject(jsonCompatibleObject),
              let data = try? JSONSerialization.data(
                withJSONObject: jsonCompatibleObject,
                options: [.prettyPrinted, .sortedKeys]
              ) else {
            return nil
        }
        return String(data: data, encoding: .utf8)
    }
}
