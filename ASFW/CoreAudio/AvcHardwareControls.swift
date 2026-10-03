import CoreAudio
import Foundation

/// HAL values are presentation of the driver's confirmed hardware state.
struct AvcHardwareControl: Identifiable, Sendable {
    let id: String
    let name: String
    let scope: UInt32
    let volumeID: UInt32?
    let muteID: UInt32?
    let decibels: Float?
    let minimum: Double?
    let maximum: Double?
    let muted: Bool?
    let volumeWritable: Bool
    let muteWritable: Bool
}

/// Serializes potentially blocking HAL callbacks away from the UI actor.
actor AvcHardwareControls {
    private func read<T>(_ object: UInt32, _ selector: UInt32, _ initial: T) -> T? {
        var address = AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var value = initial
        var size = UInt32(MemoryLayout<T>.size)
        let status = withUnsafeMutablePointer(to: &value) {
            AudioObjectGetPropertyData(object, &address, 0, nil, &size, $0)
        }
        return status == noErr && size == MemoryLayout<T>.size ? value : nil
    }
    private func list(_ object: UInt32, _ selector: UInt32) -> [UInt32] {
        var address = AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(object, &address, 0, nil, &size) == noErr,
              size > 0, size % 4 == 0 else { return [] }
        var values = [UInt32](repeating: 0, count: Int(size / 4))
        let status = values.withUnsafeMutableBytes {
            AudioObjectGetPropertyData(object, &address, 0, nil, &size, $0.baseAddress!)
        }
        return status == noErr ? Array(values.prefix(Int(size / 4))) : []
    }
    private func string(_ object: UInt32, _ selector: UInt32) -> String? {
        guard let value = read(object, selector, Optional<CFString>.none) else { return nil }
        return value as String?
    }
    private func writable(_ object: UInt32, _ selector: UInt32) -> Bool {
        var address = AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var settable: DarwinBoolean = false
        return AudioObjectIsPropertySettable(object, &address, &settable) == noErr && settable.boolValue
    }
    func load(guid: UInt64) -> [AvcHardwareControl] {
        let uid = String(format: "ASFW-%016llX", guid)
        let devices = list(UInt32(kAudioObjectSystemObject), kAudioHardwarePropertyDevices)
        guard let device = devices.first(where: { string($0, kAudioDevicePropertyDeviceUID) == uid }) else { return [] }
        struct Pair { var name: String; var scope: UInt32; var volume: UInt32?; var mute: UInt32? }
        var pairs: [String: Pair] = [:]
        for object in list(device, kAudioObjectPropertyControlList) {
            guard let kind = read(object, kAudioObjectPropertyClass, UInt32(0)),
                  kind == kAudioVolumeControlClassID || kind == kAudioMuteControlClassID,
                  let scope = read(object, kAudioControlPropertyScope, UInt32(0)),
                  let element = read(object, kAudioControlPropertyElement, UInt32(0)) else { continue }
            let key = "\(scope):\(element)"
            var pair = pairs[key] ?? Pair(name: string(object, kAudioObjectPropertyName) ?? "Hardware control \(element)", scope: scope)
            // Ambiguous HAL addresses are not writable through this UI.
            if kind == kAudioVolumeControlClassID { pair.volume = pair.volume == nil ? object : 0 }
            else { pair.mute = pair.mute == nil ? object : 0 }
            pairs[key] = pair
        }
        return pairs.map { key, pair in
            let volume = pair.volume.flatMap { $0 == 0 ? nil : $0 }
            let mute = pair.mute.flatMap { $0 == 0 ? nil : $0 }
            let range = volume.flatMap { read($0, kAudioLevelControlPropertyDecibelRange, AudioValueRange()) }
            let valid = range.map { $0.mMinimum.isFinite && $0.mMaximum.isFinite && $0.mMinimum < $0.mMaximum } ?? false
            return AvcHardwareControl(id: key, name: pair.name, scope: pair.scope, volumeID: volume, muteID: mute,
                decibels: volume.flatMap { read($0, kAudioLevelControlPropertyDecibelValue, Float(0)) },
                minimum: valid ? range?.mMinimum : nil, maximum: valid ? range?.mMaximum : nil,
                muted: mute.flatMap { read($0, kAudioBooleanControlPropertyValue, UInt32(0)).map { $0 != 0 } },
                volumeWritable: valid && volume.map { writable($0, kAudioLevelControlPropertyDecibelValue) } == true,
                muteWritable: mute.map { writable($0, kAudioBooleanControlPropertyValue) } == true)
        }.sorted {
            $0.name.replacingOccurrences(of: "Master 0", with: "Channel 0")
                .localizedStandardCompare($1.name.replacingOccurrences(of: "Master 0", with: "Channel 0")) == .orderedAscending
        }
    }
    func set(guid: UInt64, key: String, decibels: Float? = nil, muted: Bool? = nil) throws {
        // Re-resolve identity and control objects before each write: HAL IDs can
        // disappear/recycle on disconnect. Never retain IDs across a replug.
        guard let control = load(guid: guid).first(where: { $0.id == key }) else { throw Failure.unavailable }
        if let decibels {
            guard decibels.isFinite, control.volumeWritable, let object = control.volumeID,
                  let low = control.minimum, let high = control.maximum,
                  Double(decibels) >= low, Double(decibels) <= high else { throw Failure.unavailable }
            try write(object, kAudioLevelControlPropertyDecibelValue, decibels)
        }
        if let muted {
            guard control.muteWritable, let object = control.muteID else { throw Failure.unavailable }
            try write(object, kAudioBooleanControlPropertyValue, UInt32(muted ? 1 : 0))
        }
    }
    private func write<T>(_ object: UInt32, _ selector: UInt32, _ value: T) throws {
        var value = value
        var address = AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        let status = withUnsafePointer(to: &value) {
            AudioObjectSetPropertyData(object, &address, 0, nil, UInt32(MemoryLayout<T>.size), $0)
        }
        guard status == noErr else { throw Failure.rejected(status) }
    }
    enum Failure: LocalizedError {
        case unavailable, rejected(OSStatus)
        var errorDescription: String? {
            switch self {
            case .unavailable: "The hardware control is unavailable or no longer writable."
            case .rejected(let status): "The device rejected the change (Core Audio error \(status))."
            }
        }
    }
}
