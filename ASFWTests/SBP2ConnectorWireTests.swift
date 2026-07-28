import Foundation
import Testing
@testable import ASFW

struct SBP2ConnectorWireTests {
    @Test func selectorValuesMatchCurrentDriverABI() {
        #expect(ASFWDriverConnector.Method.allocateAddressRange.rawValue == 46)
        #expect(ASFWDriverConnector.Method.writeLocalData.rawValue == 49)
        #expect(ASFWDriverConnector.Method.createSBP2Session.rawValue == 52)
        #expect(ASFWDriverConnector.Method.releaseSBP2Session.rawValue == 60)
        #expect(ASFWDriverConnector.Method.requestUserBusReset.rawValue == 61)
    }

    @Test func encodesCommandRequestUsingNativeLittleEndianHeader() throws {
        let request = SBP2CommandRequest(
            cdb: [0x2A, 0, 0, 0, 0, 1],
            direction: .toTarget,
            transferLength: 4,
            outgoingData: Data([1, 2, 3, 4]),
            timeoutMs: 2_500,
            captureSenseData: true
        )

        let encoded = try SBP2CommandWireCodec.encode(request)

        #expect(encoded.count == 30)
        #expect(encoded.readUInt32LE(at: 0) == 6)
        #expect(encoded.readUInt32LE(at: 4) == 4)
        #expect(encoded.readUInt32LE(at: 8) == 4)
        #expect(encoded.readUInt32LE(at: 12) == 2_500)
        #expect(encoded[16] == SBP2CommandDataDirection.toTarget.rawValue)
        #expect(encoded[17] == 1)
        #expect(encoded[18] == 0)
        #expect(encoded[19] == 0)
        #expect(Data(encoded[20..<26]) == Data(request.cdb))
        #expect(Data(encoded[26..<30]) == request.outgoingData)
    }

    @Test func rejectsDirectionAndPayloadMismatch() {
        let request = SBP2CommandRequest(
            cdb: [0x28],
            direction: .fromTarget,
            transferLength: 4,
            outgoingData: Data([1])
        )

        #expect(throws: SBP2CommandWireError.invalidOutgoingPayload) {
            try SBP2CommandWireCodec.encode(request)
        }
    }

    @Test func resultCapacityCoversMaximumPayloadAndSenseData() throws {
        let request = SBP2CommandRequest(
            cdb: [0x28],
            direction: .fromTarget,
            transferLength: SBP2CommandWireCodec.maximumTransferLength,
            captureSenseData: true
        )

        #expect(
            try SBP2CommandWireCodec.resultCapacity(for: request)
                == 16 + 16 * 1024 * 1024 + 256
        )
    }

    @Test func rejectsOversizedResultCapacityBeforeAllocating() {
        let request = SBP2CommandRequest(
            cdb: [0x28],
            direction: .fromTarget,
            transferLength: SBP2CommandWireCodec.maximumTransferLength + 1
        )

        #expect(throws: SBP2CommandWireError.transferTooLarge) {
            try SBP2CommandWireCodec.resultCapacity(for: request)
        }
    }

    @Test func decodesCommandResultIncludingSCSIStatusAndSense() throws {
        var encoded = Data()
        encoded.appendInt32LE(-7)
        encoded.append(contentsOf: [0x00, 0x01, 0x02, 0x00])
        encoded.appendUInt32LE(3)
        encoded.appendUInt32LE(2)
        encoded.append(contentsOf: [0x11, 0x22, 0x33, 0x70, 0x05])

        let result = try SBP2CommandWireCodec.decodeResult(encoded)

        #expect(result.transportStatus == -7)
        #expect(result.sbpStatus == 0)
        #expect(result.scsiStatus == 0x02)
        #expect(result.payload == Data([0x11, 0x22, 0x33]))
        #expect(result.senseData == Data([0x70, 0x05]))
    }

    @Test func omitsSCSIStatusWhenDriverMarksItInvalid() throws {
        var encoded = Data()
        encoded.appendInt32LE(0)
        encoded.append(contentsOf: [0x00, 0x00, 0x02, 0x00])
        encoded.appendUInt32LE(0)
        encoded.appendUInt32LE(0)

        let result = try SBP2CommandWireCodec.decodeResult(encoded)

        #expect(result.scsiStatus == nil)
        #expect(result.isSuccess)
    }

    @Test func rejectsTruncatedCommandResult() {
        var encoded = Data(repeating: 0, count: 16)
        encoded.replaceUInt32LE(at: 8, with: 8)

        #expect(throws: SBP2CommandWireError.truncatedResult) {
            try SBP2CommandWireCodec.decodeResult(encoded)
        }
    }
}
