import Foundation

private final class DuetStateCacheStore {
    let lock = NSLock()
    var snapshots: [UInt64: DuetStateSnapshot] = [:]
}

private let duetStateCacheStore = DuetStateCacheStore()

extension ASFWDriverConnector {
    static let duetVendorID: UInt32 = 0x0003DB
    static let duetModelID: UInt32 = 0x01DDDD

    // MARK: - Device Selection

    func getDuetUnitGUIDs() -> [UInt64] {
        return (getAVCUnits() ?? [])
            .filter { isDuetUnit($0) }
            .map { $0.guid }
    }

    func getFirstDuetUnitGUID() -> UInt64? {
        return getDuetUnitGUIDs().first
    }

    func isDuetUnit(_ unit: AVCUnitInfo) -> Bool {
        return unit.vendorID == Self.duetVendorID && unit.modelID == Self.duetModelID
    }

    // MARK: - Sidecar Cache

    func getDuetCachedState(guid: UInt64) -> DuetStateSnapshot? {
        duetStateCacheStore.lock.lock()
        defer { duetStateCacheStore.lock.unlock() }
        return duetStateCacheStore.snapshots[guid]
    }

    func setDuetCachedState(guid: UInt64, snapshot: DuetStateSnapshot) {
        duetStateCacheStore.lock.lock()
        duetStateCacheStore.snapshots[guid] = snapshot
        duetStateCacheStore.lock.unlock()
    }

    func clearDuetCachedState(guid: UInt64) {
        duetStateCacheStore.lock.lock()
        duetStateCacheStore.snapshots.removeValue(forKey: guid)
        duetStateCacheStore.lock.unlock()
    }

    private func updateDuetCachedState(guid: UInt64, _ mutate: (inout DuetStateSnapshot) -> Void) {
        duetStateCacheStore.lock.lock()
        var snapshot = duetStateCacheStore.snapshots[guid] ?? DuetStateSnapshot()
        mutate(&snapshot)
        snapshot.updatedAt = Date()
        duetStateCacheStore.snapshots[guid] = snapshot
        duetStateCacheStore.lock.unlock()
    }

    // MARK: - Typed Duet API (Vendor-dependent over raw FCP)

    func refreshDuetState(guid: UInt64, timeoutMs: UInt32 = 15_000) -> DuetStateSnapshot? {
        let knob = getDuetKnobState(guid: guid, timeoutMs: timeoutMs)
        let output = getDuetOutputParams(guid: guid, timeoutMs: timeoutMs)
        let input = getDuetInputParams(guid: guid, timeoutMs: timeoutMs)
        let mixer = getDuetMixerParams(guid: guid, timeoutMs: timeoutMs)
        let display = getDuetDisplayParams(guid: guid, timeoutMs: timeoutMs)
        let firmware = getDuetFirmwareID(guid: guid)
        let hardware = getDuetHardwareID(guid: guid)

        if knob == nil && output == nil && input == nil && mixer == nil && display == nil {
            return nil
        }

        updateDuetCachedState(guid: guid) { snapshot in
            snapshot.knobState = knob ?? snapshot.knobState
            snapshot.outputParams = output ?? snapshot.outputParams
            snapshot.inputParams = input ?? snapshot.inputParams
            snapshot.mixerParams = mixer ?? snapshot.mixerParams
            snapshot.displayParams = display ?? snapshot.displayParams
            snapshot.firmwareID = firmware ?? snapshot.firmwareID
            snapshot.hardwareID = hardware ?? snapshot.hardwareID
        }

        return getDuetCachedState(guid: guid)
    }

    func getDuetKnobState(guid: UInt64, timeoutMs: UInt32 = 15_000) -> DuetKnobState? {
        guard let raw = duetReadHwState(guid: guid, timeoutMs: timeoutMs), raw.count >= 11 else {
            return nil
        }

        var state = DuetKnobState()
        state.outputMute = raw[0] > 0
        state.target = DuetKnobTarget(rawValue: raw[1]) ?? .outputPair0

        let inverted = Int(DuetKnobState.outputVolumeMax) - Int(raw[3])
        state.outputVolume = UInt8(max(Int(DuetKnobState.outputVolumeMin), min(Int(DuetKnobState.outputVolumeMax), inverted)))

        state.inputGains = [raw[4], raw[5]]

        updateDuetCachedState(guid: guid) { $0.knobState = state }
        return state
    }

    func setDuetKnobState(guid: UInt64, state: DuetKnobState, timeoutMs: UInt32 = 15_000) -> Bool {
        var payload = [UInt8](repeating: 0, count: 11)
        payload[0] = state.outputMute ? 1 : 0
        payload[1] = state.target.rawValue

        let clampedOutput = max(Int(DuetKnobState.outputVolumeMin), min(Int(DuetKnobState.outputVolumeMax), Int(state.outputVolume)))
        payload[3] = UInt8(Int(DuetKnobState.outputVolumeMax) - clampedOutput)

        if state.inputGains.count >= 2 {
            payload[4] = state.inputGains[0]
            payload[5] = state.inputGains[1]
        }

        guard duetWriteHwState(guid: guid, payload: payload, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(guid: guid) { $0.knobState = state }
        return true
    }

    func getDuetOutputParams(guid: UInt64, timeoutMs: UInt32 = 15_000) -> DuetOutputParams? {
        guard let mute = duetReadBool(guid: guid, code: .outMute, timeoutMs: timeoutMs),
              let volume = duetReadU8(guid: guid, code: .outVolume, timeoutMs: timeoutMs),
              let sourceIsMixer = duetReadBool(guid: guid, code: .outSourceIsMixer, timeoutMs: timeoutMs),
              let isConsumer = duetReadBool(guid: guid, code: .outIsConsumerLevel, timeoutMs: timeoutMs),
              let lineMute = duetReadBool(guid: guid, code: .muteForLineOut, timeoutMs: timeoutMs),
              let lineUnmute = duetReadBool(guid: guid, code: .unmuteForLineOut, timeoutMs: timeoutMs),
              let hpMute = duetReadBool(guid: guid, code: .muteForHpOut, timeoutMs: timeoutMs),
              let hpUnmute = duetReadBool(guid: guid, code: .unmuteForHpOut, timeoutMs: timeoutMs)
        else {
            return nil
        }

        let params = DuetOutputParams(
            mute: mute,
            volume: volume,
            source: sourceIsMixer ? .mixerOutputPair0 : .streamInputPair0,
            nominalLevel: isConsumer ? .consumer : .instrument,
            lineMuteMode: .fromWire(mute: lineMute, unmute: lineUnmute),
            hpMuteMode: .fromWire(mute: hpMute, unmute: hpUnmute)
        )

        updateDuetCachedState(guid: guid) { $0.outputParams = params }
        return params
    }

    func setDuetOutputParams(guid: UInt64, params: DuetOutputParams, timeoutMs: UInt32 = 15_000) -> Bool {
        let lineFlags = params.lineMuteMode.toWireFlags()
        let hpFlags = params.hpMuteMode.toWireFlags()

        let ok =
            duetWriteBool(guid: guid, code: .outMute, value: params.mute, timeoutMs: timeoutMs) &&
            duetWriteU8(guid: guid, code: .outVolume, value: params.volume, timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .outSourceIsMixer, value: params.source == .mixerOutputPair0, timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .outIsConsumerLevel, value: params.nominalLevel == .consumer, timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .muteForLineOut, value: lineFlags.mute, timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .unmuteForLineOut, value: lineFlags.unmute, timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .muteForHpOut, value: hpFlags.mute, timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .unmuteForHpOut, value: hpFlags.unmute, timeoutMs: timeoutMs)

        if ok {
            updateDuetCachedState(guid: guid) { $0.outputParams = params }
        }
        return ok
    }

    func setDuetOutputMute(guid: UInt64, enabled: Bool, timeoutMs: UInt32 = 15_000) -> Bool {
        guard duetWriteBool(guid: guid, code: .outMute, value: enabled, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(guid: guid) { snapshot in
            if var output = snapshot.outputParams {
                output.mute = enabled
                snapshot.outputParams = output
            }
        }
        return true
    }

    func setDuetOutputVolume(guid: UInt64, volume: UInt8, timeoutMs: UInt32 = 15_000) -> Bool {
        let clamped = max(DuetOutputParams.volumeMin, min(DuetOutputParams.volumeMax, volume))
        guard duetWriteU8(guid: guid, code: .outVolume, value: clamped, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(guid: guid) { snapshot in
            if var output = snapshot.outputParams {
                output.volume = clamped
                snapshot.outputParams = output
            }
        }
        return true
    }

    func setDuetOutputSource(guid: UInt64, source: DuetOutputSource, timeoutMs: UInt32 = 15_000) -> Bool {
        let isMixer = (source == .mixerOutputPair0)
        guard duetWriteBool(guid: guid, code: .outSourceIsMixer, value: isMixer, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(guid: guid) { snapshot in
            if var output = snapshot.outputParams {
                output.source = source
                snapshot.outputParams = output
            }
        }
        return true
    }

    func setDuetOutputNominalLevel(guid: UInt64, level: DuetOutputNominalLevel, timeoutMs: UInt32 = 15_000) -> Bool {
        let isConsumer = (level == .consumer)
        guard duetWriteBool(guid: guid, code: .outIsConsumerLevel, value: isConsumer, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(guid: guid) { snapshot in
            if var output = snapshot.outputParams {
                output.nominalLevel = level
                snapshot.outputParams = output
            }
        }
        return true
    }

    func setDuetOutputLineMuteMode(guid: UInt64, mode: DuetOutputMuteMode, timeoutMs: UInt32 = 15_000) -> Bool {
        let flags = mode.toWireFlags()
        let ok =
            duetWriteBool(guid: guid, code: .muteForLineOut, value: flags.mute, timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .unmuteForLineOut, value: flags.unmute, timeoutMs: timeoutMs)
        guard ok else {
            return false
        }

        updateDuetCachedState(guid: guid) { snapshot in
            if var output = snapshot.outputParams {
                output.lineMuteMode = mode
                snapshot.outputParams = output
            }
        }
        return true
    }

    func setDuetOutputHPMuteMode(guid: UInt64, mode: DuetOutputMuteMode, timeoutMs: UInt32 = 15_000) -> Bool {
        let flags = mode.toWireFlags()
        let ok =
            duetWriteBool(guid: guid, code: .muteForHpOut, value: flags.mute, timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .unmuteForHpOut, value: flags.unmute, timeoutMs: timeoutMs)
        guard ok else {
            return false
        }

        updateDuetCachedState(guid: guid) { snapshot in
            if var output = snapshot.outputParams {
                output.hpMuteMode = mode
                snapshot.outputParams = output
            }
        }
        return true
    }

    func getDuetInputParams(guid: UInt64, timeoutMs: UInt32 = 15_000) -> DuetInputParams? {
        guard let gain0 = duetReadU8(guid: guid, code: .inGain, index: 0, timeoutMs: timeoutMs),
              let gain1 = duetReadU8(guid: guid, code: .inGain, index: 1, timeoutMs: timeoutMs),
              let polarity0 = duetReadBool(guid: guid, code: .micPolarity, index: 0, timeoutMs: timeoutMs),
              let polarity1 = duetReadBool(guid: guid, code: .micPolarity, index: 1, timeoutMs: timeoutMs),
              let micLevel0 = duetReadBool(guid: guid, code: .xlrIsMicLevel, index: 0, timeoutMs: timeoutMs),
              let micLevel1 = duetReadBool(guid: guid, code: .xlrIsMicLevel, index: 1, timeoutMs: timeoutMs),
              let consumer0 = duetReadBool(guid: guid, code: .xlrIsConsumerLevel, index: 0, timeoutMs: timeoutMs),
              let consumer1 = duetReadBool(guid: guid, code: .xlrIsConsumerLevel, index: 1, timeoutMs: timeoutMs),
              let phantom0 = duetReadBool(guid: guid, code: .micPhantom, index: 0, timeoutMs: timeoutMs),
              let phantom1 = duetReadBool(guid: guid, code: .micPhantom, index: 1, timeoutMs: timeoutMs),
              let sourcePhone0 = duetReadBool(guid: guid, code: .inputSourceIsPhone, index: 0, timeoutMs: timeoutMs),
              let sourcePhone1 = duetReadBool(guid: guid, code: .inputSourceIsPhone, index: 1, timeoutMs: timeoutMs),
              let clickless = duetReadBool(guid: guid, code: .inClickless, timeoutMs: timeoutMs)
        else {
            return nil
        }

        let params = DuetInputParams(
            gains: [gain0, gain1],
            polarities: [polarity0, polarity1],
            xlrNominalLevels: [
                .fromWire(isMicLevel: micLevel0, isConsumerLevel: consumer0),
                .fromWire(isMicLevel: micLevel1, isConsumerLevel: consumer1)
            ],
            phantomPowerings: [phantom0, phantom1],
            sources: [sourcePhone0 ? .phone : .xlr, sourcePhone1 ? .phone : .xlr],
            clickless: clickless
        )

        updateDuetCachedState(guid: guid) { $0.inputParams = params }
        return params
    }

    func setDuetInputParams(guid: UInt64, params: DuetInputParams, timeoutMs: UInt32 = 15_000) -> Bool {
        guard params.gains.count >= 2,
              params.polarities.count >= 2,
              params.xlrNominalLevels.count >= 2,
              params.phantomPowerings.count >= 2,
              params.sources.count >= 2
        else {
            return false
        }

        let ok =
            duetWriteU8(guid: guid, code: .inGain, index: 0, value: params.gains[0], timeoutMs: timeoutMs) &&
            duetWriteU8(guid: guid, code: .inGain, index: 1, value: params.gains[1], timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .micPolarity, index: 0, value: params.polarities[0], timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .micPolarity, index: 1, value: params.polarities[1], timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .micPhantom, index: 0, value: params.phantomPowerings[0], timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .micPhantom, index: 1, value: params.phantomPowerings[1], timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .inputSourceIsPhone, index: 0, value: params.sources[0] == .phone, timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .inputSourceIsPhone, index: 1, value: params.sources[1] == .phone, timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .inClickless, value: params.clickless, timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .xlrIsMicLevel, index: 0, value: params.xlrNominalLevels[0] == .microphone, timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .xlrIsMicLevel, index: 1, value: params.xlrNominalLevels[1] == .microphone, timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .xlrIsConsumerLevel, index: 0, value: params.xlrNominalLevels[0] == .consumer, timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .xlrIsConsumerLevel, index: 1, value: params.xlrNominalLevels[1] == .consumer, timeoutMs: timeoutMs)

        if ok {
            updateDuetCachedState(guid: guid) { $0.inputParams = params }
        }
        return ok
    }

    func setDuetInputGain(guid: UInt64,
                          channel: Int,
                          gain: UInt8,
                          timeoutMs: UInt32 = 15_000) -> Bool {
        guard channel >= 0 && channel < 2 else {
            return false
        }

        let clamped = max(DuetInputParams.gainMin, min(DuetInputParams.gainMax, gain))
        guard duetWriteU8(guid: guid,
                          code: .inGain,
                          index: UInt8(channel),
                          value: clamped,
                          timeoutMs: timeoutMs)
        else {
            return false
        }

        updateDuetCachedState(guid: guid) { snapshot in
            if var input = snapshot.inputParams,
               channel < input.gains.count {
                input.gains[channel] = clamped
                snapshot.inputParams = input
            }
        }
        return true
    }

    func setDuetInputSource(guid: UInt64,
                            channel: Int,
                            source: DuetInputSource,
                            timeoutMs: UInt32 = 15_000) -> Bool {
        guard channel >= 0 && channel < 2 else {
            return false
        }

        let isPhone = (source == .phone)
        guard duetWriteBool(guid: guid,
                            code: .inputSourceIsPhone,
                            index: UInt8(channel),
                            value: isPhone,
                            timeoutMs: timeoutMs)
        else {
            return false
        }

        updateDuetCachedState(guid: guid) { snapshot in
            if var input = snapshot.inputParams,
               channel < input.sources.count {
                input.sources[channel] = source
                snapshot.inputParams = input
            }
        }
        return true
    }

    func setDuetInputXlrNominalLevel(guid: UInt64,
                                     channel: Int,
                                     level: DuetInputXlrNominalLevel,
                                     timeoutMs: UInt32 = 15_000) -> Bool {
        guard channel >= 0 && channel < 2 else {
            return false
        }

        let isMic = (level == .microphone)
        let isConsumer = (level == .consumer)
        let index = UInt8(channel)

        let ok =
            duetWriteBool(guid: guid,
                          code: .xlrIsMicLevel,
                          index: index,
                          value: isMic,
                          timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid,
                          code: .xlrIsConsumerLevel,
                          index: index,
                          value: isConsumer,
                          timeoutMs: timeoutMs)
        guard ok else {
            return false
        }

        updateDuetCachedState(guid: guid) { snapshot in
            if var input = snapshot.inputParams,
               channel < input.xlrNominalLevels.count {
                input.xlrNominalLevels[channel] = level
                snapshot.inputParams = input
            }
        }
        return true
    }

    func setDuetInputPolarity(guid: UInt64,
                              channel: Int,
                              inverted: Bool,
                              timeoutMs: UInt32 = 15_000) -> Bool {
        guard channel >= 0 && channel < 2 else {
            return false
        }

        guard duetWriteBool(guid: guid,
                            code: .micPolarity,
                            index: UInt8(channel),
                            value: inverted,
                            timeoutMs: timeoutMs)
        else {
            return false
        }

        updateDuetCachedState(guid: guid) { snapshot in
            if var input = snapshot.inputParams,
               channel < input.polarities.count {
                input.polarities[channel] = inverted
                snapshot.inputParams = input
            }
        }
        return true
    }

    func setDuetInputPhantom(guid: UInt64,
                             channel: Int,
                             enabled: Bool,
                             timeoutMs: UInt32 = 15_000) -> Bool {
        guard channel >= 0 && channel < 2 else {
            return false
        }

        let index = UInt8(channel)

        guard duetWriteBool(guid: guid,
                            code: .micPhantom,
                            index: index,
                            value: enabled,
                            timeoutMs: timeoutMs)
        else {
            return false
        }

        updateDuetCachedState(guid: guid) { snapshot in
            if var input = snapshot.inputParams,
               channel < input.phantomPowerings.count {
                input.phantomPowerings[channel] = enabled
                snapshot.inputParams = input
            }
        }

        return true
    }

    func setDuetInputClickless(guid: UInt64, enabled: Bool, timeoutMs: UInt32 = 15_000) -> Bool {
        guard duetWriteBool(guid: guid, code: .inClickless, value: enabled, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(guid: guid) { snapshot in
            if var input = snapshot.inputParams {
                input.clickless = enabled
                snapshot.inputParams = input
            }
        }
        return true
    }

    func getDuetMixerParams(guid: UInt64, timeoutMs: UInt32 = 15_000) -> DuetMixerParams? {
        var params = DuetMixerParams()

        for destination in 0..<2 {
            for source in 0..<4 {
                guard let gain = duetReadU16(guid: guid,
                                             code: .mixerSrc,
                                             index: UInt8(source),
                                             index2: UInt8(destination),
                                             timeoutMs: timeoutMs)
                else {
                    return nil
                }
                params.setGain(destination: destination, source: source, value: gain)
            }
        }

        updateDuetCachedState(guid: guid) { $0.mixerParams = params }
        return params
    }

    func setDuetMixerParams(guid: UInt64, params: DuetMixerParams, timeoutMs: UInt32 = 15_000) -> Bool {
        guard params.outputs.count >= 2 else {
            return false
        }

        for destination in 0..<2 {
            for source in 0..<4 {
                let gain = params.gain(destination: destination, source: source)
                let ok = duetWriteU16(guid: guid,
                                      code: .mixerSrc,
                                      index: UInt8(source),
                                      index2: UInt8(destination),
                                      value: gain,
                                      timeoutMs: timeoutMs)
                if !ok {
                    return false
                }
            }
        }

        updateDuetCachedState(guid: guid) { $0.mixerParams = params }
        return true
    }

    func setDuetMixerGain(guid: UInt64,
                          destination: Int,
                          source: Int,
                          gain: UInt16,
                          timeoutMs: UInt32 = 15_000) -> Bool {
        guard destination >= 0 && destination < 2,
              source >= 0 && source < 4
        else {
            return false
        }

        let clamped = max(DuetMixerParams.gainMin, min(DuetMixerParams.gainMax, gain))
        guard duetWriteU16(guid: guid,
                           code: .mixerSrc,
                           index: UInt8(source),
                           index2: UInt8(destination),
                           value: clamped,
                           timeoutMs: timeoutMs)
        else {
            return false
        }

        updateDuetCachedState(guid: guid) { snapshot in
            if var mixer = snapshot.mixerParams {
                mixer.setGain(destination: destination, source: source, value: clamped)
                snapshot.mixerParams = mixer
            }
        }
        return true
    }

    func getDuetDisplayParams(guid: UInt64, timeoutMs: UInt32 = 15_000) -> DuetDisplayParams? {
        guard let isInput = duetReadBool(guid: guid, code: .displayIsInput, timeoutMs: timeoutMs),
              let follow = duetReadBool(guid: guid, code: .displayFollowToKnob, timeoutMs: timeoutMs),
              let overholdTwoSec = duetReadBool(guid: guid, code: .displayOverholdTwoSec, timeoutMs: timeoutMs)
        else {
            return nil
        }

        let params = DuetDisplayParams(
            target: isInput ? .input : .output,
            mode: follow ? .followingToKnobTarget : .independent,
            overhold: overholdTwoSec ? .twoSeconds : .infinite
        )

        updateDuetCachedState(guid: guid) { $0.displayParams = params }
        return params
    }

    func setDuetDisplayParams(guid: UInt64, params: DuetDisplayParams, timeoutMs: UInt32 = 15_000) -> Bool {
        let ok =
            duetWriteBool(guid: guid, code: .displayIsInput, value: params.target == .input, timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .displayFollowToKnob, value: params.mode == .followingToKnobTarget, timeoutMs: timeoutMs) &&
            duetWriteBool(guid: guid, code: .displayOverholdTwoSec, value: params.overhold == .twoSeconds, timeoutMs: timeoutMs)

        if ok {
            updateDuetCachedState(guid: guid) { $0.displayParams = params }
        }
        return ok
    }

    func setDuetDisplayTarget(guid: UInt64, target: DuetDisplayTarget, timeoutMs: UInt32 = 15_000) -> Bool {
        let isInput = (target == .input)
        guard duetWriteBool(guid: guid, code: .displayIsInput, value: isInput, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(guid: guid) { snapshot in
            if var display = snapshot.displayParams {
                display.target = target
                snapshot.displayParams = display
            }
        }
        return true
    }

    func setDuetDisplayMode(guid: UInt64, mode: DuetDisplayMode, timeoutMs: UInt32 = 15_000) -> Bool {
        let follow = (mode == .followingToKnobTarget)
        guard duetWriteBool(guid: guid, code: .displayFollowToKnob, value: follow, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(guid: guid) { snapshot in
            if var display = snapshot.displayParams {
                display.mode = mode
                snapshot.displayParams = display
            }
        }
        return true
    }

    func setDuetDisplayOverhold(guid: UInt64, overhold: DuetDisplayOverhold, timeoutMs: UInt32 = 15_000) -> Bool {
        let twoSec = (overhold == .twoSeconds)
        guard duetWriteBool(guid: guid, code: .displayOverholdTwoSec, value: twoSec, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(guid: guid) { snapshot in
            if var display = snapshot.displayParams {
                display.overhold = overhold
                snapshot.displayParams = display
            }
        }
        return true
    }

    func clearDuetDisplay(guid: UInt64, timeoutMs: UInt32 = 15_000) -> Bool {
        return duetWriteNoValue(guid: guid, code: .displayClear, timeoutMs: timeoutMs)
    }

    func getDuetFirmwareID(guid: UInt64) -> UInt32? {
        guard let nodeID = duetNodeID(for: guid) else {
            return nil
        }

        let value = duetReadQuadlet(destinationID: nodeID,
                                    address: 0xFFFF_F000_0000 + 0x0005_0000)
        if let value {
            updateDuetCachedState(guid: guid) { $0.firmwareID = value }
        }
        return value
    }

    func getDuetHardwareID(guid: UInt64) -> UInt32? {
        guard let nodeID = duetNodeID(for: guid) else {
            return nil
        }

        let value = duetReadQuadlet(destinationID: nodeID,
                                    address: 0xFFFF_F000_0000 + 0x0009_0020)
        if let value {
            updateDuetCachedState(guid: guid) { $0.hardwareID = value }
        }
        return value
    }

    // MARK: - Private codec helpers

    private func duetReadBool(guid: UInt64,
                              code: DuetVendorCommandCode,
                              index: UInt8? = nil,
                              index2: UInt8? = nil,
                              timeoutMs: UInt32) -> Bool? {
        guard let payload = duetExchange(guid: guid,
                                         isStatus: true,
                                         code: code,
                                         index: index,
                                         index2: index2,
                                         controlPayload: [],
                                         timeoutMs: timeoutMs),
              let value = payload.first
        else {
            return nil
        }

        return value == DuetVendorWireConstants.boolOn
    }

    private func duetReadU8(guid: UInt64,
                            code: DuetVendorCommandCode,
                            index: UInt8? = nil,
                            timeoutMs: UInt32) -> UInt8? {
        guard let payload = duetExchange(guid: guid,
                                         isStatus: true,
                                         code: code,
                                         index: index,
                                         controlPayload: [],
                                         timeoutMs: timeoutMs),
              let value = payload.first
        else {
            return nil
        }
        return value
    }

    private func duetReadU16(guid: UInt64,
                             code: DuetVendorCommandCode,
                             index: UInt8,
                             index2: UInt8,
                             timeoutMs: UInt32) -> UInt16? {
        guard let payload = duetExchange(guid: guid,
                                         isStatus: true,
                                         code: code,
                                         index: index,
                                         index2: index2,
                                         controlPayload: [],
                                         timeoutMs: timeoutMs),
              payload.count >= 2
        else {
            return nil
        }

        return (UInt16(payload[0]) << 8) | UInt16(payload[1])
    }

    private func duetReadHwState(guid: UInt64, timeoutMs: UInt32) -> [UInt8]? {
        guard let payload = duetExchange(guid: guid,
                                         isStatus: true,
                                         code: .hwState,
                                         controlPayload: [],
                                         timeoutMs: timeoutMs),
              payload.count >= 11
        else {
            return nil
        }
        return Array(payload.prefix(11))
    }

    private func duetWriteBool(guid: UInt64,
                               code: DuetVendorCommandCode,
                               index: UInt8? = nil,
                               value: Bool,
                               timeoutMs: UInt32) -> Bool {
        let byte = value ? DuetVendorWireConstants.boolOn : DuetVendorWireConstants.boolOff
        return duetExchange(guid: guid,
                            isStatus: false,
                            code: code,
                            index: index,
                            controlPayload: [byte],
                            timeoutMs: timeoutMs) != nil
    }

    private func duetWriteU8(guid: UInt64,
                             code: DuetVendorCommandCode,
                             index: UInt8? = nil,
                             value: UInt8,
                             timeoutMs: UInt32) -> Bool {
        return duetExchange(guid: guid,
                            isStatus: false,
                            code: code,
                            index: index,
                            controlPayload: [value],
                            timeoutMs: timeoutMs) != nil
    }

    private func duetWriteU16(guid: UInt64,
                              code: DuetVendorCommandCode,
                              index: UInt8,
                              index2: UInt8,
                              value: UInt16,
                              timeoutMs: UInt32) -> Bool {
        let payload: [UInt8] = [UInt8((value >> 8) & 0xFF), UInt8(value & 0xFF)]
        return duetExchange(guid: guid,
                            isStatus: false,
                            code: code,
                            index: index,
                            index2: index2,
                            controlPayload: payload,
                            timeoutMs: timeoutMs) != nil
    }

    private func duetWriteHwState(guid: UInt64, payload: [UInt8], timeoutMs: UInt32) -> Bool {
        guard payload.count == 11 else {
            return false
        }

        return duetExchange(guid: guid,
                            isStatus: false,
                            code: .hwState,
                            controlPayload: payload,
                            timeoutMs: timeoutMs) != nil
    }

    private func duetWriteNoValue(guid: UInt64,
                                  code: DuetVendorCommandCode,
                                  timeoutMs: UInt32) -> Bool {
        return duetExchange(guid: guid,
                            isStatus: false,
                            code: code,
                            controlPayload: [],
                            timeoutMs: timeoutMs) != nil
    }

    private func duetExchange(guid: UInt64,
                              isStatus: Bool,
                              code: DuetVendorCommandCode,
                              index: UInt8? = nil,
                              index2: UInt8? = nil,
                              controlPayload: [UInt8],
                              timeoutMs: UInt32) -> [UInt8]? {
        guard let frame = DuetVendorCodec.buildFrame(isStatus: isStatus,
                                                     code: code,
                                                     index: index,
                                                     index2: index2,
                                                     controlPayload: controlPayload),
              let response = sendRawFCPCommand(guid: guid, frame: frame, timeoutMs: timeoutMs),
              let payload = DuetVendorCodec.parseStatusPayload(response,
                                                               expectedCode: code,
                                                               expectedIndex: index,
                                                               expectedIndex2: index2)
        else {
            return nil
        }

        return payload
    }

    private func duetNodeID(for guid: UInt64) -> UInt16? {
        return getAVCUnits()?.first(where: { $0.guid == guid })?.nodeID
    }

    private func duetReadQuadlet(destinationID: UInt16, address: UInt64) -> UInt32? {
        guard let data = duetSyncAsyncRead(destinationID: destinationID, address: address, length: 4),
              data.count >= 4
        else {
            return nil
        }

        return (UInt32(data[0]) << 24) |
               (UInt32(data[1]) << 16) |
               (UInt32(data[2]) << 8) |
               UInt32(data[3])
    }

    private func duetSyncAsyncRead(destinationID: UInt16, address: UInt64, length: UInt32) -> Data? {
        let addressHigh = UInt16((address >> 32) & 0xFFFF)
        let addressLow = UInt32(address & 0xFFFF_FFFF)

        var input = Data(capacity: 12)
        input.append(contentsOf: withUnsafeBytes(of: destinationID.bigEndian) { Data($0) })
        input.append(contentsOf: withUnsafeBytes(of: addressHigh.bigEndian) { Data($0) })
        input.append(contentsOf: withUnsafeBytes(of: addressLow.bigEndian) { Data($0) })
        input.append(contentsOf: withUnsafeBytes(of: length.bigEndian) { Data($0) })

        return callStruct(.asyncRead, input: input, initialCap: Int(length) + 128)
    }
}
