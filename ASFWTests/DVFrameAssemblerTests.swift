import Foundation
import Testing
@testable import ASFW

struct DVFrameAssemblerTests {
    private func makeFrame(pal: Bool, marker: UInt8) -> Data {
        let sequences = pal ? 12 : 10
        var frame = Data(repeating: marker,
                         count: sequences * 150 * 80)

        for sequence in 0..<sequences {
            func setID(_ position: Int, section: UInt8, block: UInt8) {
                let offset = (sequence * 150 + position) * 80
                frame[offset] = section << 5
                frame[offset + 1] = UInt8(sequence << 4)
                frame[offset + 2] = block
            }

            setID(0, section: 0, block: 0)
            for block in 0..<2 {
                setID(1 + block, section: 1, block: UInt8(block))
            }
            for block in 0..<3 {
                setID(3 + block, section: 2, block: UInt8(block))
            }
            for block in 0..<9 {
                setID(6 + block * 16, section: 3, block: UInt8(block))
            }
            for block in 0..<135 {
                setID(7 + (block / 15) + block,
                      section: 4,
                      block: UInt8(block))
            }
        }
        frame[3] = pal ? 0x80 : 0x00
        return frame
    }

    private func ingest(_ data: Data,
                        into assembler: inout DVFrameAssembler)
        -> DVFrameAssemblyResult {
        data.withUnsafeBytes { assembler.ingest($0) }
    }

    @Test func reassemblesNTSCFrameByDifIdentity() {
        let frame = makeFrame(pal: false, marker: 0xA5)
        let next = makeFrame(pal: false, marker: 0x5A)
        var assembler = DVFrameAssembler()

        for offset in stride(from: 0, to: frame.count, by: 480) {
            _ = ingest(frame.subdata(in: offset..<(offset + 480)),
                       into: &assembler)
        }
        let result = ingest(next.subdata(in: 0..<480), into: &assembler)

        #expect(result.droppedFrame == false)
        #expect(result.completedFrame == frame)
        #expect(assembler.isPAL == false)
    }

    @Test func reassemblesPALFrameWhenPacketsArriveOutOfOrder() {
        let frame = makeFrame(pal: true, marker: 0x3C)
        let next = makeFrame(pal: true, marker: 0xC3)
        var packets: [Data] = []
        for offset in stride(from: 0, to: frame.count, by: 480) {
            packets.append(frame.subdata(in: offset..<(offset + 480)))
        }
        packets.swapAt(10, 11)

        var assembler = DVFrameAssembler()
        for packet in packets {
            _ = ingest(packet, into: &assembler)
        }
        let result = ingest(next.subdata(in: 0..<480), into: &assembler)

        #expect(result.completedFrame == frame)
        #expect(assembler.isPAL == true)
    }

    @Test func duplicateCannotHideMissingPacket() {
        let frame = makeFrame(pal: false, marker: 0x11)
        let next = makeFrame(pal: false, marker: 0x22)
        var packets: [Data] = []
        for offset in stride(from: 0, to: frame.count, by: 480) {
            packets.append(frame.subdata(in: offset..<(offset + 480)))
        }

        var assembler = DVFrameAssembler()
        for (index, packet) in packets.enumerated() where index != 12 {
            _ = ingest(packet, into: &assembler)
            if index == 13 {
                let duplicate = ingest(packet, into: &assembler)
                #expect(duplicate.duplicateBlocks == 6)
            }
        }
        let result = ingest(next.subdata(in: 0..<480), into: &assembler)

        #expect(result.completedFrame == nil)
        #expect(result.droppedFrame)
    }
}
