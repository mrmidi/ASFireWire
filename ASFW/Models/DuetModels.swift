import Foundation

// MARK: - Duet Wire Constants

struct DuetVendorWireConstants {
    static let oui: [UInt8] = [0x00, 0x03, 0xDB]
    static let prefix: [UInt8] = [0x50, 0x43, 0x4D] // "PCM"

    static let argDefault: UInt8 = 0xFF
    static let argIndexed: UInt8 = 0x80

    static let boolOn: UInt8 = 0x70
    static let boolOff: UInt8 = 0x60

    static let ctypeControl: UInt8 = 0x00
    static let ctypeStatus: UInt8 = 0x01
    static let subunitUnit: UInt8 = 0xFF
    static let opcodeVendorDependent: UInt8 = 0x00
}

enum DuetVendorCommandCode: UInt8 {
    case micPolarity = 0x00
    case xlrIsMicLevel = 0x01
    case xlrIsConsumerLevel = 0x02
    case micPhantom = 0x03
    case outIsConsumerLevel = 0x04
    case inGain = 0x05
    case hwState = 0x07
    case outMute = 0x09
    case inputSourceIsPhone = 0x0C
    case mixerSrc = 0x10
    case outSourceIsMixer = 0x11
    case displayOverholdTwoSec = 0x13
    case displayClear = 0x14
    case outVolume = 0x15
    case muteForLineOut = 0x16
    case muteForHpOut = 0x17
    case unmuteForLineOut = 0x18
    case unmuteForHpOut = 0x19
    case displayIsInput = 0x1B
    case inClickless = 0x1E
    case displayFollowToKnob = 0x22
}

struct DuetVendorCodec {
    static func buildFrame(isStatus: Bool,
                           code: DuetVendorCommandCode,
                           index: UInt8? = nil,
                           index2: UInt8? = nil,
                           controlPayload: [UInt8] = []) -> Data? {
        var operands: [UInt8] = []
        operands.reserveCapacity(16)

        operands.append(contentsOf: DuetVendorWireConstants.oui)
        operands.append(contentsOf: DuetVendorWireConstants.prefix)
        operands.append(code.rawValue)

        let args = resolveArgs(code: code, index: index, index2: index2)
        operands.append(args.arg1)
        operands.append(args.arg2)

        if !isStatus {
            operands.append(contentsOf: controlPayload)
        }

        var frame = Data()
        frame.append(isStatus ? DuetVendorWireConstants.ctypeStatus : DuetVendorWireConstants.ctypeControl)
        frame.append(DuetVendorWireConstants.subunitUnit)
        frame.append(DuetVendorWireConstants.opcodeVendorDependent)
        frame.append(contentsOf: operands)

        let paddedLength = (frame.count + 3) & ~3
        if paddedLength > 512 {
            return nil
        }

        if paddedLength > frame.count {
            frame.append(contentsOf: Array(repeating: 0, count: paddedLength - frame.count))
        }

        return frame
    }

    static func parseStatusPayload(_ response: Data,
                                   expectedCode: DuetVendorCommandCode,
                                   expectedIndex: UInt8? = nil,
                                   expectedIndex2: UInt8? = nil) -> [UInt8]? {
        guard response.count >= 12 else {
            return nil
        }

        guard isSuccessResponseCType(response[0]),
              response[1] == DuetVendorWireConstants.subunitUnit,
              response[2] == DuetVendorWireConstants.opcodeVendorDependent,
              response[3] == DuetVendorWireConstants.oui[0],
              response[4] == DuetVendorWireConstants.oui[1],
              response[5] == DuetVendorWireConstants.oui[2],
              response[6] == DuetVendorWireConstants.prefix[0],
              response[7] == DuetVendorWireConstants.prefix[1],
              response[8] == DuetVendorWireConstants.prefix[2],
              response[9] == expectedCode.rawValue
        else {
            return nil
        }

        if usesIndexedArg(expectedCode), let index = expectedIndex, response[11] != index {
            return nil
        }

        if expectedCode == .mixerSrc {
            guard let source = expectedIndex, let destination = expectedIndex2 else {
                return nil
            }
            if response[10] != encodeMixerSource(source) || response[11] != destination {
                return nil
            }
        }

        return Array(response.dropFirst(12))
    }

    static func resolveArgs(code: DuetVendorCommandCode,
                            index: UInt8?,
                            index2: UInt8?) -> (arg1: UInt8, arg2: UInt8) {
        if usesIndexedArg(code) {
            return (DuetVendorWireConstants.argIndexed, index ?? 0)
        }

        if usesOutputIndexedArg(code) {
            return (DuetVendorWireConstants.argIndexed, DuetVendorWireConstants.argDefault)
        }

        if code == .mixerSrc {
            return (encodeMixerSource(index ?? 0), index2 ?? 0)
        }

        return (DuetVendorWireConstants.argDefault, DuetVendorWireConstants.argDefault)
    }

    static func encodeMixerSource(_ source: UInt8) -> UInt8 {
        return ((source / 2) << 4) | (source % 2)
    }

    static func usesIndexedArg(_ code: DuetVendorCommandCode) -> Bool {
        switch code {
        case .micPolarity, .xlrIsMicLevel, .xlrIsConsumerLevel, .micPhantom, .inGain, .inputSourceIsPhone:
            return true
        default:
            return false
        }
    }

    static func usesOutputIndexedArg(_ code: DuetVendorCommandCode) -> Bool {
        switch code {
        case .outIsConsumerLevel, .outMute, .outVolume, .muteForLineOut, .muteForHpOut, .unmuteForLineOut, .unmuteForHpOut:
            return true
        default:
            return false
        }
    }

    static func isSuccessResponseCType(_ ctype: UInt8) -> Bool {
        switch ctype {
        case 0x09, 0x0C, 0x0D:
            return true
        default:
            return false
        }
    }
}

// MARK: - Duet Domain Models

enum DuetKnobTarget: UInt8, CaseIterable, Identifiable {
    case outputPair0 = 0
    case inputPair0 = 1
    case inputPair1 = 2

    var id: UInt8 { rawValue }

    var displayName: String {
        switch self {
        case .outputPair0: return "Output"
        case .inputPair0: return "Input 1"
        case .inputPair1: return "Input 2"
        }
    }
}

struct DuetKnobState: Equatable {
    var outputMute: Bool = false
    var target: DuetKnobTarget = .outputPair0
    var outputVolume: UInt8 = 0
    var inputGains: [UInt8] = [0, 0]

    static let outputVolumeMin: UInt8 = 0
    static let outputVolumeMax: UInt8 = 64
    static let inputGainMin: UInt8 = 10
    static let inputGainMax: UInt8 = 75
}

enum DuetOutputSource: UInt8, CaseIterable, Identifiable {
    case streamInputPair0 = 0
    case mixerOutputPair0 = 1

    var id: UInt8 { rawValue }

    var displayName: String {
        switch self {
        case .streamInputPair0: return "Stream"
        case .mixerOutputPair0: return "Mixer"
        }
    }
}

enum DuetOutputNominalLevel: UInt8, CaseIterable, Identifiable {
    case instrument = 0
    case consumer = 1

    var id: UInt8 { rawValue }

    var displayName: String {
        switch self {
        case .instrument: return "+4 dBu"
        case .consumer: return "-10 dBV"
        }
    }
}

enum DuetOutputMuteMode: UInt8, CaseIterable, Identifiable {
    case never = 0
    case normal = 1
    case swapped = 2

    var id: UInt8 { rawValue }

    var displayName: String {
        switch self {
        case .never: return "Never"
        case .normal: return "Normal"
        case .swapped: return "Swapped"
        }
    }
}

struct DuetOutputParams: Equatable {
    var mute: Bool = false
    var volume: UInt8 = 0
    var source: DuetOutputSource = .streamInputPair0
    var nominalLevel: DuetOutputNominalLevel = .instrument
    var lineMuteMode: DuetOutputMuteMode = .never
    var hpMuteMode: DuetOutputMuteMode = .never

    static let volumeMin: UInt8 = 0
    static let volumeMax: UInt8 = 64
}

enum DuetInputSource: UInt8, CaseIterable, Identifiable {
    case xlr = 0
    case phone = 1

    var id: UInt8 { rawValue }

    var displayName: String {
        switch self {
        case .xlr: return "XLR"
        case .phone: return "Phone"
        }
    }
}

enum DuetInputXlrNominalLevel: UInt8, CaseIterable, Identifiable {
    case microphone = 0
    case professional = 1
    case consumer = 2

    var id: UInt8 { rawValue }

    var displayName: String {
        switch self {
        case .microphone: return "Mic"
        case .professional: return "+4"
        case .consumer: return "-10"
        }
    }
}

struct DuetInputParams: Equatable {
    var gains: [UInt8] = [0, 0]
    var polarities: [Bool] = [false, false]
    var xlrNominalLevels: [DuetInputXlrNominalLevel] = [.microphone, .microphone]
    var phantomPowerings: [Bool] = [false, false]
    var sources: [DuetInputSource] = [.xlr, .xlr]
    var clickless: Bool = false

    static let gainMin: UInt8 = 10
    static let gainMax: UInt8 = 75
}

struct DuetMixerCoefficients: Equatable {
    var analogInputs: [UInt16] = [0, 0]
    var streamInputs: [UInt16] = [0, 0]
}

struct DuetMixerParams: Equatable {
    var outputs: [DuetMixerCoefficients] = [DuetMixerCoefficients(), DuetMixerCoefficients()]

    static let gainMin: UInt16 = 0
    static let gainMax: UInt16 = 0x3FFF

    func gain(destination: Int, source: Int) -> UInt16 {
        guard destination >= 0 && destination < outputs.count else { return 0 }
        if source >= 0 && source < 2 {
            return outputs[destination].analogInputs[source]
        }
        if source >= 2 && source < 4 {
            return outputs[destination].streamInputs[source - 2]
        }
        return 0
    }

    mutating func setGain(destination: Int, source: Int, value: UInt16) {
        guard destination >= 0 && destination < outputs.count else { return }
        let clamped = max(DuetMixerParams.gainMin, min(DuetMixerParams.gainMax, value))
        if source >= 0 && source < 2 {
            outputs[destination].analogInputs[source] = clamped
        } else if source >= 2 && source < 4 {
            outputs[destination].streamInputs[source - 2] = clamped
        }
    }
}

enum DuetDisplayTarget: UInt8, CaseIterable, Identifiable {
    case output = 0
    case input = 1

    var id: UInt8 { rawValue }
}

enum DuetDisplayMode: UInt8, CaseIterable, Identifiable {
    case independent = 0
    case followingToKnobTarget = 1

    var id: UInt8 { rawValue }
}

enum DuetDisplayOverhold: UInt8, CaseIterable, Identifiable {
    case infinite = 0
    case twoSeconds = 1

    var id: UInt8 { rawValue }
}

struct DuetDisplayParams: Equatable {
    var target: DuetDisplayTarget = .output
    var mode: DuetDisplayMode = .independent
    var overhold: DuetDisplayOverhold = .infinite
}

struct DuetStateSnapshot: Equatable {
    var knobState: DuetKnobState?
    var outputParams: DuetOutputParams?
    var inputParams: DuetInputParams?
    var mixerParams: DuetMixerParams?
    var displayParams: DuetDisplayParams?
    var firmwareID: UInt32?
    var hardwareID: UInt32?
    var updatedAt: Date?
}

enum DuetOutputBank: Int, CaseIterable, Identifiable {
    case output1 = 0
    case output2 = 1

    var id: Int { rawValue }

    var displayName: String {
        switch self {
        case .output1: return "Output 1"
        case .output2: return "Output 2"
        }
    }
}

extension DuetInputXlrNominalLevel {
    static func fromWire(isMicLevel: Bool, isConsumerLevel: Bool) -> DuetInputXlrNominalLevel {
        if isMicLevel {
            return .microphone
        }
        if isConsumerLevel {
            return .consumer
        }
        return .professional
    }
}

extension DuetOutputMuteMode {
    static func fromWire(mute: Bool, unmute: Bool) -> DuetOutputMuteMode {
        if mute && unmute {
            return .never
        }
        if mute && !unmute {
            return .swapped
        }
        if !mute && unmute {
            return .normal
        }
        return .never
    }

    func toWireFlags() -> (mute: Bool, unmute: Bool) {
        switch self {
        case .never:
            return (true, true)
        case .normal:
            return (false, true)
        case .swapped:
            return (true, false)
        }
    }
}
