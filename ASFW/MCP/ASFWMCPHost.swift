import Foundation
import MCP
import Network

struct ASFWMCPHostConfiguration: Equatable, Sendable {
    var bindHost: String
    var port: UInt16
    var path: String

    nonisolated init(bindHost: String = "127.0.0.1", port: UInt16 = 8765, path: String = "/mcp") {
        self.bindHost = bindHost
        self.port = port
        self.path = path.hasPrefix("/") ? path : "/\(path)"
    }
}

struct ASFWMCPHostStatus: Equatable, Sendable {
    var isRunning: Bool
    var endpointURL: URL?
    var activeHTTPConnections: Int

    static let stopped = ASFWMCPHostStatus(
        isRunning: false,
        endpointURL: nil,
        activeHTTPConnections: 0
    )
}

enum ASFWMCPHostError: Error, Equatable {
    case alreadyRunning
    case invalidPort(UInt16)
    case listenerFailed(String)
}

@MainActor
final class ASFWMCPHost<Driver: ASFWDriverControlling> {
    private let core: ASFWMCPCore<Driver>
    private var server: Server?
    private var transport: StatefulHTTPServerTransport?
    private var httpAdapter: ASFWMCPHTTPAdapter?
    private(set) var status: ASFWMCPHostStatus = .stopped
    var onStatusChanged: ((ASFWMCPHostStatus) -> Void)?

    init(core: ASFWMCPCore<Driver>) {
        self.core = core
    }

    func start() async throws -> ASFWMCPHostStatus {
        try await start(configuration: ASFWMCPHostConfiguration())
    }

    func start(configuration: ASFWMCPHostConfiguration) async throws -> ASFWMCPHostStatus {
        guard status.isRunning == false else {
            throw ASFWMCPHostError.alreadyRunning
        }

        let server = Server(
            name: "ASFW MCP Control Plane",
            version: "0.1.0",
            capabilities: Server.Capabilities(resources: .init(), tools: .init())
        )
        let transport = StatefulHTTPServerTransport()
        await ASFWMCPSDKBridge(core: core).registerHandlers(on: server)
        try await server.start(transport: transport)

        let adapter = ASFWMCPHTTPAdapter(
            bindHost: configuration.bindHost,
            port: configuration.port,
            path: configuration.path,
            handler: { request in
                await transport.handleRequest(request)
            },
            connectionCountChanged: { [weak self] count in
                Task { @MainActor [weak self] in
                    self?.setActiveHTTPConnections(count)
                }
            }
        )

        let actualPort = try await adapter.start()
        let endpoint = URL(string: "http://\(configuration.bindHost):\(actualPort)\(configuration.path)")
        self.server = server
        self.transport = transport
        self.httpAdapter = adapter
        setStatus(ASFWMCPHostStatus(
            isRunning: true,
            endpointURL: endpoint,
            activeHTTPConnections: 0
        ))
        return status
    }

    func stop() async {
        await httpAdapter?.stop()
        await transport?.disconnect()
        await server?.stop()
        httpAdapter = nil
        transport = nil
        server = nil
        setStatus(.stopped)
    }

    private func setActiveHTTPConnections(_ count: Int) {
        var nextStatus = status
        nextStatus.activeHTTPConnections = count
        setStatus(nextStatus)
    }

    private func setStatus(_ status: ASFWMCPHostStatus) {
        self.status = status
        onStatusChanged?(status)
    }
}

private actor ASFWMCPHTTPAdapter {
    typealias Handler = @Sendable (HTTPRequest) async -> HTTPResponse
    typealias ConnectionCountChanged = @Sendable (Int) -> Void

    private let bindHost: String
    private let requestedPort: UInt16
    private let path: String
    private let handler: Handler
    private let connectionCountChanged: ConnectionCountChanged
    private var listener: NWListener?
    private var activeConnections: [ObjectIdentifier: NWConnection] = [:]
    private var readyContinuation: CheckedContinuation<UInt16, Error>?

    init(
        bindHost: String,
        port: UInt16,
        path: String,
        handler: @escaping Handler,
        connectionCountChanged: @escaping ConnectionCountChanged
    ) {
        self.bindHost = bindHost
        self.requestedPort = port
        self.path = path
        self.handler = handler
        self.connectionCountChanged = connectionCountChanged
    }

    func start() async throws -> UInt16 {
        guard listener == nil else {
            throw ASFWMCPHostError.alreadyRunning
        }
        guard let nwPort = NWEndpoint.Port(rawValue: requestedPort) else {
            throw ASFWMCPHostError.invalidPort(requestedPort)
        }

        let parameters = NWParameters.tcp
        parameters.allowLocalEndpointReuse = true
        if let ipv4 = IPv4Address(bindHost) {
            parameters.requiredLocalEndpoint = .hostPort(host: .ipv4(ipv4), port: nwPort)
        }
        let listener = try NWListener(using: parameters, on: nwPort)
        listener.service = nil
        listener.newConnectionHandler = { [weak self] connection in
            Task { await self?.accept(connection) }
        }
        listener.stateUpdateHandler = { [weak self] state in
            Task { await self?.listenerStateChanged(state) }
        }
        self.listener = listener
        listener.start(queue: .global(qos: .userInitiated))

        return try await withCheckedThrowingContinuation { continuation in
            self.readyContinuation = continuation
        }
    }

    func stop() async {
        listener?.cancel()
        listener = nil
        for connection in activeConnections.values {
            connection.cancel()
        }
        activeConnections.removeAll()
        connectionCountChanged(0)
    }

    private func listenerStateChanged(_ state: NWListener.State) {
        switch state {
        case .ready:
            let port = listener?.port?.rawValue ?? requestedPort
            readyContinuation?.resume(returning: port)
            readyContinuation = nil
        case .failed(let error):
            readyContinuation?.resume(throwing: ASFWMCPHostError.listenerFailed(error.localizedDescription))
            readyContinuation = nil
        default:
            break
        }
    }

    private func accept(_ connection: NWConnection) {
        let id = ObjectIdentifier(connection)
        activeConnections[id] = connection
        connectionCountChanged(activeConnections.count)
        connection.stateUpdateHandler = { [weak self, weak connection] state in
            guard case .cancelled = state, let connection else { return }
            Task { await self?.remove(connection) }
        }
        connection.start(queue: .global(qos: .userInitiated))

        Task {
            await handle(connection)
            await remove(connection)
        }
    }

    private func remove(_ connection: NWConnection) {
        activeConnections.removeValue(forKey: ObjectIdentifier(connection))
        connectionCountChanged(activeConnections.count)
    }

    private func handle(_ connection: NWConnection) async {
        do {
            let requestData = try await readHTTPRequest(from: connection)
            let parsed = try parseHTTPRequest(requestData)
            guard parsed.path == path else {
                try await sendResponse(.error(statusCode: 404, .invalidRequest("Not Found")), to: connection)
                connection.cancel()
                return
            }

            let response = await handler(parsed.request)
            try await sendResponse(response, to: connection)
        } catch {
            let message = error.localizedDescription
            try? await sendResponse(.error(statusCode: 400, .invalidRequest(message)), to: connection)
        }

        connection.cancel()
    }

    private func readHTTPRequest(from connection: NWConnection) async throws -> Data {
        var buffer = Data()
        var expectedLength: Int?

        while expectedLength == nil || buffer.count < expectedLength! {
            let chunk = try await receive(from: connection)
            guard chunk.isEmpty == false else { break }
            buffer.append(chunk)

            if expectedLength == nil,
               let headerRange = buffer.range(of: Data("\r\n\r\n".utf8)) {
                let headerLength = headerRange.upperBound
                let headerData = buffer[..<headerRange.lowerBound]
                let headerText = String(decoding: headerData, as: UTF8.self)
                let contentLength = Self.contentLength(in: headerText)
                expectedLength = headerLength + contentLength
            }
        }

        return buffer
    }

    private func receive(from connection: NWConnection) async throws -> Data {
        try await withCheckedThrowingContinuation { continuation in
            connection.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { data, _, isComplete, error in
                if let error {
                    continuation.resume(throwing: error)
                } else if let data {
                    continuation.resume(returning: data)
                } else if isComplete {
                    continuation.resume(returning: Data())
                } else {
                    continuation.resume(returning: Data())
                }
            }
        }
    }

    private func parseHTTPRequest(_ data: Data) throws -> (request: HTTPRequest, path: String) {
        guard let headerRange = data.range(of: Data("\r\n\r\n".utf8)) else {
            throw ASFWMCPHostError.listenerFailed("Missing HTTP header terminator.")
        }

        let headerText = String(decoding: data[..<headerRange.lowerBound], as: UTF8.self)
        let lines = headerText.split(separator: "\r\n", omittingEmptySubsequences: false)
        guard let requestLine = lines.first else {
            throw ASFWMCPHostError.listenerFailed("Missing HTTP request line.")
        }

        let requestParts = requestLine.split(separator: " ")
        guard requestParts.count >= 2 else {
            throw ASFWMCPHostError.listenerFailed("Malformed HTTP request line.")
        }

        let method = String(requestParts[0])
        let target = String(requestParts[1])
        let path = target.split(separator: "?", maxSplits: 1).first.map(String.init) ?? target
        var headers: [String: String] = [:]
        for line in lines.dropFirst() {
            guard let separator = line.firstIndex(of: ":") else { continue }
            let name = String(line[..<separator])
            let value = line[line.index(after: separator)...].trimmingCharacters(in: .whitespaces)
            headers[name] = value
        }

        let bodyStart = headerRange.upperBound
        let body = bodyStart < data.endIndex ? Data(data[bodyStart...]) : nil
        return (
            HTTPRequest(method: method, headers: headers, body: body, path: path),
            path
        )
    }

    private func sendResponse(_ response: HTTPResponse, to connection: NWConnection) async throws {
        switch response {
        case .stream(let stream, let headers):
            var responseHeaders = headers
            responseHeaders["Transfer-Encoding"] = "chunked"
            responseHeaders["Connection"] = responseHeaders["Connection"] ?? "close"
            try await sendHeader(statusCode: response.statusCode, headers: responseHeaders, to: connection)
            for try await chunk in stream {
                try await sendChunk(chunk, to: connection)
            }
            try await send(Data("0\r\n\r\n".utf8), to: connection)
        default:
            let body = response.bodyData ?? Data()
            var headers = response.headers
            headers["Content-Length"] = "\(body.count)"
            headers["Connection"] = "close"
            try await sendHeader(statusCode: response.statusCode, headers: headers, to: connection)
            if body.isEmpty == false {
                try await send(body, to: connection)
            }
        }
    }

    private func sendHeader(statusCode: Int, headers: [String: String], to connection: NWConnection) async throws {
        var header = "HTTP/1.1 \(statusCode) \(Self.reasonPhrase(for: statusCode))\r\n"
        for (name, value) in headers.sorted(by: { $0.key < $1.key }) {
            header += "\(name): \(value)\r\n"
        }
        header += "\r\n"
        try await send(Data(header.utf8), to: connection)
    }

    private func sendChunk(_ data: Data, to connection: NWConnection) async throws {
        try await send(Data(String(data.count, radix: 16).utf8), to: connection)
        try await send(Data("\r\n".utf8), to: connection)
        try await send(data, to: connection)
        try await send(Data("\r\n".utf8), to: connection)
    }

    private func send(_ data: Data, to connection: NWConnection) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            connection.send(content: data, completion: .contentProcessed { error in
                if let error {
                    continuation.resume(throwing: error)
                } else {
                    continuation.resume()
                }
            })
        }
    }

    private static func contentLength(in headerText: String) -> Int {
        for line in headerText.split(separator: "\r\n") {
            let parts = line.split(separator: ":", maxSplits: 1)
            guard parts.count == 2, parts[0].lowercased() == "content-length" else {
                continue
            }
            return Int(parts[1].trimmingCharacters(in: .whitespaces)) ?? 0
        }
        return 0
    }

    private static func reasonPhrase(for statusCode: Int) -> String {
        switch statusCode {
        case 200: "OK"
        case 202: "Accepted"
        case 400: "Bad Request"
        case 404: "Not Found"
        case 405: "Method Not Allowed"
        case 409: "Conflict"
        case 500: "Internal Server Error"
        default: "HTTP"
        }
    }
}
