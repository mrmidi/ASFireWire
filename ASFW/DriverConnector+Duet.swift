import Foundation

final class DriverConnectorDuetStateCacheStore {
    struct Entry {
        let generation: UInt32
        var snapshot: DuetStateSnapshot
    }

    let lock = NSLock()
    var snapshots: [UnitInstanceID: Entry] = [:]
    var routeGenerations: [DeviceInstanceID: UInt32] = [:]
}

extension ASFWDriverConnector {
    static let duetVendorID: UInt32 = 0x0003DB
    static let duetModelID: UInt32 = 0x01DDDD

    // MARK: - Device Selection

    func getDuetUnitIDs() -> [UnitInstanceID] {
        return (getAVCUnits() ?? [])
            .filter { isDuetUnit($0) }
            .map(\.id)
    }

    func getFirstDuetUnitID() -> UnitInstanceID? {
        return getDuetUnitIDs().first
    }

    func isDuetUnit(_ unit: AVCUnitInfo) -> Bool {
        return unit.vendorID == Self.duetVendorID && unit.modelID == Self.duetModelID
    }

    // MARK: - Sidecar Cache

    func getDuetCachedState(unitID: UnitInstanceID) -> DuetStateSnapshot? {
        duetStateCacheStore.lock.lock()
        defer { duetStateCacheStore.lock.unlock() }
        guard let entry = duetStateCacheStore.snapshots[unitID],
              entry.generation == duetStateCacheStore.routeGenerations[unitID.device, default: 0] else {
            return nil
        }
        return entry.snapshot
    }

    func setDuetCachedState(unitID: UnitInstanceID, snapshot: DuetStateSnapshot) {
        duetStateCacheStore.lock.lock()
        let generation = duetStateCacheStore.routeGenerations[unitID.device, default: 0]
        duetStateCacheStore.snapshots[unitID] = .init(
            generation: generation,
            snapshot: snapshot
        )
        duetStateCacheStore.lock.unlock()
    }

    func clearDuetCachedState(unitID: UnitInstanceID) {
        duetStateCacheStore.lock.lock()
        duetStateCacheStore.snapshots.removeValue(forKey: unitID)
        duetStateCacheStore.lock.unlock()
    }

    private func updateDuetCachedState(unitID: UnitInstanceID, _ mutate: (inout DuetStateSnapshot) -> Void) {
        duetStateCacheStore.lock.lock()
        let generation = duetStateCacheStore.routeGenerations[unitID.device, default: 0]
        var snapshot = duetStateCacheStore.snapshots[unitID]
            .flatMap { $0.generation == generation ? $0.snapshot : nil }
            ?? DuetStateSnapshot()
        mutate(&snapshot)
        snapshot.updatedAt = Date()
        duetStateCacheStore.snapshots[unitID] = .init(
            generation: generation,
            snapshot: snapshot
        )
        duetStateCacheStore.lock.unlock()
    }

    /// Replace the complete route-generation snapshot. Unit IDs intentionally
    /// survive an uninterrupted reset, but cached device state does not: a
    /// generation change invalidates every entry for that physical instance.
    func reconcileDuetCachedStateRoutes(_ routes: [DeviceInstanceID: UInt32]) {
        duetStateCacheStore.lock.lock()
        duetStateCacheStore.routeGenerations = routes
        duetStateCacheStore.snapshots = duetStateCacheStore.snapshots.filter { unitID, entry in
            routes[unitID.device] == entry.generation
        }
        duetStateCacheStore.lock.unlock()
    }

    // MARK: - Typed Duet API (Vendor-dependent over raw FCP)

    func refreshDuetState(unitID: UnitInstanceID, timeoutMs: UInt32 = 15_000) -> DuetStateSnapshot? {
        let knob = getDuetKnobState(unitID: unitID, timeoutMs: timeoutMs)
        let output = getDuetOutputParams(unitID: unitID, timeoutMs: timeoutMs)
        let input = getDuetInputParams(unitID: unitID, timeoutMs: timeoutMs)
        let mixer = getDuetMixerParams(unitID: unitID, timeoutMs: timeoutMs)
        let display = getDuetDisplayParams(unitID: unitID, timeoutMs: timeoutMs)
        let firmware = getDuetFirmwareID(unitID: unitID)
        let hardware = getDuetHardwareID(unitID: unitID)

        if knob == nil && output == nil && input == nil && mixer == nil && display == nil {
            return nil
        }

        updateDuetCachedState(unitID: unitID) { snapshot in
            snapshot.knobState = knob ?? snapshot.knobState
            snapshot.outputParams = output ?? snapshot.outputParams
            snapshot.inputParams = input ?? snapshot.inputParams
            snapshot.mixerParams = mixer ?? snapshot.mixerParams
            snapshot.displayParams = display ?? snapshot.displayParams
            snapshot.firmwareID = firmware ?? snapshot.firmwareID
            snapshot.hardwareID = hardware ?? snapshot.hardwareID
        }

        return getDuetCachedState(unitID: unitID)
    }

    func getDuetKnobState(unitID: UnitInstanceID, timeoutMs: UInt32 = 15_000) -> DuetKnobState? {
        guard let raw = duetReadHwState(unitID: unitID, timeoutMs: timeoutMs), raw.count >= 11 else {
            return nil
        }

        var state = DuetKnobState()
        state.outputMute = raw[0] > 0
        state.target = DuetKnobTarget(rawValue: raw[1]) ?? .outputPair0

        let inverted = Int(DuetKnobState.outputVolumeMax) - Int(raw[3])
        state.outputVolume = UInt8(max(Int(DuetKnobState.outputVolumeMin), min(Int(DuetKnobState.outputVolumeMax), inverted)))

        state.inputGains = [raw[4], raw[5]]

        updateDuetCachedState(unitID: unitID) { $0.knobState = state }
        return state
    }

    func setDuetKnobState(unitID: UnitInstanceID, state: DuetKnobState, timeoutMs: UInt32 = 15_000) -> Bool {
        var payload = [UInt8](repeating: 0, count: 11)
        payload[0] = state.outputMute ? 1 : 0
        payload[1] = state.target.rawValue

        let clampedOutput = max(Int(DuetKnobState.outputVolumeMin), min(Int(DuetKnobState.outputVolumeMax), Int(state.outputVolume)))
        payload[3] = UInt8(Int(DuetKnobState.outputVolumeMax) - clampedOutput)

        if state.inputGains.count >= 2 {
            payload[4] = state.inputGains[0]
            payload[5] = state.inputGains[1]
        }

        guard duetWriteHwState(unitID: unitID, payload: payload, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(unitID: unitID) { $0.knobState = state }
        return true
    }

    func getDuetOutputParams(unitID: UnitInstanceID, timeoutMs: UInt32 = 15_000) -> DuetOutputParams? {
        guard let mute = duetReadBool(unitID: unitID, code: .outMute, timeoutMs: timeoutMs),
              let volume = duetReadU8(unitID: unitID, code: .outVolume, timeoutMs: timeoutMs),
              let sourceIsMixer = duetReadBool(unitID: unitID, code: .outSourceIsMixer, timeoutMs: timeoutMs),
              let isConsumer = duetReadBool(unitID: unitID, code: .outIsConsumerLevel, timeoutMs: timeoutMs),
              let lineMute = duetReadBool(unitID: unitID, code: .muteForLineOut, timeoutMs: timeoutMs),
              let lineUnmute = duetReadBool(unitID: unitID, code: .unmuteForLineOut, timeoutMs: timeoutMs),
              let hpMute = duetReadBool(unitID: unitID, code: .muteForHpOut, timeoutMs: timeoutMs),
              let hpUnmute = duetReadBool(unitID: unitID, code: .unmuteForHpOut, timeoutMs: timeoutMs)
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

        updateDuetCachedState(unitID: unitID) { $0.outputParams = params }
        return params
    }

    func setDuetOutputParams(unitID: UnitInstanceID, params: DuetOutputParams, timeoutMs: UInt32 = 15_000) -> Bool {
        let lineFlags = params.lineMuteMode.toWireFlags()
        let hpFlags = params.hpMuteMode.toWireFlags()

        let ok =
            duetWriteBool(unitID: unitID, code: .outMute, value: params.mute, timeoutMs: timeoutMs) &&
            duetWriteU8(unitID: unitID, code: .outVolume, value: params.volume, timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .outSourceIsMixer, value: params.source == .mixerOutputPair0, timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .outIsConsumerLevel, value: params.nominalLevel == .consumer, timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .muteForLineOut, value: lineFlags.mute, timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .unmuteForLineOut, value: lineFlags.unmute, timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .muteForHpOut, value: hpFlags.mute, timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .unmuteForHpOut, value: hpFlags.unmute, timeoutMs: timeoutMs)

        if ok {
            updateDuetCachedState(unitID: unitID) { $0.outputParams = params }
        }
        return ok
    }

    func setDuetOutputMute(unitID: UnitInstanceID, enabled: Bool, timeoutMs: UInt32 = 15_000) -> Bool {
        guard duetWriteBool(unitID: unitID, code: .outMute, value: enabled, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(unitID: unitID) { snapshot in
            if var output = snapshot.outputParams {
                output.mute = enabled
                snapshot.outputParams = output
            }
        }
        return true
    }

    func setDuetOutputVolume(unitID: UnitInstanceID, volume: UInt8, timeoutMs: UInt32 = 15_000) -> Bool {
        let clamped = max(DuetOutputParams.volumeMin, min(DuetOutputParams.volumeMax, volume))
        guard duetWriteU8(unitID: unitID, code: .outVolume, value: clamped, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(unitID: unitID) { snapshot in
            if var output = snapshot.outputParams {
                output.volume = clamped
                snapshot.outputParams = output
            }
        }
        return true
    }

    func setDuetOutputSource(unitID: UnitInstanceID, source: DuetOutputSource, timeoutMs: UInt32 = 15_000) -> Bool {
        let isMixer = (source == .mixerOutputPair0)
        guard duetWriteBool(unitID: unitID, code: .outSourceIsMixer, value: isMixer, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(unitID: unitID) { snapshot in
            if var output = snapshot.outputParams {
                output.source = source
                snapshot.outputParams = output
            }
        }
        return true
    }

    func setDuetOutputNominalLevel(unitID: UnitInstanceID, level: DuetOutputNominalLevel, timeoutMs: UInt32 = 15_000) -> Bool {
        let isConsumer = (level == .consumer)
        guard duetWriteBool(unitID: unitID, code: .outIsConsumerLevel, value: isConsumer, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(unitID: unitID) { snapshot in
            if var output = snapshot.outputParams {
                output.nominalLevel = level
                snapshot.outputParams = output
            }
        }
        return true
    }

    func setDuetOutputLineMuteMode(unitID: UnitInstanceID, mode: DuetOutputMuteMode, timeoutMs: UInt32 = 15_000) -> Bool {
        let flags = mode.toWireFlags()
        let ok =
            duetWriteBool(unitID: unitID, code: .muteForLineOut, value: flags.mute, timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .unmuteForLineOut, value: flags.unmute, timeoutMs: timeoutMs)
        guard ok else {
            return false
        }

        updateDuetCachedState(unitID: unitID) { snapshot in
            if var output = snapshot.outputParams {
                output.lineMuteMode = mode
                snapshot.outputParams = output
            }
        }
        return true
    }

    func setDuetOutputHPMuteMode(unitID: UnitInstanceID, mode: DuetOutputMuteMode, timeoutMs: UInt32 = 15_000) -> Bool {
        let flags = mode.toWireFlags()
        let ok =
            duetWriteBool(unitID: unitID, code: .muteForHpOut, value: flags.mute, timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .unmuteForHpOut, value: flags.unmute, timeoutMs: timeoutMs)
        guard ok else {
            return false
        }

        updateDuetCachedState(unitID: unitID) { snapshot in
            if var output = snapshot.outputParams {
                output.hpMuteMode = mode
                snapshot.outputParams = output
            }
        }
        return true
    }

    func getDuetInputParams(unitID: UnitInstanceID, timeoutMs: UInt32 = 15_000) -> DuetInputParams? {
        guard let gain0 = duetReadU8(unitID: unitID, code: .inGain, index: 0, timeoutMs: timeoutMs),
              let gain1 = duetReadU8(unitID: unitID, code: .inGain, index: 1, timeoutMs: timeoutMs),
              let polarity0 = duetReadBool(unitID: unitID, code: .micPolarity, index: 0, timeoutMs: timeoutMs),
              let polarity1 = duetReadBool(unitID: unitID, code: .micPolarity, index: 1, timeoutMs: timeoutMs),
              let micLevel0 = duetReadBool(unitID: unitID, code: .xlrIsMicLevel, index: 0, timeoutMs: timeoutMs),
              let micLevel1 = duetReadBool(unitID: unitID, code: .xlrIsMicLevel, index: 1, timeoutMs: timeoutMs),
              let consumer0 = duetReadBool(unitID: unitID, code: .xlrIsConsumerLevel, index: 0, timeoutMs: timeoutMs),
              let consumer1 = duetReadBool(unitID: unitID, code: .xlrIsConsumerLevel, index: 1, timeoutMs: timeoutMs),
              let phantom0 = duetReadBool(unitID: unitID, code: .micPhantom, index: 0, timeoutMs: timeoutMs),
              let phantom1 = duetReadBool(unitID: unitID, code: .micPhantom, index: 1, timeoutMs: timeoutMs),
              let sourcePhone0 = duetReadBool(unitID: unitID, code: .inputSourceIsPhone, index: 0, timeoutMs: timeoutMs),
              let sourcePhone1 = duetReadBool(unitID: unitID, code: .inputSourceIsPhone, index: 1, timeoutMs: timeoutMs),
              let clickless = duetReadBool(unitID: unitID, code: .inClickless, timeoutMs: timeoutMs)
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

        updateDuetCachedState(unitID: unitID) { $0.inputParams = params }
        return params
    }

    func setDuetInputParams(unitID: UnitInstanceID, params: DuetInputParams, timeoutMs: UInt32 = 15_000) -> Bool {
        guard params.gains.count >= 2,
              params.polarities.count >= 2,
              params.xlrNominalLevels.count >= 2,
              params.phantomPowerings.count >= 2,
              params.sources.count >= 2
        else {
            return false
        }

        let ok =
            duetWriteU8(unitID: unitID, code: .inGain, index: 0, value: params.gains[0], timeoutMs: timeoutMs) &&
            duetWriteU8(unitID: unitID, code: .inGain, index: 1, value: params.gains[1], timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .micPolarity, index: 0, value: params.polarities[0], timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .micPolarity, index: 1, value: params.polarities[1], timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .micPhantom, index: 0, value: params.phantomPowerings[0], timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .micPhantom, index: 1, value: params.phantomPowerings[1], timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .inputSourceIsPhone, index: 0, value: params.sources[0] == .phone, timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .inputSourceIsPhone, index: 1, value: params.sources[1] == .phone, timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .inClickless, value: params.clickless, timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .xlrIsMicLevel, index: 0, value: params.xlrNominalLevels[0] == .microphone, timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .xlrIsMicLevel, index: 1, value: params.xlrNominalLevels[1] == .microphone, timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .xlrIsConsumerLevel, index: 0, value: params.xlrNominalLevels[0] == .consumer, timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .xlrIsConsumerLevel, index: 1, value: params.xlrNominalLevels[1] == .consumer, timeoutMs: timeoutMs)

        if ok {
            updateDuetCachedState(unitID: unitID) { $0.inputParams = params }
        }
        return ok
    }

    func setDuetInputGain(unitID: UnitInstanceID,
                          channel: Int,
                          gain: UInt8,
                          timeoutMs: UInt32 = 15_000) -> Bool {
        guard channel >= 0 && channel < 2 else {
            return false
        }

        let clamped = max(DuetInputParams.gainMin, min(DuetInputParams.gainMax, gain))
        guard duetWriteU8(unitID: unitID,
                          code: .inGain,
                          index: UInt8(channel),
                          value: clamped,
                          timeoutMs: timeoutMs)
        else {
            return false
        }

        updateDuetCachedState(unitID: unitID) { snapshot in
            if var input = snapshot.inputParams,
               channel < input.gains.count {
                input.gains[channel] = clamped
                snapshot.inputParams = input
            }
        }
        return true
    }

    func setDuetInputSource(unitID: UnitInstanceID,
                            channel: Int,
                            source: DuetInputSource,
                            timeoutMs: UInt32 = 15_000) -> Bool {
        guard channel >= 0 && channel < 2 else {
            return false
        }

        let isPhone = (source == .phone)
        guard duetWriteBool(unitID: unitID,
                            code: .inputSourceIsPhone,
                            index: UInt8(channel),
                            value: isPhone,
                            timeoutMs: timeoutMs)
        else {
            return false
        }

        updateDuetCachedState(unitID: unitID) { snapshot in
            if var input = snapshot.inputParams,
               channel < input.sources.count {
                input.sources[channel] = source
                snapshot.inputParams = input
            }
        }
        return true
    }

    func setDuetInputXlrNominalLevel(unitID: UnitInstanceID,
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
            duetWriteBool(unitID: unitID,
                          code: .xlrIsMicLevel,
                          index: index,
                          value: isMic,
                          timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID,
                          code: .xlrIsConsumerLevel,
                          index: index,
                          value: isConsumer,
                          timeoutMs: timeoutMs)
        guard ok else {
            return false
        }

        updateDuetCachedState(unitID: unitID) { snapshot in
            if var input = snapshot.inputParams,
               channel < input.xlrNominalLevels.count {
                input.xlrNominalLevels[channel] = level
                snapshot.inputParams = input
            }
        }
        return true
    }

    func setDuetInputPolarity(unitID: UnitInstanceID,
                              channel: Int,
                              inverted: Bool,
                              timeoutMs: UInt32 = 15_000) -> Bool {
        guard channel >= 0 && channel < 2 else {
            return false
        }

        guard duetWriteBool(unitID: unitID,
                            code: .micPolarity,
                            index: UInt8(channel),
                            value: inverted,
                            timeoutMs: timeoutMs)
        else {
            return false
        }

        updateDuetCachedState(unitID: unitID) { snapshot in
            if var input = snapshot.inputParams,
               channel < input.polarities.count {
                input.polarities[channel] = inverted
                snapshot.inputParams = input
            }
        }
        return true
    }

    func setDuetInputPhantom(unitID: UnitInstanceID,
                             channel: Int,
                             enabled: Bool,
                             timeoutMs: UInt32 = 15_000) -> Bool {
        guard channel >= 0 && channel < 2 else {
            return false
        }

        let index = UInt8(channel)

        guard duetWriteBool(unitID: unitID,
                            code: .micPhantom,
                            index: index,
                            value: enabled,
                            timeoutMs: timeoutMs)
        else {
            return false
        }

        updateDuetCachedState(unitID: unitID) { snapshot in
            if var input = snapshot.inputParams,
               channel < input.phantomPowerings.count {
                input.phantomPowerings[channel] = enabled
                snapshot.inputParams = input
            }
        }

        return true
    }

    func setDuetInputClickless(unitID: UnitInstanceID, enabled: Bool, timeoutMs: UInt32 = 15_000) -> Bool {
        guard duetWriteBool(unitID: unitID, code: .inClickless, value: enabled, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(unitID: unitID) { snapshot in
            if var input = snapshot.inputParams {
                input.clickless = enabled
                snapshot.inputParams = input
            }
        }
        return true
    }

    func getDuetMixerParams(unitID: UnitInstanceID, timeoutMs: UInt32 = 15_000) -> DuetMixerParams? {
        var params = DuetMixerParams()

        for destination in 0..<2 {
            for source in 0..<4 {
                guard let gain = duetReadU16(unitID: unitID,
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

        updateDuetCachedState(unitID: unitID) { $0.mixerParams = params }
        return params
    }

    func setDuetMixerParams(unitID: UnitInstanceID, params: DuetMixerParams, timeoutMs: UInt32 = 15_000) -> Bool {
        guard params.outputs.count >= 2 else {
            return false
        }

        for destination in 0..<2 {
            for source in 0..<4 {
                let gain = params.gain(destination: destination, source: source)
                let ok = duetWriteU16(unitID: unitID,
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

        updateDuetCachedState(unitID: unitID) { $0.mixerParams = params }
        return true
    }

    func setDuetMixerGain(unitID: UnitInstanceID,
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
        guard duetWriteU16(unitID: unitID,
                           code: .mixerSrc,
                           index: UInt8(source),
                           index2: UInt8(destination),
                           value: clamped,
                           timeoutMs: timeoutMs)
        else {
            return false
        }

        updateDuetCachedState(unitID: unitID) { snapshot in
            if var mixer = snapshot.mixerParams {
                mixer.setGain(destination: destination, source: source, value: clamped)
                snapshot.mixerParams = mixer
            }
        }
        return true
    }

    func getDuetDisplayParams(unitID: UnitInstanceID, timeoutMs: UInt32 = 15_000) -> DuetDisplayParams? {
        guard let isInput = duetReadBool(unitID: unitID, code: .displayIsInput, timeoutMs: timeoutMs),
              let follow = duetReadBool(unitID: unitID, code: .displayFollowToKnob, timeoutMs: timeoutMs),
              let overholdTwoSec = duetReadBool(unitID: unitID, code: .displayOverholdTwoSec, timeoutMs: timeoutMs)
        else {
            return nil
        }

        let params = DuetDisplayParams(
            target: isInput ? .input : .output,
            mode: follow ? .followingToKnobTarget : .independent,
            overhold: overholdTwoSec ? .twoSeconds : .infinite
        )

        updateDuetCachedState(unitID: unitID) { $0.displayParams = params }
        return params
    }

    func setDuetDisplayParams(unitID: UnitInstanceID, params: DuetDisplayParams, timeoutMs: UInt32 = 15_000) -> Bool {
        let ok =
            duetWriteBool(unitID: unitID, code: .displayIsInput, value: params.target == .input, timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .displayFollowToKnob, value: params.mode == .followingToKnobTarget, timeoutMs: timeoutMs) &&
            duetWriteBool(unitID: unitID, code: .displayOverholdTwoSec, value: params.overhold == .twoSeconds, timeoutMs: timeoutMs)

        if ok {
            updateDuetCachedState(unitID: unitID) { $0.displayParams = params }
        }
        return ok
    }

    func setDuetDisplayTarget(unitID: UnitInstanceID, target: DuetDisplayTarget, timeoutMs: UInt32 = 15_000) -> Bool {
        let isInput = (target == .input)
        guard duetWriteBool(unitID: unitID, code: .displayIsInput, value: isInput, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(unitID: unitID) { snapshot in
            if var display = snapshot.displayParams {
                display.target = target
                snapshot.displayParams = display
            }
        }
        return true
    }

    func setDuetDisplayMode(unitID: UnitInstanceID, mode: DuetDisplayMode, timeoutMs: UInt32 = 15_000) -> Bool {
        let follow = (mode == .followingToKnobTarget)
        guard duetWriteBool(unitID: unitID, code: .displayFollowToKnob, value: follow, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(unitID: unitID) { snapshot in
            if var display = snapshot.displayParams {
                display.mode = mode
                snapshot.displayParams = display
            }
        }
        return true
    }

    func setDuetDisplayOverhold(unitID: UnitInstanceID, overhold: DuetDisplayOverhold, timeoutMs: UInt32 = 15_000) -> Bool {
        let twoSec = (overhold == .twoSeconds)
        guard duetWriteBool(unitID: unitID, code: .displayOverholdTwoSec, value: twoSec, timeoutMs: timeoutMs) else {
            return false
        }

        updateDuetCachedState(unitID: unitID) { snapshot in
            if var display = snapshot.displayParams {
                display.overhold = overhold
                snapshot.displayParams = display
            }
        }
        return true
    }

    func clearDuetDisplay(unitID: UnitInstanceID, timeoutMs: UInt32 = 15_000) -> Bool {
        return duetWriteNoValue(unitID: unitID, code: .displayClear, timeoutMs: timeoutMs)
    }

    func getDuetFirmwareID(unitID: UnitInstanceID) -> UInt32? {
        let value = duetReadQuadlet(deviceID: unitID.device,
                                    address: 0xFFFF_F000_0000 + 0x0005_0000)
        if let value {
            updateDuetCachedState(unitID: unitID) { $0.firmwareID = value }
        }
        return value
    }

    func getDuetHardwareID(unitID: UnitInstanceID) -> UInt32? {
        let value = duetReadQuadlet(deviceID: unitID.device,
                                    address: 0xFFFF_F000_0000 + 0x0009_0020)
        if let value {
            updateDuetCachedState(unitID: unitID) { $0.hardwareID = value }
        }
        return value
    }

    // MARK: - Private codec helpers

    private func duetReadBool(unitID: UnitInstanceID,
                              code: DuetVendorCommandCode,
                              index: UInt8? = nil,
                              index2: UInt8? = nil,
                              timeoutMs: UInt32) -> Bool? {
        guard let payload = duetExchange(unitID: unitID,
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

    private func duetReadU8(unitID: UnitInstanceID,
                            code: DuetVendorCommandCode,
                            index: UInt8? = nil,
                            timeoutMs: UInt32) -> UInt8? {
        guard let payload = duetExchange(unitID: unitID,
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

    private func duetReadU16(unitID: UnitInstanceID,
                             code: DuetVendorCommandCode,
                             index: UInt8,
                             index2: UInt8,
                             timeoutMs: UInt32) -> UInt16? {
        guard let payload = duetExchange(unitID: unitID,
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

    private func duetReadHwState(unitID: UnitInstanceID, timeoutMs: UInt32) -> [UInt8]? {
        guard let payload = duetExchange(unitID: unitID,
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

    private func duetWriteBool(unitID: UnitInstanceID,
                               code: DuetVendorCommandCode,
                               index: UInt8? = nil,
                               value: Bool,
                               timeoutMs: UInt32) -> Bool {
        let byte = value ? DuetVendorWireConstants.boolOn : DuetVendorWireConstants.boolOff
        return duetExchange(unitID: unitID,
                            isStatus: false,
                            code: code,
                            index: index,
                            controlPayload: [byte],
                            timeoutMs: timeoutMs) != nil
    }

    private func duetWriteU8(unitID: UnitInstanceID,
                             code: DuetVendorCommandCode,
                             index: UInt8? = nil,
                             value: UInt8,
                             timeoutMs: UInt32) -> Bool {
        return duetExchange(unitID: unitID,
                            isStatus: false,
                            code: code,
                            index: index,
                            controlPayload: [value],
                            timeoutMs: timeoutMs) != nil
    }

    private func duetWriteU16(unitID: UnitInstanceID,
                              code: DuetVendorCommandCode,
                              index: UInt8,
                              index2: UInt8,
                              value: UInt16,
                              timeoutMs: UInt32) -> Bool {
        let payload: [UInt8] = [UInt8((value >> 8) & 0xFF), UInt8(value & 0xFF)]
        return duetExchange(unitID: unitID,
                            isStatus: false,
                            code: code,
                            index: index,
                            index2: index2,
                            controlPayload: payload,
                            timeoutMs: timeoutMs) != nil
    }

    private func duetWriteHwState(unitID: UnitInstanceID, payload: [UInt8], timeoutMs: UInt32) -> Bool {
        guard payload.count == 11 else {
            return false
        }

        return duetExchange(unitID: unitID,
                            isStatus: false,
                            code: .hwState,
                            controlPayload: payload,
                            timeoutMs: timeoutMs) != nil
    }

    private func duetWriteNoValue(unitID: UnitInstanceID,
                                  code: DuetVendorCommandCode,
                                  timeoutMs: UInt32) -> Bool {
        return duetExchange(unitID: unitID,
                            isStatus: false,
                            code: code,
                            controlPayload: [],
                            timeoutMs: timeoutMs) != nil
    }

    private func duetExchange(unitID: UnitInstanceID,
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
              let response = sendRawFCPCommand(unitID: unitID, frame: frame, timeoutMs: timeoutMs),
              let payload = DuetVendorCodec.parseStatusPayload(response,
                                                               expectedCode: code,
                                                               expectedIndex: index,
                                                               expectedIndex2: index2)
        else {
            return nil
        }

        return payload
    }

    private func duetReadQuadlet(deviceID: DeviceInstanceID, address: UInt64) -> UInt32? {
        guard let data = duetSyncAsyncRead(deviceID: deviceID, address: address, length: 4),
              data.count >= 4
        else {
            return nil
        }

        return (UInt32(data[0]) << 24) |
               (UInt32(data[1]) << 16) |
               (UInt32(data[2]) << 8) |
               UInt32(data[3])
    }

    private func duetSyncAsyncRead(deviceID: DeviceInstanceID,
                                   address: UInt64,
                                   length: UInt32) -> Data? {
        let addressHigh = UInt16((address >> 32) & 0xFFFF)
        let addressLow = UInt32(address & 0xFFFF_FFFF)
        guard let handle = asyncRead(deviceID: deviceID,
                                     addressHigh: addressHigh,
                                     addressLow: addressLow,
                                     length: length) else { return nil }
        let deadline = Date().addingTimeInterval(2.0)
        while Date() < deadline {
            if let result = getTransactionResult(handle: handle,
                                                 initialPayloadCapacity: Int(length) + 128) {
                guard result.status == 0, result.responseCode == 0 else { return nil }
                return result.payload
            }
            Thread.sleep(forTimeInterval: 0.01)
        }
        return nil
    }
}
