import Foundation
import Testing
@testable import ASFW

struct AlesisModelsTests {
    @Test func diceSectionsParseQuadletOffsetsAndSizes() {
        var data = Data()
        appendBE32(0x10, to: &data)
        appendBE32(0x20, to: &data)
        appendBE32(0x30, to: &data)
        appendBE32(0x40, to: &data)
        appendBE32(0x50, to: &data)
        appendBE32(0x60, to: &data)
        appendBE32(0x70, to: &data)
        appendBE32(0x80, to: &data)
        appendBE32(0x90, to: &data)
        appendBE32(0xA0, to: &data)

        let sections = AlesisDiceSections.parse(data)

        #expect(sections?.global.offset == 0x40)
        #expect(sections?.global.size == 0x80)
        #expect(sections?.txStreamFormat.offset == 0xC0)
        #expect(sections?.rxStreamFormat.size == 0x180)
        #expect(sections?.extSync.offset == 0x1C0)
    }

    @Test func globalStateParsesClockAndRate() {
        var data = Data(repeating: 0, count: 0x68)
        writeString("MultiMix", at: 0x0C, length: 64, in: &data)
        writeBE32(0x0000000C, at: 0x4C, in: &data)
        writeBE32(0x00000001, at: 0x50, in: &data)
        writeBE32(0x00000201, at: 0x54, in: &data)
        writeBE32(0x00000000, at: 0x58, in: &data)
        writeBE32(48_000, at: 0x5C, in: &data)
        writeBE32(0x01020304, at: 0x60, in: &data)
        writeBE32(0x0000000F, at: 0x64, in: &data)

        let global = AlesisDiceGlobalState.parse(data)

        #expect(global?.nickname == "MultiMix")
        #expect(global?.enabled == true)
        #expect(global?.sourceLocked == true)
        #expect(global?.clockSourceName == "Internal")
        #expect(global?.nominalRateHz == 48_000)
        #expect(global?.sampleRate == 48_000)
    }

    @Test func txAndRxStreamLayoutsParseSeparately() {
        var tx = Data()
        appendBE32(1, to: &tx)
        appendBE32(4, to: &tx)
        appendBE32(7, to: &tx)
        appendBE32(12, to: &tx)
        appendBE32(1, to: &tx)
        appendBE32(2, to: &tx)

        var rx = Data()
        appendBE32(1, to: &rx)
        appendBE32(4, to: &rx)
        appendBE32(0xFFFF_FFFF, to: &rx)
        appendBE32(4, to: &rx)
        appendBE32(2, to: &rx)
        appendBE32(0, to: &rx)

        let txConfig = AlesisDiceStreamConfig.parse(tx, isRxLayout: false)
        let rxConfig = AlesisDiceStreamConfig.parse(rx, isRxLayout: true)

        #expect(txConfig?.streams.first?.isoChannel == 7)
        #expect(txConfig?.streams.first?.pcmChannels == 12)
        #expect(txConfig?.streams.first?.midiPorts == 1)
        #expect(txConfig?.streams.first?.speed == 2)
        #expect(txConfig?.activePcmChannels == 12)

        #expect(rxConfig?.streams.first?.isoChannel == -1)
        #expect(rxConfig?.streams.first?.isActive == false)
        #expect(rxConfig?.streams.first?.seqStart == 4)
        #expect(rxConfig?.streams.first?.pcmChannels == 2)
        #expect(rxConfig?.activePcmChannels == 0)
    }

    @Test func systemProfilerAlesisStatusParsesChannelShape() {
        let output = """
        Audio:

            Alesis MultiMix Firewire:

              Input Channels: 12
              Output Channels: 2
              Current SampleRate: 48000

            Mac Studio Speakers:

              Output Channels: 2
        """

        let status = AlesisSystemProfilerStatus.parse(output)

        #expect(status?.deviceName == "Alesis MultiMix Firewire")
        #expect(status?.inputChannels == 12)
        #expect(status?.outputChannels == 2)
        #expect(status?.sampleRate == 48_000)
    }

    @Test func publishedDiceStatusParsesAudioNubProperties() {
        let output = """
        +-o ASFWAudioNub  <class IOUserService>
          | {
          |   "ASFWDICEProtocol" = "TCAT DICE"
          |   "ASFWDICECapsSource" = "runtime-discovery"
          |   "ASFWDICERuntimeCapsValid" = Yes
          |   "ASFWDICEHostInputPcmChannels" = 12
          |   "ASFWDICEHostOutputPcmChannels" = 2
          |   "ASFWDICEDeviceToHostAm824Slots" = 12
          |   "ASFWDICEHostToDeviceAm824Slots" = 2
          |   "ASFWDICESampleRateHz" = 48000
          |   "ASFWDICEDeviceToHostIsoChannel" = 1
          |   "ASFWDICEHostToDeviceIsoChannel" = 0
          | }
        """

        let status = AlesisPublishedDiceStatus.parse(output)

        #expect(status?.protocolName == "TCAT DICE")
        #expect(status?.capsSource == "runtime-discovery")
        #expect(status?.channelSummary == "12 in / 2 out")
        #expect(status?.slotSummary == "12 capture / 2 playback")
        #expect(status?.isoSummary == "TX 1, RX 0")
        #expect(status?.sampleRateHz == 48_000)
    }

    private func appendBE32(_ value: UInt32, to data: inout Data) {
        data.append(UInt8((value >> 24) & 0xFF))
        data.append(UInt8((value >> 16) & 0xFF))
        data.append(UInt8((value >> 8) & 0xFF))
        data.append(UInt8(value & 0xFF))
    }

    private func writeBE32(_ value: UInt32, at offset: Int, in data: inout Data) {
        data[offset] = UInt8((value >> 24) & 0xFF)
        data[offset + 1] = UInt8((value >> 16) & 0xFF)
        data[offset + 2] = UInt8((value >> 8) & 0xFF)
        data[offset + 3] = UInt8(value & 0xFF)
    }

    private func writeString(_ string: String, at offset: Int, length: Int, in data: inout Data) {
        let bytes = Array(string.utf8.prefix(length))
        for index in 0..<bytes.count {
            data[offset + index] = bytes[index]
        }
    }
}
