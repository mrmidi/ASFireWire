import Foundation

enum AlesisConstants {
    static let vendorID: UInt32 = 0x000595
    static let multiMixModelID: UInt32 = 0x000000
    static let fireWireUnitSpecID: UInt32 = 0x000595
    static let fireWireUnitSoftwareVersion: UInt32 = 0x000001
    static let coreAudioDeviceName = "Alesis MultiMix Firewire"
    static let diceBaseAddress: UInt64 = 0xFFFFE0000000
}

struct AlesisCoreAudioStatus: Equatable {
    var deviceName: String
    var inputChannels: Int
    var outputChannels: Int
    var sampleRate: Double
    var bufferFrameSize: UInt32
    var bufferFrameSizeRange: ClosedRange<UInt32>?
    var inputLatency: UInt32
    var outputLatency: UInt32
    var inputSafetyOffset: UInt32
    var outputSafetyOffset: UInt32
    var isRunning: Bool
    var isDefaultInput: Bool
    var isDefaultOutput: Bool

    var channelSummary: String {
        "\(inputChannels) in / \(outputChannels) out"
    }

    var sampleRateSummary: String {
        sampleRate > 0 ? "\(Int(sampleRate)) Hz" : "Unknown"
    }

    var bufferSummary: String {
        if let range = bufferFrameSizeRange {
            return "\(bufferFrameSize) frames (\(range.lowerBound)-\(range.upperBound))"
        }
        return "\(bufferFrameSize) frames"
    }

    static func make(from device: AudioWrapperDevice) -> AlesisCoreAudioStatus {
        let range = device.bufferFrameSizeRange
        let frameRange: ClosedRange<UInt32>? = (range.min > 0 || range.max > 0) ? range.min...range.max : nil
        return AlesisCoreAudioStatus(
            deviceName: device.name,
            inputChannels: device.inputChannelCount,
            outputChannels: device.outputChannelCount,
            sampleRate: device.sampleRate,
            bufferFrameSize: device.bufferFrameSize,
            bufferFrameSizeRange: frameRange,
            inputLatency: device.inputDeviceLatency,
            outputLatency: device.outputDeviceLatency,
            inputSafetyOffset: device.inputSafetyOffset,
            outputSafetyOffset: device.outputSafetyOffset,
            isRunning: device.isRunning,
            isDefaultInput: device.isDefaultInput,
            isDefaultOutput: device.isDefaultOutput
        )
    }
}

struct AlesisSystemProfilerStatus: Equatable {
    var deviceName: String
    var inputChannels: Int?
    var outputChannels: Int?
    var sampleRate: Double?

    nonisolated static func parse(_ output: String,
                                  deviceName: String = AlesisConstants.coreAudioDeviceName) -> AlesisSystemProfilerStatus? {
        guard let nameRange = output.range(of: "\(deviceName):",
                                           options: [.caseInsensitive]) else {
            return nil
        }

        let tail = output[nameRange.upperBound...]
        let nextDevice = tail.range(of: "\n\\S[^\\n]*:\\s*$", options: .regularExpression)
        let block = String(tail[..<(nextDevice?.lowerBound ?? tail.endIndex)])

        return AlesisSystemProfilerStatus(
            deviceName: deviceName,
            inputChannels: firstInt(after: "Input Channels", in: block),
            outputChannels: firstInt(after: "Output Channels", in: block),
            sampleRate: firstDouble(after: "Current SampleRate", in: block)
        )
    }

    nonisolated private static func firstInt(after label: String, in block: String) -> Int? {
        firstDouble(after: label, in: block).map(Int.init)
    }

    nonisolated private static func firstDouble(after label: String, in block: String) -> Double? {
        let escaped = NSRegularExpression.escapedPattern(for: label)
        let pattern = "\(escaped)\\s*:\\s*([0-9]+(?:\\.[0-9]+)?)"
        guard let regex = try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive]) else {
            return nil
        }
        let range = NSRange(block.startIndex..<block.endIndex, in: block)
        guard let match = regex.firstMatch(in: block, range: range),
              match.numberOfRanges > 1,
              let valueRange = Range(match.range(at: 1), in: block) else {
            return nil
        }
        return Double(block[valueRange])
    }
}

struct AlesisDiscoveredIdentity: Equatable {
    var guid: UInt64
    var nodeID: UInt8
    var generation: UInt32
    var vendorID: UInt32
    var modelID: UInt32
    var vendorName: String
    var modelName: String
    var unitCount: Int

    var displayName: String {
        let name = "\(vendorName) \(modelName)".trimmingCharacters(in: .whitespacesAndNewlines)
        return name.isEmpty ? AlesisConstants.coreAudioDeviceName : name
    }

    var guidHex: String { String(format: "0x%016llX", guid) }
    var vendorHex: String { String(format: "0x%06X", vendorID) }
    var modelHex: String { String(format: "0x%06X", modelID) }

    nonisolated static func make(from device: ASFWDriverConnector.FWDeviceInfo) -> AlesisDiscoveredIdentity {
        AlesisDiscoveredIdentity(
            guid: device.guid,
            nodeID: device.nodeId,
            generation: device.generation,
            vendorID: device.vendorId,
            modelID: device.modelId,
            vendorName: device.vendorName,
            modelName: device.modelName,
            unitCount: device.units.count
        )
    }
}

struct AlesisDiceSection: Equatable {
    var offset: UInt32
    var size: UInt32

    nonisolated static func parse(_ data: Data, offset: Int) -> AlesisDiceSection? {
        guard let rawOffset = data.asfwBE32(at: offset),
              let rawSize = data.asfwBE32(at: offset + 4) else {
            return nil
        }
        return AlesisDiceSection(offset: rawOffset * 4, size: rawSize * 4)
    }
}

struct AlesisDiceSections: Equatable {
    var global: AlesisDiceSection
    var txStreamFormat: AlesisDiceSection
    var rxStreamFormat: AlesisDiceSection
    var extSync: AlesisDiceSection
    var reserved: AlesisDiceSection

    nonisolated static func parse(_ data: Data) -> AlesisDiceSections? {
        guard data.count >= 40,
              let global = AlesisDiceSection.parse(data, offset: 0),
              let tx = AlesisDiceSection.parse(data, offset: 8),
              let rx = AlesisDiceSection.parse(data, offset: 16),
              let ext = AlesisDiceSection.parse(data, offset: 24),
              let reserved = AlesisDiceSection.parse(data, offset: 32) else {
            return nil
        }
        return AlesisDiceSections(
            global: global,
            txStreamFormat: tx,
            rxStreamFormat: rx,
            extSync: ext,
            reserved: reserved
        )
    }
}

struct AlesisDiceGlobalState: Equatable {
    var nickname: String
    var clockSelect: UInt32
    var enabled: Bool
    var status: UInt32
    var extStatus: UInt32
    var sampleRate: UInt32
    var version: UInt32
    var clockCaps: UInt32

    var sourceLocked: Bool { (status & 0x00000001) != 0 }

    var nominalRateHz: UInt32 {
        let index = (status & 0x0000FF00) >> 8
        switch index {
        case 0x00: return 32_000
        case 0x01: return 44_100
        case 0x02: return 48_000
        case 0x03: return 88_200
        case 0x04: return 96_000
        case 0x05: return 176_400
        case 0x06: return 192_000
        default: return 0
        }
    }

    var clockSourceName: String {
        let source = clockSelect & 0x000000FF
        switch source {
        case 0x00: return "AES 1"
        case 0x01: return "AES 2"
        case 0x02: return "AES 3"
        case 0x03: return "AES 4"
        case 0x04: return "AES Any"
        case 0x05: return "ADAT"
        case 0x06: return "TDIF"
        case 0x07: return "Word Clock"
        case 0x08: return "ARX 1"
        case 0x09: return "ARX 2"
        case 0x0A: return "ARX 3"
        case 0x0B: return "ARX 4"
        case 0x0C: return "Internal"
        default: return String(format: "Unknown 0x%02X", source)
        }
    }

    nonisolated static func parse(_ data: Data) -> AlesisDiceGlobalState? {
        guard data.count >= 0x68,
              let clockSelect = data.asfwBE32(at: 0x4C),
              let enable = data.asfwBE32(at: 0x50),
              let status = data.asfwBE32(at: 0x54),
              let extStatus = data.asfwBE32(at: 0x58),
              let sampleRate = data.asfwBE32(at: 0x5C),
              let version = data.asfwBE32(at: 0x60),
              let clockCaps = data.asfwBE32(at: 0x64) else {
            return nil
        }

        return AlesisDiceGlobalState(
            nickname: data.asfwDICEString(at: 0x0C, length: 64),
            clockSelect: clockSelect,
            enabled: enable != 0,
            status: status,
            extStatus: extStatus,
            sampleRate: sampleRate,
            version: version,
            clockCaps: clockCaps
        )
    }
}

struct AlesisDiceStreamEntry: Equatable, Identifiable {
    var id: Int
    var isoChannel: Int32
    var seqStart: UInt32?
    var pcmChannels: UInt32
    var midiPorts: UInt32
    var speed: UInt32?
    var labels: String

    var isActive: Bool { isoChannel >= 0 }
    var am824Slots: UInt32 { pcmChannels + ((midiPorts + 7) / 8) }
}

struct AlesisDiceStreamConfig: Equatable {
    var isRxLayout: Bool
    var reportedStreamCount: UInt32
    var entrySizeBytes: UInt32
    var streams: [AlesisDiceStreamEntry]

    var activePcmChannels: UInt32 {
        streams.filter(\.isActive).reduce(0) { $0 + $1.pcmChannels }
    }

    var totalPcmChannels: UInt32 {
        streams.reduce(0) { $0 + $1.pcmChannels }
    }

    nonisolated static func parse(_ data: Data, isRxLayout: Bool) -> AlesisDiceStreamConfig? {
        guard data.count >= 8,
              let count = data.asfwBE32(at: 0),
              let entryQuadlets = data.asfwBE32(at: 4) else {
            return nil
        }

        let entrySize = entryQuadlets * 4
        guard entrySize >= 16 else {
            return AlesisDiceStreamConfig(isRxLayout: isRxLayout,
                                          reportedStreamCount: count,
                                          entrySizeBytes: entrySize,
                                          streams: [])
        }

        let clampedCount = min(Int(count), 4)
        var streams: [AlesisDiceStreamEntry] = []
        for index in 0..<clampedCount {
            let base = 8 + (index * Int(entrySize))
            guard base + 16 <= data.count,
                  let iso = data.asfwBE32(at: base),
                  let a = data.asfwBE32(at: base + 4),
                  let b = data.asfwBE32(at: base + 8),
                  let c = data.asfwBE32(at: base + 12) else {
                break
            }

            let labels = (Int(entrySize) >= 280 && base + 280 <= data.count)
                ? data.asfwDICEString(at: base + 0x10, length: 256)
                : ""

            let entry: AlesisDiceStreamEntry
            if isRxLayout {
                entry = AlesisDiceStreamEntry(id: index,
                                              isoChannel: Int32(bitPattern: iso),
                                              seqStart: a,
                                              pcmChannels: b,
                                              midiPorts: c,
                                              speed: nil,
                                              labels: labels)
            } else {
                entry = AlesisDiceStreamEntry(id: index,
                                              isoChannel: Int32(bitPattern: iso),
                                              seqStart: nil,
                                              pcmChannels: a,
                                              midiPorts: b,
                                              speed: c,
                                              labels: labels)
            }
            streams.append(entry)
        }

        return AlesisDiceStreamConfig(isRxLayout: isRxLayout,
                                      reportedStreamCount: count,
                                      entrySizeBytes: entrySize,
                                      streams: streams)
    }
}

struct AlesisDiceSnapshot: Equatable {
    var sections: AlesisDiceSections
    var global: AlesisDiceGlobalState?
    var txStreams: AlesisDiceStreamConfig?
    var rxStreams: AlesisDiceStreamConfig?
}

private extension Data {
    func asfwBE32(at offset: Int) -> UInt32? {
        guard offset >= 0, offset + 4 <= count else { return nil }
        return self[offset..<offset + 4].reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
    }

    func asfwDICEString(at offset: Int, length: Int) -> String {
        guard offset >= 0, length > 0, offset < count else { return "" }
        let end = Swift.min(count, offset + length)
        let bytes = Array(self[offset..<end])
        let terminator = bytes.firstIndex(of: 0) ?? bytes.count
        return String(bytes: bytes[..<terminator], encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    }
}
