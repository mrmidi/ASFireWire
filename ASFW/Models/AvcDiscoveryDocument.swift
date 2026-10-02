import Foundation

/// Any JSON value, kept whole so an exported report carries the driver's
/// discovery document unchanged (replay tools read it as-is).
indirect enum JSONValue: Codable, Sendable, Equatable {
    case null
    case bool(Bool)
    case number(Double)
    case string(String)
    case array([JSONValue])
    case object([String: JSONValue])

    init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() { self = .null }
        else if let value = try? container.decode(Bool.self) { self = .bool(value) }
        else if let value = try? container.decode(Double.self) { self = .number(value) }
        else if let value = try? container.decode(String.self) { self = .string(value) }
        else if let value = try? container.decode([JSONValue].self) { self = .array(value) }
        else { self = .object(try container.decode([String: JSONValue].self)) }
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .null: try container.encodeNil()
        case .bool(let value): try container.encode(value)
        case .number(let value): try container.encode(value)
        case .string(let value): try container.encode(value)
        case .array(let value): try container.encode(value)
        case .object(let value): try container.encode(value)
        }
    }
}

/// The driver's per-unit discovery document (UserClient/WireFormats/AVCDiscoveryDocument.hpp),
/// format "asfw.avc.discovery" version 1, and how it is read page by page.
enum AvcDiscoveryDocument {
    static let format = "asfw.avc.discovery"
    static let supportedVersion = 1
    /// A document larger than this is refused rather than assembled.
    static let maxBytes = 1024 * 1024

    struct PageHeader: Equatable {
        static let size = 32
        static let magic: UInt32 = 0x4444_5641 // "AVDD"
        var version: UInt16
        var session: UInt32
        var generation: UInt32
        var totalBytes: UInt32
        var offset: UInt32
        var length: UInt32
        var checksum: UInt32
    }

    enum AssemblyError: Error, Equatable {
        case unavailable, malformedPage, unsupportedVersion, tooLarge
        /// Pages of two different documents (a refresh or new traffic between reads).
        case mixedPages
        case checksumMismatch
    }

    static func parseHeader(_ page: Data) -> PageHeader? {
        let bytes = [UInt8](page)
        guard bytes.count >= PageHeader.size else { return nil }
        func u16(_ at: Int) -> UInt16 { UInt16(bytes[at]) | UInt16(bytes[at + 1]) << 8 }
        func u32(_ at: Int) -> UInt32 { UInt32(u16(at)) | UInt32(u16(at + 2)) << 16 }
        guard u32(0) == PageHeader.magic, u16(6) == UInt16(PageHeader.size) else { return nil }
        let header = PageHeader(version: u16(4), session: u32(8), generation: u32(12), totalBytes: u32(16),
                                offset: u32(20), length: u32(24), checksum: u32(28))
        guard Int(header.length) == bytes.count - PageHeader.size else { return nil }
        return header
    }

    static func fnv1a(_ data: Data) -> UInt32 {
        var hash: UInt32 = 0x811C_9DC5
        for byte in data { hash = (hash ^ UInt32(byte)) &* 0x0100_0193 }
        return hash
    }

    /// Read pages from offset 0 until the document is complete. Every page
    /// must agree on session, generation, length and checksum; the whole
    /// document must match the checksum.
    static func assemble(fetch: (UInt32) -> Data?) -> Result<Data, AssemblyError> {
        var document = Data()
        var first: PageHeader?
        // Each page carries at least one byte, so this bound covers maxBytes.
        for _ in 0...(maxBytes / (4096 - PageHeader.size) + 1) {
            guard let page = fetch(UInt32(document.count)) else { return .failure(.unavailable) }
            guard let header = parseHeader(page) else { return .failure(.malformedPage) }
            guard Int(header.version) == supportedVersion else { return .failure(.unsupportedVersion) }
            guard Int(header.totalBytes) <= maxBytes else { return .failure(.tooLarge) }
            if let first {
                guard header.session == first.session, header.generation == first.generation,
                      header.totalBytes == first.totalBytes, header.checksum == first.checksum else {
                    return .failure(.mixedPages)
                }
            } else {
                first = header
            }
            guard Int(header.offset) == document.count else { return .failure(.malformedPage) }
            document.append(page.dropFirst(PageHeader.size))
            if document.count >= Int(header.totalBytes) || header.length == 0 {
                guard document.count == Int(header.totalBytes) else { return .failure(.malformedPage) }
                guard fnv1a(document) == header.checksum else { return .failure(.checksumMismatch) }
                return .success(document)
            }
        }
        return .failure(.tooLarge)
    }

    /// The facts the text report shows; everything else stays in the JSON.
    struct Summary: Decodable, Sendable {
        struct Snapshot: Decodable, Sendable {
            struct Failure: Decodable, Sendable { var kind: String }
            var complete: Bool
            var cancelled: Bool
            var terminalError: Failure?
            var probeCount: Int?
            var failedProbes: [Failure.Probe]?
        }
        struct Stream: Decodable, Sendable {
            var channels: Int
            var dataBlockSize: Int
            var midi: Int
            var rate: Int
            var rates: [Int]
            var channelNames: [String]
        }
        struct Graph: Decodable, Sendable { var playback: Stream; var capture: Stream }
        struct Exchange: Decodable, Sendable { var elapsedUs: Int }
        struct Exchanges: Decodable, Sendable { var session: Int; var dropped: Int; var records: [Exchange] }
        var format: String
        var version: Int
        var session: Int
        var snapshot: Snapshot?
        var graph: Graph?
        var exchanges: Exchanges
    }

    static func summary(_ document: JSONValue) -> Summary? {
        guard let data = try? JSONEncoder().encode(document) else { return nil }
        return try? JSONDecoder().decode(Summary.self, from: data)
    }
}

extension AvcDiscoveryDocument.Summary.Snapshot.Failure {
    struct Probe: Decodable, Sendable { var opcode: Int; var error: AvcDiscoveryDocument.Summary.Snapshot.Failure }
}
