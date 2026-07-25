import Foundation

enum SBP2CommandDataDirection: UInt8, CaseIterable, Identifiable, Sendable {
    case none = 0
    case fromTarget = 1
    case toTarget = 2

    var id: UInt8 { rawValue }
}

enum SBP2TaskManagementFunction: UInt64, CaseIterable, Identifiable, Sendable {
    case abortTaskSet = 0x0C
    case logicalUnitReset = 0x0E
    case targetReset = 0x0F

    var id: UInt64 { rawValue }
}

enum SBP2LoginState: UInt8, Sendable {
    case idle = 0
    case loggingIn = 1
    case loggedIn = 2
    case reconnecting = 3
    case loggingOut = 4
    case suspended = 5
    case failed = 6
}

struct SBP2SessionState: Sendable {
    let loginState: SBP2LoginState
    let loginID: UInt16
    let generation: UInt16
    let lastError: Int32
    let reconnectPending: Bool
}

struct SBP2CommandRequest: Sendable {
    let cdb: [UInt8]
    let direction: SBP2CommandDataDirection
    let transferLength: UInt32
    let outgoingData: Data
    let timeoutMs: UInt32
    let captureSenseData: Bool

    init(cdb: [UInt8],
         direction: SBP2CommandDataDirection,
         transferLength: UInt32 = 0,
         outgoingData: Data = Data(),
         timeoutMs: UInt32 = 2_000,
         captureSenseData: Bool = false) {
        self.cdb = cdb
        self.direction = direction
        self.transferLength = transferLength
        self.outgoingData = outgoingData
        self.timeoutMs = timeoutMs
        self.captureSenseData = captureSenseData
    }
}

struct SBP2CommandResult: Sendable {
    let transportStatus: Int32
    let sbpStatus: UInt8
    let scsiStatus: UInt8?
    let payload: Data
    let senseData: Data

    var isSuccess: Bool {
        transportStatus == 0 && sbpStatus == 0 && (scsiStatus == nil || scsiStatus == 0)
    }
}

enum SBP2CommandWireError: Error, Equatable {
    case emptyCDB
    case cdbTooLong
    case transferTooLarge
    case invalidOutgoingPayload
    case invalidNoDataTransfer
    case truncatedResult
}

enum SBP2CommandWireCodec {
    static let requestHeaderSize = 20
    static let resultHeaderSize = 16
    static let maximumCDBLength = 16
    static let maximumTransferLength: UInt32 = 16 * 1024 * 1024
    static let maximumSenseLength = 256

    static func encode(_ request: SBP2CommandRequest) throws -> Data {
        try validate(request)

        var encoded = Data()
        encoded.reserveCapacity(requestHeaderSize + request.cdb.count + request.outgoingData.count)
        encoded.appendUInt32LE(UInt32(request.cdb.count))
        encoded.appendUInt32LE(request.transferLength)
        encoded.appendUInt32LE(UInt32(request.outgoingData.count))
        encoded.appendUInt32LE(request.timeoutMs)
        encoded.append(request.direction.rawValue)
        encoded.append(request.captureSenseData ? 1 : 0)
        encoded.append(contentsOf: [0, 0])
        encoded.append(contentsOf: request.cdb)
        encoded.append(request.outgoingData)
        return encoded
    }

    static func decodeResult(_ encoded: Data) throws -> SBP2CommandResult {
        guard encoded.count >= resultHeaderSize,
              let transportRaw = encoded.readUInt32LE(at: 0),
              let payloadLengthRaw = encoded.readUInt32LE(at: 8),
              let senseLengthRaw = encoded.readUInt32LE(at: 12) else {
            throw SBP2CommandWireError.truncatedResult
        }

        let payloadLength = Int(payloadLengthRaw)
        let senseLength = Int(senseLengthRaw)
        let payloadStart = resultHeaderSize
        let payloadEnd = payloadStart + payloadLength
        let senseEnd = payloadEnd + senseLength
        guard payloadEnd >= payloadStart,
              senseEnd >= payloadEnd,
              senseEnd <= encoded.count else {
            throw SBP2CommandWireError.truncatedResult
        }

        return SBP2CommandResult(
            transportStatus: Int32(bitPattern: transportRaw),
            sbpStatus: encoded[4],
            scsiStatus: encoded[5] == 0 ? nil : encoded[6],
            payload: encoded.subdata(in: payloadStart..<payloadEnd),
            senseData: encoded.subdata(in: payloadEnd..<senseEnd)
        )
    }

    static func resultCapacity(for request: SBP2CommandRequest) throws -> Int {
        try validate(request)
        return resultHeaderSize + Int(request.transferLength) + maximumSenseLength
    }

    private static func validate(_ request: SBP2CommandRequest) throws {
        guard !request.cdb.isEmpty else { throw SBP2CommandWireError.emptyCDB }
        guard request.cdb.count <= maximumCDBLength else {
            throw SBP2CommandWireError.cdbTooLong
        }
        guard request.transferLength <= maximumTransferLength else {
            throw SBP2CommandWireError.transferTooLarge
        }

        switch request.direction {
        case .none:
            guard request.transferLength == 0, request.outgoingData.isEmpty else {
                throw SBP2CommandWireError.invalidNoDataTransfer
            }
        case .fromTarget:
            guard request.outgoingData.isEmpty else {
                throw SBP2CommandWireError.invalidOutgoingPayload
            }
        case .toTarget:
            guard request.outgoingData.count == Int(request.transferLength) else {
                throw SBP2CommandWireError.invalidOutgoingPayload
            }
        }
    }
}

extension Data {
    mutating func appendUInt32LE(_ value: UInt32) {
        var raw = value.littleEndian
        Swift.withUnsafeBytes(of: &raw) { append(contentsOf: $0) }
    }

    mutating func appendInt32LE(_ value: Int32) {
        appendUInt32LE(UInt32(bitPattern: value))
    }

    func readUInt32LE(at offset: Int) -> UInt32? {
        guard offset >= 0, offset + 4 <= count else { return nil }
        var value: UInt32 = 0
        for index in 0..<4 {
            value |= UInt32(self[startIndex + offset + index]) << (index * 8)
        }
        return value
    }

    mutating func replaceUInt32LE(at offset: Int, with value: UInt32) {
        guard offset >= 0, offset + 4 <= count else { return }
        var replacement = Data()
        replacement.appendUInt32LE(value)
        replaceSubrange(offset..<(offset + 4), with: replacement)
    }
}
