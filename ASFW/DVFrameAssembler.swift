import Foundation

struct DVFrameAssemblyResult: Sendable {
    var completedFrame: Data?
    var droppedFrame = false
    var duplicateBlocks = 0
    var invalidBlocks = 0
}

/// Reassembles SD-DV DIF blocks by their on-wire identifiers. The placement
/// follows the behavioral mapping in libiec61883's DV frame receiver; a bitmap
/// additionally prevents a duplicate block from hiding a missing block.
struct DVFrameAssembler {
    static let sourcePacketBytes = 480
    static let difBlockBytes = 80
    static let blocksPerSequence = 150

    private(set) var isPAL: Bool?
    private var frame = Data()
    private var received: [Bool] = []
    private var receivedCount = 0
    private var active = false

    mutating func reset() {
        isPAL = nil
        frame.removeAll(keepingCapacity: false)
        received.removeAll(keepingCapacity: false)
        receivedCount = 0
        active = false
    }

    mutating func ingest(_ chunk: UnsafeRawBufferPointer) -> DVFrameAssemblyResult {
        var result = DVFrameAssemblyResult()
        guard chunk.count == Self.sourcePacketBytes else {
            result.invalidBlocks = 1
            return result
        }

        if isFrameStart(chunk) {
            result = finishCurrentFrame()
            let pal = (chunk[3] & 0x80) != 0
            beginFrame(pal: pal)
        }

        guard active else { return result }

        for offset in stride(from: 0,
                             to: Self.sourcePacketBytes,
                             by: Self.difBlockBytes) {
            guard let index = frameBlockIndex(chunk: chunk, offset: offset),
                  index < received.count else {
                result.invalidBlocks += 1
                continue
            }

            if received[index] {
                result.duplicateBlocks += 1
                continue
            }

            let destination = index * Self.difBlockBytes
            frame.replaceSubrange(destination..<(destination + Self.difBlockBytes),
                                  with: chunk[offset..<(offset + Self.difBlockBytes)])
            received[index] = true
            receivedCount += 1
        }

        return result
    }

    mutating func finish() -> DVFrameAssemblyResult {
        let result = finishCurrentFrame()
        reset()
        return result
    }

    private mutating func beginFrame(pal: Bool) {
        isPAL = pal
        let sequences = pal ? 12 : 10
        let blockCount = sequences * Self.blocksPerSequence
        frame = Data(repeating: 0,
                     count: blockCount * Self.difBlockBytes)
        received = [Bool](repeating: false, count: blockCount)
        receivedCount = 0
        active = true
    }

    private mutating func finishCurrentFrame() -> DVFrameAssemblyResult {
        guard active else { return DVFrameAssemblyResult() }
        let complete = receivedCount == received.count &&
                       received.allSatisfy { $0 }
        let result = DVFrameAssemblyResult(
            completedFrame: complete ? frame : nil,
            droppedFrame: !complete)
        active = false
        return result
    }

    private func isFrameStart(_ chunk: UnsafeRawBufferPointer) -> Bool {
        guard chunk.count >= 4 else { return false }
        return (chunk[0] >> 5) == 0 &&
               (chunk[1] >> 4) == 0 &&
               chunk[2] == 0
    }

    private func frameBlockIndex(chunk: UnsafeRawBufferPointer,
                                 offset: Int) -> Int? {
        let section = Int(chunk[offset] >> 5)
        let sequence = Int(chunk[offset + 1] >> 4)
        let block = Int(chunk[offset + 2])
        let sequenceCount = isPAL == true ? 12 : 10
        guard sequence < sequenceCount else { return nil }

        let withinSequence: Int
        switch section {
        case 0 where block == 0:
            withinSequence = 0
        case 1 where block < 2:
            withinSequence = 1 + block
        case 2 where block < 3:
            withinSequence = 3 + block
        case 3 where block < 9:
            withinSequence = 6 + block * 16
        case 4 where block < 135:
            withinSequence = 7 + (block / 15) + block
        default:
            return nil
        }

        guard withinSequence < Self.blocksPerSequence else { return nil }
        return sequence * Self.blocksPerSequence + withinSequence
    }
}
