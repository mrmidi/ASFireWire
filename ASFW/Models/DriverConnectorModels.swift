import Foundation

// Canonical DriverConnector model types (IOKit-free, pure data/parsers).

enum DriverConnectorSharedStatusReason: UInt32 {
    case boot = 1
    case interrupt = 2
    case busReset = 3
    case asyncActivity = 4
    case watchdog = 5
    case manual = 6
    case disconnect = 7
    case unknown = 0
}

struct DriverConnectorSharedStatusFlags {
    static let isIRM: UInt32 = 1 << 0
    static let isCycleMaster: UInt32 = 1 << 1
    static let linkActive: UInt32 = 1 << 2
}

extension DriverConnectorSharedStatusReason {
    var displayName: String {
        switch self {
        case .boot: return "Boot"
        case .interrupt: return "Interrupt"
        case .busReset: return "Bus Reset"
        case .asyncActivity: return "Async Activity"
        case .watchdog: return "Watchdog"
        case .manual: return "Manual"
        case .disconnect: return "Disconnect"
        case .unknown: return "Unknown"
        }
    }
}

struct DriverConnectorStatus {
    let sequence: UInt64
    let timestampMach: UInt64
    let reason: DriverConnectorSharedStatusReason
    let detailMask: UInt32
    let controllerState: UInt32
    let controllerStateName: String
    let flags: UInt32

    let busGeneration: UInt32
    let nodeCount: UInt32
    let localNodeID: UInt32?
    let rootNodeID: UInt32?
    let irmNodeID: UInt32?

    let busResetCount: UInt64
    let lastBusResetStart: UInt64
    let lastBusResetCompletion: UInt64

    let asyncLastCompletion: UInt64
    let asyncTimeouts: UInt32
    let watchdogTickCount: UInt64
    let watchdogLastTickUsec: UInt64

    init?(rawPointer: UnsafeRawPointer, length: Int) {
        guard length >= 256 else { return nil }

        func loadUInt32(_ offset: Int) -> UInt32 {
            rawPointer.load(fromByteOffset: offset, as: UInt32.self).littleEndian
        }

        func loadUInt64(_ offset: Int) -> UInt64 {
            rawPointer.load(fromByteOffset: offset, as: UInt64.self).littleEndian
        }

        let version = loadUInt32(0)
        guard version == 1 else { return nil }

        let payloadLength = loadUInt32(4)
        guard payloadLength <= length else { return nil }

        self.sequence = loadUInt64(8)
        self.timestampMach = loadUInt64(16)
        self.reason = DriverConnectorSharedStatusReason(rawValue: loadUInt32(24)) ?? .unknown
        self.detailMask = loadUInt32(28)

        var nameBuffer = [CChar](repeating: 0, count: 32)
        nameBuffer.withUnsafeMutableBytes { dest in
            dest.copyBytes(from: UnsafeRawBufferPointer(start: rawPointer.advanced(by: 32), count: 32))
        }
        self.controllerStateName = String(cString: nameBuffer)

        self.controllerState = loadUInt32(64)
        self.flags = loadUInt32(68)

        self.busGeneration = loadUInt32(72)
        self.nodeCount = loadUInt32(76)

        func decodeNodeID(_ raw: UInt32) -> UInt32? {
            return raw == 0xFFFF_FFFF ? nil : raw
        }

        self.localNodeID = decodeNodeID(loadUInt32(80))
        self.rootNodeID = decodeNodeID(loadUInt32(84))
        self.irmNodeID = decodeNodeID(loadUInt32(88))

        self.busResetCount = loadUInt64(96)
        self.lastBusResetStart = loadUInt64(104)
        self.lastBusResetCompletion = loadUInt64(112)
        self.asyncLastCompletion = loadUInt64(120)
        _ = loadUInt32(128) // asyncPending (reserved)
        self.asyncTimeouts = loadUInt32(132)
        self.watchdogTickCount = loadUInt64(136)
        self.watchdogLastTickUsec = loadUInt64(144)
    }

    var isIRM: Bool { (flags & DriverConnectorSharedStatusFlags.isIRM) != 0 }
    var isCycleMaster: Bool { (flags & DriverConnectorSharedStatusFlags.isCycleMaster) != 0 }
    var linkActive: Bool { (flags & DriverConnectorSharedStatusFlags.linkActive) != 0 }
}

struct DriverConnectorVersionInfo {
    let semanticVersion: String
    let gitCommitShort: String
    let gitCommitFull: String
    let gitBranch: String
    let buildTimestamp: String
    let buildHost: String
    let gitDirty: Bool

    init?(data: Data) {
        // Expect at least 242 bytes (up to gitDirty)
        guard data.count >= 242 else { return nil }

        func readString(offset: Int, length: Int) -> String {
            let subdata = data.subdata(in: offset..<(offset + length))
            return subdata.withUnsafeBytes { ptr in
                if let base = ptr.baseAddress {
                    return String(cString: base.bindMemory(to: CChar.self, capacity: length))
                }
                return ""
            }
        }

        self.semanticVersion = readString(offset: 0, length: 32)
        self.gitCommitShort = readString(offset: 32, length: 8)
        self.gitCommitFull = readString(offset: 40, length: 41)
        self.gitBranch = readString(offset: 81, length: 64)
        self.buildTimestamp = readString(offset: 145, length: 32)
        self.buildHost = readString(offset: 177, length: 64)
        self.gitDirty = data[241] != 0
    }
}

struct DriverConnectorAVCSubunitInfo: Identifiable {
    let id = UUID()
    let type: UInt8
    let subunitID: UInt8
    let numSrcPlugs: UInt8
    let numDestPlugs: UInt8

    var typeName: String {
        switch type {
        case 0x00: return "Video"
        case 0x01: return "Audio"
        case 0x0C: return "Music"
        default: return String(format: "Unknown (0x%02X)", type)
        }
    }

    var symbolName: String {
        switch type {
        case 0x00: return "tv"
        case 0x01: return "speaker.wave.2"
        case 0x0C: return "music.note"
        default: return "questionmark.square"
        }
    }

    var accentColor: String {
        switch type {
        case 0x00: return "blue"
        case 0x01: return "purple"
        case 0x0C: return "orange"
        default: return "gray"
        }
    }
}

struct DriverConnectorAVCUnitInfo: Identifiable {
    let id = UUID()
    let guid: UInt64
    let nodeID: UInt16
    let vendorID: UInt16
    let modelID: UInt16
    let subunits: [DriverConnectorAVCSubunitInfo]
    
    // Unit-level plug counts (from AVCUnitPlugInfoCommand)
    let isoInputPlugs: UInt8
    let isoOutputPlugs: UInt8
    let extInputPlugs: UInt8
    let extOutputPlugs: UInt8

    var guidHex: String { String(format: "0x%016X", guid) }
    var nodeIDHex: String { String(format: "0x%04X", nodeID) }
    var isInitialized: Bool { true } // Always true for discovered units
    
    /// Total isochronous plugs (bidirectional)
    var totalIsoPlugs: UInt8 { isoInputPlugs + isoOutputPlugs }
    
    /// Total external plugs (bidirectional)
    var totalExtPlugs: UInt8 { extInputPlugs + extOutputPlugs }
}

struct DriverConnectorAVCMusicCapabilities {
    let hasAudioCapability: Bool
    let hasMidiCapability: Bool
    let hasSmpteCapability: Bool

    // Global Rates
    let currentRate: UInt8
    let supportedRatesMask: UInt32

    let audioInputPorts: UInt8
    let audioOutputPorts: UInt8
    let midiInputPorts: UInt8
    let midiOutputPorts: UInt8

    let smpteInputPorts: UInt8
    let smpteOutputPorts: UInt8

    /// Individual channel detail within a signal block
    struct ChannelDetail: Identifiable {
        let id = UUID()
        let musicPlugID: UInt16
        let position: UInt8
        let name: String
    }

    /// Signal block with nested channel details
    struct SignalBlock: Identifiable {
        let id = UUID()
        let formatCode: UInt8
        let channelCount: UInt8
        let channels: [ChannelDetail]

        var formatCodeName: String {
            switch formatCode {
            case 0x00: return "IEC60958"
            case 0x06: return "MBLA"
            case 0x0D: return "MIDI"
            case 0x40: return "SyncStream"
            default: return String(format: "0x%02X", formatCode)
            }
        }
    }

    /// Supported stream format entry (from 0xBF STREAM FORMAT queries)
    struct SupportedFormat: Identifiable {
        let id = UUID()
        let sampleRateCode: UInt8
        let formatCode: UInt8
        let channelCount: UInt8

        var sampleRateName: String {
            switch sampleRateCode {
            case 0x00: return "22.05 kHz"
            case 0x01: return "24 kHz"
            case 0x02: return "32 kHz"
            case 0x03: return "44.1 kHz"
            case 0x04: return "48 kHz"
            case 0x05: return "96 kHz"
            case 0x06: return "176.4 kHz"
            case 0x07: return "192 kHz"
            case 0x0A: return "88.2 kHz"
            case 0x0F: return "Don't Care"
            default: return String(format: "0x%02X", sampleRateCode)
            }
        }

        var formatCodeName: String {
            switch formatCode {
            case 0x06: return "MBLA"
            case 0x40: return "Sync"
            default: return String(format: "0x%02X", formatCode)
            }
        }
    }

    /// Plug info with nested signal blocks and supported formats
    struct PlugInfo: Identifiable {
        let id = UUID()
        let plugID: UInt8
        let isInput: Bool
        let type: UInt8
        let name: String
        let signalBlocks: [SignalBlock]
        let supportedFormats: [SupportedFormat]

        var typeName: String {
            switch type {
            case 0x00: return "Audio"
            case 0x01: return "MIDI"
            case 0x80: return "Sync"
            default: return String(format: "0x%02X", type)
            }
        }

        /// Get all channel names from all signal blocks (flattened)
        var allChannelNames: [String] {
            signalBlocks.flatMap { $0.channels.map { $0.name } }.filter { !$0.isEmpty }
        }
    }

    let plugs: [PlugInfo]

    /// Legacy: flat channel list for backward compatibility
    /// Now populated from nested channels within plugs
    struct MusicChannel: Identifiable {
        let id = UUID()
        let musicPlugID: UInt16
        let plugType: UInt8
        let name: String

        var plugTypeName: String {
            switch plugType {
            case 0x00: return "Audio"
            case 0x01: return "MIDI"
            case 0x02: return "SMPTE"
            case 0x03: return "SampleCount"
            case 0x80: return "Sync"
            default: return String(format: "0x%02X", plugType)
            }
        }
    }

    let channels: [MusicChannel]

    init?(data: Data) {
        guard data.count >= 18 else { return nil }

        #if DEBUG
        print("[ASFW][Serde] AVCMusicCapabilities: raw bytes=\(data.count)")
        #endif

        // Header: AVCMusicCapabilitiesWire (18 bytes aligned to 20)
        // Byte 0: flags (hasAudio:1, hasMIDI:1, hasSMPTE:1, reserved:5)
        let flags = data[0]
        self.hasAudioCapability = (flags & 0x01) != 0
        self.hasMidiCapability = (flags & 0x02) != 0
        self.hasSmpteCapability = (flags & 0x04) != 0

        // Byte 1: currentRate
        self.currentRate = data[1]

        // Bytes 2-5: supportedRatesMask (little-endian)
        self.supportedRatesMask = data.subdata(in: 2..<6).withUnsafeBytes { $0.load(as: UInt32.self) }

        // Bytes 6-7: padding

        // Bytes 8-13: port counts
        self.audioInputPorts = data[8]
        self.audioOutputPorts = data[9]
        self.midiInputPorts = data[10]
        self.midiOutputPorts = data[11]
        self.smpteInputPorts = data[12]
        self.smpteOutputPorts = data[13]

        // Byte 14: numPlugs
        let numPlugs = Int(data[14])
        // Byte 15: reserved (was numChannels)
        // Bytes 16-17: padding

        var offset = 18  // Header size

        #if DEBUG
        print("[ASFW][Serde] Flags: audio=\(hasAudioCapability) midi=\(hasMidiCapability) smpte=\(hasSmpteCapability)")
        print(String(format: "[ASFW][Serde] Rates: current=0x%02X mask=0x%08X", currentRate, supportedRatesMask))
        print("[ASFW][Serde] Ports: audioIn=\(audioInputPorts) audioOut=\(audioOutputPorts)")
        print("[ASFW][Serde] numPlugs=\(numPlugs)")
        #endif

        // Parse Plugs with nested SignalBlocks and ChannelDetails
        var parsedPlugs: [PlugInfo] = []
        var allChannels: [MusicChannel] = []

        for plugIdx in 0..<numPlugs {
            // PlugInfoWire: 40 bytes fixed header
            // 0: plugID, 1: isInput, 2: type, 3: numSignalBlocks, 4: nameLength, 5-36: name[32], 37: numSupportedFormats, 38-39: padding
            guard offset + 40 <= data.count else { break }

            let plugID = data[offset]
            let isInput = data[offset+1] != 0
            let plugType = data[offset+2]
            let numBlocks = Int(data[offset+3])
            let nameLength = Int(data[offset+4])

            let nameData = data.subdata(in: (offset + 5)..<(offset + 5 + min(nameLength, 32)))
            let plugName = String(data: nameData, encoding: .utf8)?.trimmingCharacters(in: .controlCharacters) ?? ""

            let numSupportedFormats = Int(data[offset+37])

            offset += 40

            // Parse SignalBlocks
            var signalBlocks: [SignalBlock] = []
            for _ in 0..<numBlocks {
                // SignalBlockWire: 4 bytes
                // 0: formatCode, 1: channelCount, 2: numChannelDetails, 3: padding
                guard offset + 4 <= data.count else { break }

                let formatCode = data[offset]
                let channelCount = data[offset+1]
                let numChannelDetails = Int(data[offset+2])
                // byte 3 is padding

                offset += 4

                // Parse ChannelDetails
                var channelDetails: [ChannelDetail] = []
                for _ in 0..<numChannelDetails {
                    // ChannelDetailWire: 36 bytes
                    // 0-1: musicPlugID (LE), 2: position, 3: nameLength, 4-35: name[32]
                    guard offset + 36 <= data.count else { break }

                    let musicPlugID = UInt16(data[offset]) | (UInt16(data[offset+1]) << 8)
                    let position = data[offset+2]
                    let chNameLength = Int(data[offset+3])

                    let chNameData = data.subdata(in: (offset + 4)..<(offset + 4 + min(chNameLength, 32)))
                    let chName = String(data: chNameData, encoding: .utf8)?.trimmingCharacters(in: .controlCharacters) ?? ""

                    channelDetails.append(ChannelDetail(musicPlugID: musicPlugID, position: position, name: chName))

                    // Also populate flat channel list for backward compatibility
                    allChannels.append(MusicChannel(musicPlugID: musicPlugID, plugType: plugType, name: chName))

                    offset += 36
                }

                signalBlocks.append(SignalBlock(formatCode: formatCode, channelCount: channelCount, channels: channelDetails))
            }

            // Parse Supported Formats (from 0xBF STREAM FORMAT queries)
            var supportedFormats: [SupportedFormat] = []
            for _ in 0..<numSupportedFormats {
                // SupportedFormatWire: 4 bytes
                // 0: sampleRateCode, 1: formatCode, 2: channelCount, 3: padding
                guard offset + 4 <= data.count else { break }

                let sampleRateCode = data[offset]
                let fmtFormatCode = data[offset+1]
                let fmtChannelCount = data[offset+2]
                // byte 3 is padding

                supportedFormats.append(SupportedFormat(sampleRateCode: sampleRateCode, formatCode: fmtFormatCode, channelCount: fmtChannelCount))

                offset += 4
            }

            parsedPlugs.append(PlugInfo(plugID: plugID, isInput: isInput, type: plugType, name: plugName, signalBlocks: signalBlocks, supportedFormats: supportedFormats))

            #if DEBUG
            let channelNames = signalBlocks.flatMap { $0.channels.map { $0.name } }.joined(separator: ", ")
            print("[ASFW][Serde] Plug \(plugIdx): id=\(plugID) \(isInput ? "In" : "Out") type=0x\(String(format: "%02X", plugType)) name='\(plugName)' blocks=\(signalBlocks.count) channels=[\(channelNames)] supportedFormats=\(supportedFormats.count)")
            #endif
        }

        self.plugs = parsedPlugs
        self.channels = allChannels

        #if DEBUG
        print("[ASFW][Serde] Parsed \(parsedPlugs.count) plugs, \(allChannels.count) total channels")
        #endif
    }
}

enum DriverConnectorFWDeviceState: UInt8 {
    case created = 0
    case ready = 1
    case suspended = 2
    case terminated = 3

    var description: String {
        switch self {
        case .created: return "Created"
        case .ready: return "Ready"
        case .suspended: return "Suspended"
        case .terminated: return "Terminated"
        }
    }
}

enum DriverConnectorFWUnitState: UInt8 {
    case created = 0
    case ready = 1
    case suspended = 2
    case terminated = 3

    var description: String {
        switch self {
        case .created: return "Created"
        case .ready: return "Ready"
        case .suspended: return "Suspended"
        case .terminated: return "Terminated"
        }
    }
}

struct DriverConnectorFWDeviceInfo: Identifiable {
    let id: UInt64  // GUID
    let guid: UInt64
    let vendorId: UInt32
    let modelId: UInt32
    let vendorName: String
    let modelName: String
    let nodeId: UInt8
    let generation: UInt32
    let state: DriverConnectorFWDeviceState
    let units: [DriverConnectorFWUnitInfo]

    var stateString: String { state.description }
}

struct DriverConnectorFWUnitInfo: Identifiable {
    let id = UUID()
    let specId: UInt32
    let swVersion: UInt32
    let state: DriverConnectorFWUnitState
    let romOffset: UInt32
    let vendorName: String?
    let productName: String?

    var specIdHex: String { String(format: "0x%06X", specId) }
    var swVersionHex: String { String(format: "0x%06X", swVersion) }
    var stateString: String { state.description }
}

// API compatibility aliases (keep existing public names and nested access stable)
typealias SharedStatusReason = DriverConnectorSharedStatusReason
typealias SharedStatusFlags = DriverConnectorSharedStatusFlags
typealias DriverStatus = DriverConnectorStatus
typealias DriverVersionInfo = DriverConnectorVersionInfo
typealias AVCSubunitInfo = DriverConnectorAVCSubunitInfo
typealias AVCUnitInfo = DriverConnectorAVCUnitInfo
typealias AVCMusicCapabilities = DriverConnectorAVCMusicCapabilities
typealias FWDeviceState = DriverConnectorFWDeviceState
typealias FWUnitState = DriverConnectorFWUnitState
typealias FWDeviceInfo = DriverConnectorFWDeviceInfo
typealias FWUnitInfo = DriverConnectorFWUnitInfo
