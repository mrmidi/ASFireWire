//
//  AudioWrappers.swift
//  ASFW
//
//  Minimal Core Audio wrapper for ASFireWire debugging.
//

import Foundation
import CoreAudio

// MARK: - Core Audio Helpers

func getAudioObjectProperty<T>(
    objectID: AudioObjectID,
    selector: AudioObjectPropertySelector,
    scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
    element: AudioObjectPropertyElement = kAudioObjectPropertyElementMain,
    defaultValue: T
) -> T {
    var address = AudioObjectPropertyAddress(
        mSelector: selector,
        mScope: scope,
        mElement: element
    )
    
    var size = UInt32(MemoryLayout<T>.size)
    var value = defaultValue
    
    let status = withUnsafeMutablePointer(to: &value) { ptr in
        AudioObjectGetPropertyData(objectID, &address, 0, nil, &size, ptr)
    }
    
    if status != noErr {
        return defaultValue
    }
    return value
}

func getAudioObjectPropertyArray<T>(
    objectID: AudioObjectID,
    selector: AudioObjectPropertySelector,
    scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
    element: AudioObjectPropertyElement = kAudioObjectPropertyElementMain
) -> [T] {
    var address = AudioObjectPropertyAddress(
        mSelector: selector,
        mScope: scope,
        mElement: element
    )
    
    var size: UInt32 = 0
    var status = AudioObjectGetPropertyDataSize(objectID, &address, 0, nil, &size)
    if status != noErr { return [] }
    
    let count = Int(size) / MemoryLayout<T>.size
    if count == 0 { return [] }
    
    let result = [T](unsafeUninitializedCapacity: count) { buffer, initializedCount in
        let ptr = buffer.baseAddress!
        status = AudioObjectGetPropertyData(objectID, &address, 0, nil, &size, ptr)
        initializedCount = (status == noErr) ? Int(size) / MemoryLayout<T>.size : 0
    }
    
    return status == noErr ? result : []
}

func getAudioObjectStringProperty(
    objectID: AudioObjectID,
    selector: AudioObjectPropertySelector,
    scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
    element: AudioObjectPropertyElement = kAudioObjectPropertyElementMain
) -> String {
    var address = AudioObjectPropertyAddress(
        mSelector: selector,
        mScope: scope,
        mElement: element
    )
    
    var stringRef: CFString? = nil
    var size = UInt32(MemoryLayout<CFString?>.size)
    
    let status = withUnsafeMutablePointer(to: &stringRef) { ptr in
        AudioObjectGetPropertyData(objectID, &address, 0, nil, &size, ptr)
    }
    
    if status == noErr, let validRef = stringRef {
        return validRef as String
    }
    return ""
}

// MARK: - Audio Object Wrappers

class AudioObject: Identifiable, Hashable {
    let id: AudioObjectID
    
    init(id: AudioObjectID) {
        self.id = id
    }
    
    var name: String {
        getAudioObjectStringProperty(objectID: id, selector: kAudioObjectPropertyName)
    }
    
    var manufacturer: String {
        getAudioObjectStringProperty(objectID: id, selector: kAudioObjectPropertyManufacturer)
    }
    
    static func == (lhs: AudioObject, rhs: AudioObject) -> Bool {
        return lhs.id == rhs.id
    }
    
    func hash(into hasher: inout Hasher) {
        hasher.combine(id)
    }
}

// MARK: - AudioStream

class AudioStream: AudioObject {
    /// 0 = Output, 1 = Input
    var directionRaw: UInt32 {
        getAudioObjectProperty(objectID: id, selector: kAudioStreamPropertyDirection, defaultValue: 999)
    }
    
    var isInput: Bool { directionRaw == 1 }
    var isOutput: Bool { directionRaw == 0 }
    
    var direction: String {
        switch directionRaw {
        case 0: return "Output"
        case 1: return "Input"
        default: return "Unknown"
        }
    }
    
    var terminalType: String {
        let type: UInt32 = getAudioObjectProperty(objectID: id, selector: kAudioStreamPropertyTerminalType, defaultValue: 0)
        switch type {
        case kAudioStreamTerminalTypeLine: return "Line"
        case kAudioStreamTerminalTypeHeadphones: return "Headphones"
        case kAudioStreamTerminalTypeSpeaker: return "Speakers"
        case kAudioStreamTerminalTypeMicrophone: return "Microphone"
        case kAudioStreamTerminalTypeDigitalAudioInterface: return "Digital"
        case kAudioStreamTerminalTypeHDMI: return "HDMI"
        case kAudioStreamTerminalTypeDisplayPort: return "DisplayPort"
        default: return "Unknown (\(String(format: "%04x", type)))"
        }
    }
    
    var physicalFormat: AudioStreamBasicDescription? {
        var asbd = AudioStreamBasicDescription()
        var size = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
        var address = AudioObjectPropertyAddress(mSelector: kAudioStreamPropertyPhysicalFormat, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        
        let status = AudioObjectGetPropertyData(id, &address, 0, nil, &size, &asbd)
        return status == noErr ? asbd : nil
    }
    
    var virtualFormat: AudioStreamBasicDescription? {
        var asbd = AudioStreamBasicDescription()
        var size = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
        var address = AudioObjectPropertyAddress(mSelector: kAudioStreamPropertyVirtualFormat, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        
        let status = AudioObjectGetPropertyData(id, &address, 0, nil, &size, &asbd)
        return status == noErr ? asbd : nil
    }
    
    /// Available physical formats for this stream
    var availablePhysicalFormats: [AudioStreamRangedDescription] {
        getAudioObjectPropertyArray(objectID: id, selector: kAudioStreamPropertyAvailablePhysicalFormats)
    }
}

// MARK: - AudioWrapperDevice

class AudioWrapperDevice: AudioObject {
    var uid: String {
        getAudioObjectStringProperty(objectID: id, selector: kAudioDevicePropertyDeviceUID)
    }
    
    var modelUID: String {
        getAudioObjectStringProperty(objectID: id, selector: kAudioDevicePropertyModelUID)
    }
    
    var sampleRate: Float64 {
        getAudioObjectProperty(objectID: id, selector: kAudioDevicePropertyNominalSampleRate, defaultValue: 0.0)
    }
    
    var isRunning: Bool {
        let val: UInt32 = getAudioObjectProperty(objectID: id, selector: kAudioDevicePropertyDeviceIsRunning, defaultValue: 0)
        return val != 0
    }

    var transportType: TransportType {
        let val: UInt32 = getAudioObjectProperty(objectID: id, selector: kAudioDevicePropertyTransportType, defaultValue: 0)
        return TransportType(rawValue: val) ?? .unknown
    }
    
    /// All streams (both input and output)
    var allStreams: [AudioStream] {
        let streamIDs: [AudioStreamID] = getAudioObjectPropertyArray(objectID: id, selector: kAudioDevicePropertyStreams)
        return streamIDs.map { AudioStream(id: $0) }
    }
    
    var inputStreams: [AudioStream] {
        allStreams.filter { $0.isInput }
    }
    
    var outputStreams: [AudioStream] {
        allStreams.filter { $0.isOutput }
    }
    
    var inputChannelCount: Int {
        inputStreams.reduce(0) { $0 + Int($1.physicalFormat?.mChannelsPerFrame ?? 0) }
    }
    
    var outputChannelCount: Int {
        outputStreams.reduce(0) { $0 + Int($1.physicalFormat?.mChannelsPerFrame ?? 0) }
    }
    
    var isDefaultInput: Bool {
        AudioSystem.shared.defaultInputDeviceID == id
    }
    
    var isDefaultOutput: Bool {
        AudioSystem.shared.defaultOutputDeviceID == id
    }
    
    /// Current buffer frame size (samples per buffer)
    var bufferFrameSize: UInt32 {
        getAudioObjectProperty(objectID: id, selector: kAudioDevicePropertyBufferFrameSize, defaultValue: 0)
    }
    
    /// Allowed buffer frame size range (min, max)
    var bufferFrameSizeRange: (min: UInt32, max: UInt32) {
        var range = AudioValueRange()
        var size = UInt32(MemoryLayout<AudioValueRange>.size)
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyBufferFrameSizeRange,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        let status = AudioObjectGetPropertyData(id, &address, 0, nil, &size, &range)
        if status == noErr {
            return (min: UInt32(range.mMinimum), max: UInt32(range.mMaximum))
        }
        return (min: 0, max: 0)
    }
    
    /// Latency in frames (device-level)
    var outputDeviceLatency: UInt32 {
        getAudioObjectProperty(
            objectID: id,
            selector: kAudioDevicePropertyLatency,
            scope: kAudioDevicePropertyScopeOutput,
            defaultValue: 0
        )
    }

    var inputDeviceLatency: UInt32 {
        getAudioObjectProperty(
            objectID: id,
            selector: kAudioDevicePropertyLatency,
            scope: kAudioDevicePropertyScopeInput,
            defaultValue: 0
        )
    }

    /// Backward-compatible aggregate (max of output/input scopes)
    var deviceLatency: UInt32 {
        max(outputDeviceLatency, inputDeviceLatency)
    }
    
    /// Safety offset in frames
    var outputSafetyOffset: UInt32 {
        getAudioObjectProperty(
            objectID: id,
            selector: kAudioDevicePropertySafetyOffset,
            scope: kAudioDevicePropertyScopeOutput,
            defaultValue: 0
        )
    }

    var inputSafetyOffset: UInt32 {
        getAudioObjectProperty(
            objectID: id,
            selector: kAudioDevicePropertySafetyOffset,
            scope: kAudioDevicePropertyScopeInput,
            defaultValue: 0
        )
    }

    /// Backward-compatible aggregate (max of output/input scopes)
    var safetyOffset: UInt32 {
        max(outputSafetyOffset, inputSafetyOffset)
    }
}

// MARK: - TransportType

enum TransportType: UInt32 {
    case builtIn    = 0x626c746e // 'bltn'
    case usb        = 0x75736220 // 'usb '
    case fireWire   = 0x31333934 // '1394'
    case bluetooth  = 0x626c7465 // 'blte'
    case hdmi       = 0x68646d69 // 'hdmi'
    case thunderbolt = 0x7468756e // 'thun'
    case pci        = 0x70636920 // 'pci '
    case virtual    = 0x76697274 // 'virt'
    case unknown    = 0
    
    var name: String {
        switch self {
        case .builtIn: return "Built-In"
        case .usb: return "USB"
        case .fireWire: return "FireWire"
        case .bluetooth: return "Bluetooth"
        case .hdmi: return "HDMI"
        case .thunderbolt: return "Thunderbolt"
        case .pci: return "PCI"
        case .virtual: return "Virtual"
        case .unknown: return "Unknown"
        }
    }
    
    var iconName: String {
        switch self {
        case .builtIn: return "macbook"
        case .usb: return "cable.connector.horizontal"
        case .fireWire: return "flame"
        case .bluetooth: return "wave.3.left"
        case .hdmi: return "tv"
        case .thunderbolt: return "bolt"
        case .pci: return "memorychip"
        case .virtual: return "waveform"
        case .unknown: return "questionmark.circle"
        }
    }
}

// MARK: - FormatFlagsInfo

/// Decoded format flags from `AudioStreamBasicDescription.mFormatFlags`
struct FormatFlagsInfo {
    let isFloat: Bool
    let isSignedInteger: Bool
    let isPacked: Bool
    let isBigEndian: Bool
    let isNonInterleaved: Bool
    let isNonMixable: Bool
    let isAlignedHigh: Bool // For non-packed formats: data is in high bits
    
    init(flags: AudioFormatFlags, formatID: AudioFormatID) {
        // These flags are only meaningful for Linear PCM
        let isLinearPCM = formatID == kAudioFormatLinearPCM
        
        self.isFloat = isLinearPCM && (flags & kAudioFormatFlagIsFloat) != 0
        self.isSignedInteger = isLinearPCM && (flags & kAudioFormatFlagIsSignedInteger) != 0
        self.isPacked = isLinearPCM && (flags & kAudioFormatFlagIsPacked) != 0
        self.isBigEndian = isLinearPCM && (flags & kAudioFormatFlagIsBigEndian) != 0
        self.isNonInterleaved = (flags & kAudioFormatFlagIsNonInterleaved) != 0
        self.isNonMixable = (flags & kAudioFormatFlagIsNonMixable) != 0
        self.isAlignedHigh = isLinearPCM && (flags & kAudioFormatFlagIsAlignedHigh) != 0
    }
    
    /// Human-readable data type string (e.g. "Float32", "SInt24 Packed", etc.)
    var dataTypeString: String {
        if isFloat {
            return "Float"
        } else if isSignedInteger {
            return isPacked ? "SInt (Packed)" : (isAlignedHigh ? "SInt (High-Aligned)" : "SInt")
        } else {
            return isPacked ? "UInt (Packed)" : (isAlignedHigh ? "UInt (High-Aligned)" : "UInt")
        }
    }
    
    var endiannessString: String {
        isBigEndian ? "Big-Endian" : "Little-Endian"
    }
}

// MARK: - AudioStreamBasicDescription Extension

extension AudioStreamBasicDescription {
    var isInterleaved: Bool {
        return (mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0
    }
    
    var formatFlags: FormatFlagsInfo {
        FormatFlagsInfo(flags: mFormatFlags, formatID: mFormatID)
    }
    
    var formatName: String {
        switch mFormatID {
        case kAudioFormatLinearPCM: return "Linear PCM"
        case kAudioFormatAC3: return "AC-3"
        case kAudioFormat60958AC3: return "AC-3 (IEC 60958)"
        case kAudioFormatMPEG4AAC: return "AAC"
        case kAudioFormatMPEG4CELP: return "CELP"
        case kAudioFormatMPEG4HVXC: return "HVXC"
        case kAudioFormatMPEG4TwinVQ: return "TwinVQ"
        case kAudioFormatAppleLossless: return "Apple Lossless"
        default:
            return formatIDToString(mFormatID)
        }
    }
    
    func formatIDToString(_ id: UInt32) -> String {
        let bytes = [
            UInt8((id >> 24) & 0xFF),
            UInt8((id >> 16) & 0xFF),
            UInt8((id >> 8) & 0xFF),
            UInt8(id & 0xFF)
        ]
        return String(bytes: bytes, encoding: .ascii)?.trimmingCharacters(in: .whitespaces) ?? "?"
    }
    
    /// Compact human-readable summary
    var summary: String {
        let flags = formatFlags
        let typeStr = flags.dataTypeString
        let bits = mBitsPerChannel > 0 ? "\(mBitsPerChannel)-bit" : ""
        let ch = mChannelsPerFrame > 0 ? "\(mChannelsPerFrame)ch" : ""
        let rate = mSampleRate > 0 ? "\(Int(mSampleRate))Hz" : ""
        
        return [formatName, typeStr, bits, ch, rate].filter { !$0.isEmpty }.joined(separator: " • ")
    }
}

// MARK: - AudioSystem

class AudioSystem {
    static let shared = AudioSystem()
    
    var devices: [AudioWrapperDevice] {
        let deviceIDs: [AudioDeviceID] = getAudioObjectPropertyArray(
            objectID: AudioObjectID(kAudioObjectSystemObject),
            selector: kAudioHardwarePropertyDevices
        )
        return deviceIDs.map { AudioWrapperDevice(id: $0) }
    }
    
    var defaultInputDeviceID: AudioDeviceID {
        getAudioObjectProperty(
            objectID: AudioObjectID(kAudioObjectSystemObject),
            selector: kAudioHardwarePropertyDefaultInputDevice,
            defaultValue: 0
        )
    }
    
    var defaultOutputDeviceID: AudioDeviceID {
        getAudioObjectProperty(
            objectID: AudioObjectID(kAudioObjectSystemObject),
            selector: kAudioHardwarePropertyDefaultOutputDevice,
            defaultValue: 0
        )
    }
    
    func findDevice(byUID uid: String) -> AudioWrapperDevice? {
        devices.first { $0.uid == uid || $0.name.contains(uid) }
    }
}
