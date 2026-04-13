import Foundation
import Testing
@testable import ASFW

struct DuetCodecTests {
    private func makeStatusResponse(code: DuetVendorCommandCode,
                                    index: UInt8? = nil,
                                    index2: UInt8? = nil,
                                    payload: [UInt8]) -> Data {
        let args = DuetVendorCodec.resolveArgs(code: code, index: index, index2: index2)
        var data = Data()
        data.append(0x0C) // IMPLEMENTED/STABLE
        data.append(DuetVendorWireConstants.subunitUnit)
        data.append(DuetVendorWireConstants.opcodeVendorDependent)
        data.append(contentsOf: DuetVendorWireConstants.oui)
        data.append(contentsOf: DuetVendorWireConstants.prefix)
        data.append(code.rawValue)
        data.append(args.arg1)
        data.append(args.arg2)
        data.append(contentsOf: payload)

        let padded = (data.count + 3) & ~3
        if padded > data.count {
            data.append(contentsOf: Array(repeating: 0, count: padded - data.count))
        }

        return data
    }

    @Test func buildsIndexedBoolStatusFrame() {
        let frame = DuetVendorCodec.buildFrame(isStatus: true,
                                               code: .xlrIsConsumerLevel,
                                               index: 1,
                                               controlPayload: [])
        #expect(frame != nil)

        guard let frame else { return }
        #expect(frame.count == 12)
        #expect(frame[0] == 0x01)
        #expect(frame[1] == 0xFF)
        #expect(frame[2] == 0x00)
        #expect(frame[9] == DuetVendorCommandCode.xlrIsConsumerLevel.rawValue)
        #expect(frame[10] == 0x80)
        #expect(frame[11] == 0x01)
    }

    @Test func buildsMixerControlFrameWithBigEndianU16() {
        let frame = DuetVendorCodec.buildFrame(isStatus: false,
                                               code: .mixerSrc,
                                               index: 3,
                                               index2: 1,
                                               controlPayload: [0x12, 0x34])
        #expect(frame != nil)

        guard let frame else { return }
        #expect(frame.count == 16)
        #expect(frame[9] == DuetVendorCommandCode.mixerSrc.rawValue)
        #expect(frame[10] == 0x11) // source=3 -> 0x11
        #expect(frame[11] == 0x01)
        #expect(frame[12] == 0x12)
        #expect(frame[13] == 0x34)
    }

    @Test func parsesInputAndClicklessStatusResponses() {
        let gainResponse = makeStatusResponse(code: .inGain, index: 0, payload: [0x2A])
        let clicklessResponse = makeStatusResponse(code: .inClickless, payload: [DuetVendorWireConstants.boolOn])

        let gainPayload = DuetVendorCodec.parseStatusPayload(gainResponse,
                                                             expectedCode: .inGain,
                                                             expectedIndex: 0)
        let clicklessPayload = DuetVendorCodec.parseStatusPayload(clicklessResponse,
                                                                  expectedCode: .inClickless)

        #expect(gainPayload != nil)
        #expect(clicklessPayload != nil)
        #expect(gainPayload?.first == 0x2A)
        #expect(clicklessPayload?.first == DuetVendorWireConstants.boolOn)
    }

    @Test func parsesMixerAndDisplayStatusResponses() {
        let mixerResponse = makeStatusResponse(code: .mixerSrc, index: 2, index2: 1, payload: [0x3F, 0x00])
        let displayResponse = makeStatusResponse(code: .displayIsInput, payload: [DuetVendorWireConstants.boolOff])

        let mixerPayload = DuetVendorCodec.parseStatusPayload(mixerResponse,
                                                              expectedCode: .mixerSrc,
                                                              expectedIndex: 2,
                                                              expectedIndex2: 1)
        let displayPayload = DuetVendorCodec.parseStatusPayload(displayResponse,
                                                                expectedCode: .displayIsInput)

        #expect(mixerPayload != nil)
        #expect(displayPayload != nil)

        if let mixerPayload {
            #expect(mixerPayload.count >= 2)
            let gain = (UInt16(mixerPayload[0]) << 8) | UInt16(mixerPayload[1])
            #expect(gain == 0x3F00)
        }

        #expect(displayPayload?.first == DuetVendorWireConstants.boolOff)

        let mismatched = DuetVendorCodec.parseStatusPayload(mixerResponse,
                                                            expectedCode: .mixerSrc,
                                                            expectedIndex: 0,
                                                            expectedIndex2: 1)
        #expect(mismatched == nil)
    }
}
