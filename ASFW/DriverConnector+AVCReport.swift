//
//  DriverConnector+AVCReport.swift
//  ASFW
//
//  Read-only capture of an AV/C audio device, for the AV/C Device Report tab.
//
//  Layered the same way FFADO layers libavc under GenericAVC::Device: the
//  generic probe below works on any conforming AV/C unit, and per-family
//  extensions (BridgeCo today) attach as optional sections.
//
//  Ordering is load-bearing. Config ROM identity is already known from
//  discovery, so the probe tier is decided BEFORE any transaction is issued
//  (see AVCReportModels.swift for why). Within a tier:
//
//    1. family memory blocks       -- plain reads, safe everywhere
//    2. AV/C STATUS inventory      -- suppressed on restricted devices
//
//  No CONTROL commands, no writes, no stream state is touched.
//

import Foundation

extension ASFWDriverConnector {

    private static let avcReportFcpTimeoutMs: UInt32 = 4_000

    /// AV/C General SUBUNIT INFO (opcode 0x31), page 0, all subunit types.
    /// FFADO libavc/general/avc_subunit_info.h.
    private static let subunitInfoCommand: [UInt8] =
        [0x01, 0xff, 0x31, 0x07, 0xff, 0xff, 0xff, 0xff]

    /// Capture one stable unit instance. The route is revalidated after the
    /// diagnostic reads so a reset can never splice two generations together.
    func captureAVCSnapshot(unitID: UnitInstanceID, attempts: Int = 2) -> AVCSnapshotCaptureResult {
        for _ in 0..<max(1, attempts) {
            guard let before = getDiscoveredDevices()?.first(where: { $0.id == unitID.device }),
                  let beforeUnit = getAVCUnits()?.first(where: { $0.id == unitID }) else {
                return .unavailable(unitID: unitID)
            }
            let snapshot = captureAVCSnapshotOnce(device: before, unit: beforeUnit)
            guard let after = getDiscoveredDevices()?.first(where: { $0.id == unitID.device }),
                  getAVCUnits()?.contains(where: { $0.id == unitID }) == true else {
                continue
            }
            guard after.nodeId == before.nodeId, after.generation == before.generation else {
                continue
            }
            return snapshot.isAVCDevice ? .captured(snapshot) : .notAVC(unitID: unitID)
        }
        return .unstable(unitID: unitID)
    }

    private func captureAVCSnapshotOnce(device: FWDeviceInfo,
                                        unit: AVCUnitInfo) -> AVCDeviceSnapshot {
        var snap = AVCDeviceSnapshot()
        snap.guid = device.observedGuid
        snap.nodeId = UInt16(device.nodeId)
        snap.generation = device.generation
        snap.vendorId = device.vendorId
        snap.modelId = device.modelId
        snap.vendorName = device.vendorName
        snap.modelName = device.modelName

        // Decided from Config ROM identity alone -- nothing has been sent yet.
        snap.policy = device.isQuarantined
            ? AVCProbePolicy(
                tier: .memoryOnly,
                rationale: "The runtime identity resolver quarantined this instance (\(device.quarantineReason)); no bus transaction is permitted."
            )
            : AVCProbePolicy(
                tier: .avcStatus,
                rationale: "The selected unit passed the runtime catalog safety gate; STATUS-only AV/C diagnostics are permitted."
            )

        guard !device.isQuarantined else {
            snap.note("Only cached Config-ROM identity is available for this quarantined instance.")
            return snap
        }

        // Family memory blocks first: they are safe at every tier, and they are
        // what identifies a device whose AV/C surface we are not allowed to ask.
        captureBridgeCoSection(device: device, into: &snap)

        // The driver's own discovery already ran UNIT/SUBUNIT INFO for units it
        // did not bypass. Reuse it rather than re-probing the device.
        adoptDriverDiscoveredInventory(unit: unit, into: &snap)

        switch snap.policy.tier {
        case .memoryOnly:
            snap.note("AV/C probing suppressed by policy — plug inventory not queried.")
        case .avcStatus:
            captureUnitPlugs(unitID: unit.id, into: &snap)
            captureSubunits(unitID: unit.id, into: &snap)
            captureMusicSubunitPlugs(unitID: unit.id, into: &snap)
        }
        return snap
    }

    // MARK: - Driver-side discovery reuse

    private func adoptDriverDiscoveredInventory(unit: AVCUnitInfo,
                                                into snap: inout AVCDeviceSnapshot) {
        // The driver registers a unit before its probe runs, so an entry with
        // nothing in it is the signature of a probe that failed or never
        // completed -- not a device that reported four zero plug counts. Adopt
        // it only when it carries something, and say so when it does not.
        let plugs = AVCUnitPlugCounts(isochronousInput: unit.isoInputPlugs,
                                      isochronousOutput: unit.isoOutputPlugs,
                                      externalInput: unit.extInputPlugs,
                                      externalOutput: unit.extOutputPlugs)
        let plugsAreEmpty = plugs == AVCUnitPlugCounts(isochronousInput: 0, isochronousOutput: 0,
                                                       externalInput: 0, externalOutput: 0)
        if plugsAreEmpty && unit.subunits.isEmpty {
            snap.note("The driver has an AV/C unit registered for this device but its "
                      + "discovery reported no plugs and no subunits, which means the probe "
                      + "did not complete. Nothing below is a device answer.")
            return
        }

        snap.inventory.unitPlugs = plugs
        snap.inventory.unitPlugsProvenance = .driverDiscovery
        if !unit.subunits.isEmpty {
            snap.inventory.subunits = unit.subunits.map {
                AVCSubunitEntry(type: $0.type, id: $0.subunitID,
                                sourcePlugs: $0.numSrcPlugs,
                                destinationPlugs: $0.numDestPlugs)
            }
            snap.inventory.subunitsProvenance = .driverDiscovery
        }
    }

    // MARK: - Generic AV/C STATUS inventory

    // TODO: distinguish "refused by driver policy" from "did not answer".
    //
    // Every `did not answer` note below is written on a nil response, which
    // collapses two different facts. Measured against a booted M-Audio 1814 on
    // 2026-08-13: the driver refused PLUG_INFO (0x02) and SUBUNIT_INFO (0x31) at
    // FCPTransport::SubmitCommand — the frames never reached the bus, confirmed
    // by three `FCPTransport: refused ctype=0x01 opcode=…` ring records and by
    // the absence of any matching AT write in the transaction trace — and the
    // report said the device did not answer. The device was never asked.
    //
    // The driver already draws the distinction: FCPStatus::kRefusedByFilter maps
    // to kIOReturnNotPermitted precisely so this layer can tell them apart. It
    // is thrown away here because sendAVCStatus returns Data?.
    //
    // Fix: surface the kern_return_t (or a small typed result) out of
    // sendAVCStatus and note refusals as "not sent — outside this device's
    // permitted command set", citing AVC_DEVICE_HAZARDS.md H1.
    //
    // Related and larger: the probe tier itself is stale. `Tier: memory + AV/C
    // STATUS` reasons from "passed the catalog safety gate", a gate these
    // identities no longer have, and splits on STATUS-vs-CONTROL — which is the
    // wrong axis, since PLUG_INFO and SUBUNIT_INFO are both STATUS and both
    // freeze-capable. The app should read the device's ProbePolicyId and simply
    // not attempt these commands for BeBoBFilteredCommandSet devices. The driver
    // gate is sufficient for safety; this is about the report telling the truth.

    /// AV/C General PLUG_INFO (opcode 0x02, subfunction 0x00).
    private func captureUnitPlugs(unitID: UnitInstanceID, into snap: inout AVCDeviceSnapshot) {
        guard let response = sendAVCStatus(unitID: unitID,
                                           frame: ASFWMCPBeBoBUnitPlugInformation.statusCommand) else {
            if snap.inventory.unitPlugs == nil {
                snap.note("Unit PLUG_INFO (AV/C STATUS 0x02) did not answer.")
            }
            return
        }
        guard let plugs = ASFWMCPBeBoBUnitPlugInformation.decode(response) else {
            snap.note("Unit PLUG_INFO returned an unexpected response: \(Self.hex(response)).")
            return
        }
        // A live answer supersedes whatever discovery cached.
        snap.inventory.unitPlugs = AVCUnitPlugCounts(
            isochronousInput: plugs.isochronousInputCount,
            isochronousOutput: plugs.isochronousOutputCount,
            externalInput: plugs.externalInputCount,
            externalOutput: plugs.externalOutputCount)
        snap.inventory.unitPlugsProvenance = .probed
    }

    /// AV/C General SUBUNIT INFO (opcode 0x31). Only probed when the driver's
    /// discovery did not already supply the list.
    private func captureSubunits(unitID: UnitInstanceID, into snap: inout AVCDeviceSnapshot) {
        guard snap.inventory.subunits.isEmpty else { return }
        guard let response = sendAVCStatus(unitID: unitID, frame: Self.subunitInfoCommand) else {
            snap.note("SUBUNIT INFO (AV/C STATUS 0x31) did not answer.")
            return
        }
        // Response: [0]=0x0c STABLE, [1]=0xff unit, [2]=0x31, [3]=page/extension,
        // [4..7] = up to four subunit descriptors, 0xff padding.
        guard response.count >= 8, response[0] == 0x0c, response[2] == 0x31 else {
            snap.note("SUBUNIT INFO returned an unexpected response: \(Self.hex(response)).")
            return
        }
        for byte in response[4..<8] where byte != 0xff {
            let type = byte >> 3
            let id = byte & 0x07
            // maxSubunitID 0x07 marks an extended descriptor we do not decode.
            guard id != 0x07 else { continue }
            snap.inventory.subunits.append(
                AVCSubunitEntry(type: type, id: id, sourcePlugs: 0, destinationPlugs: 0))
        }
        snap.inventory.subunitsProvenance = .probed
    }

    /// AV/C Music subunit plug inventory via extended plug info (0x02 / 0xC0).
    private func captureMusicSubunitPlugs(unitID: UnitInstanceID, into snap: inout AVCDeviceSnapshot) {
        guard let response = sendAVCStatus(
                unitID: unitID,
                frame: ASFWMCPBeBoBClockTopology.musicSubunitPlugInfoCommand()) else {
            snap.note("Music subunit PLUG_INFO did not answer — device may have no music subunit.")
            return
        }
        guard let count = ASFWMCPBeBoBClockTopology.musicSubunitInputPlugCount(response) else {
            snap.note("Music subunit PLUG_INFO returned an unexpected response: \(Self.hex(response)).")
            return
        }
        snap.inventory.musicSubunitInputPlugCount = count
        guard count <= ASFWMCPBeBoBClockTopology.maxMusicSubunitInputPlugs else {
            snap.note("Music subunit reported \(count) input plugs, above the "
                      + "\(ASFWMCPBeBoBClockTopology.maxMusicSubunitInputPlugs) bound — not enumerated.")
            return
        }
        for plug in 0..<count {
            guard let typeResponse = sendAVCStatus(
                    unitID: unitID,
                    frame: ASFWMCPBeBoBClockTopology.musicSubunitInputPlugTypeCommand(plug)),
                  let type = ASFWMCPBeBoBClockTopology.plugType(typeResponse) else {
                snap.note("Music subunit input plug \(plug): type query did not answer.")
                continue
            }
            snap.inventory.musicSubunitInputPlugs.append(
                AVCMusicPlug(index: plug, type: type,
                             typeName: ASFWMCPBeBoBClockTopology.plugTypeName(type)))
        }
    }

    // MARK: - Family section: BridgeCo (BeBoB)

    private func captureBridgeCoSection(device: FWDeviceInfo,
                                        into snap: inout AVCDeviceSnapshot) {
        let base = (UInt64(ASFWMCPBeBoBBootRomInformation.addressHigh) << 32)
            | UInt64(ASFWMCPBeBoBBootRomInformation.addressLow)

        // Try the extended record first; older BridgeCo firmware exposes only
        // the base 80-byte record and refuses the longer read.
        let lengths = [ASFWMCPBeBoBBootRomInformation.sizeWithDebugger,
                       ASFWMCPBeBoBBootRomInformation.sizeWithoutDebugger]
        for length in lengths {
            guard let data = readDeviceBlock(deviceID: device.id,
                                             base: base,
                                             offset: 0,
                                             length: length) else {
                continue
            }
            var section = BridgeCoSection()
            section.vendorId = device.vendorId
            section.modelId = device.modelId
            section.raw = data
            if let info = ASFWMCPBeBoBBootRomInformation.decode([UInt8](data)) {
                section.bootRom = info
                snap.bridgeCo = section
                return
            }
            // The read worked but the payload is not a BootROM. That is the
            // normal answer for a non-BridgeCo AV/C device, so it is not a
            // note-worthy failure -- just no BridgeCo section.
            return
        }
    }

    // MARK: - Helpers

    /// STATUS-only FCP. The ctype byte is asserted rather than trusted so a
    /// future caller cannot turn this diagnostic into a CONTROL path.
    private func sendAVCStatus(unitID: UnitInstanceID, frame: [UInt8]) -> [UInt8]? {
        guard frame.first == 0x01 else { return nil }  // AV/C STATUS
        guard let response = sendRawFCPCommand(unitID: unitID,
                                               frame: Data(frame),
                                               timeoutMs: Self.avcReportFcpTimeoutMs) else {
            return nil
        }
        return [UInt8](response)
    }

    private static func hex(_ bytes: [UInt8]) -> String {
        bytes.map { String(format: "%02X", $0) }.joined(separator: " ")
    }
}
