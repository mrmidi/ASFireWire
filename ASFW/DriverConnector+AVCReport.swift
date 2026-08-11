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

    /// Capture only a stable view of a GUID. The legacy user-client async read
    /// selector has no generation argument, so a before/after discovery check
    /// is the boundary that prevents node-ID reuse after a bus reset from being
    /// presented as one coherent device report.
    func captureAVCSnapshot(guid: UInt64, attempts: Int = 2) -> AVCSnapshotCaptureResult {
        for _ in 0..<max(1, attempts) {
            guard let before = getDiscoveredDevices()?.first(where: { $0.guid == guid }) else {
                return .unavailable(guid: guid)
            }
            let snapshot = captureAVCSnapshotOnce(device: before)
            guard let after = getDiscoveredDevices()?.first(where: { $0.guid == guid }) else {
                continue
            }
            guard after.nodeId == before.nodeId, after.generation == before.generation else {
                continue
            }
            return snapshot.isAVCDevice ? .captured(snapshot) : .notAVC(guid: guid)
        }
        return .unstable(guid: guid)
    }

    private func captureAVCSnapshotOnce(device: FWDeviceInfo) -> AVCDeviceSnapshot {
        var snap = AVCDeviceSnapshot()
        snap.guid = device.guid
        snap.nodeId = UInt16(device.nodeId)
        snap.generation = device.generation
        snap.vendorId = device.vendorId
        snap.modelId = device.modelId
        snap.vendorName = device.vendorName
        snap.modelName = device.modelName

        // Decided from Config ROM identity alone -- nothing has been sent yet.
        snap.policy = AVCProbeRestrictions.policy(vendorId: device.vendorId,
                                                  modelId: device.modelId)

        // Family memory blocks first: they are safe at every tier, and they are
        // what identifies a device whose AV/C surface we are not allowed to ask.
        let node = UInt16(0xFFC0) | UInt16(device.nodeId)
        captureBridgeCoSection(node: node, device: device, into: &snap)

        // The driver's own discovery already ran UNIT/SUBUNIT INFO for units it
        // did not bypass. Reuse it rather than re-probing the device.
        adoptDriverDiscoveredInventory(guid: device.guid, into: &snap)

        switch snap.policy.tier {
        case .memoryOnly:
            snap.note("AV/C probing suppressed by policy — plug inventory not queried.")
        case .avcStatus:
            captureUnitPlugs(guid: device.guid, into: &snap)
            captureSubunits(guid: device.guid, into: &snap)
            captureMusicSubunitPlugs(guid: device.guid, into: &snap)
        }
        return snap
    }

    // MARK: - Driver-side discovery reuse

    private func adoptDriverDiscoveredInventory(guid: UInt64, into snap: inout AVCDeviceSnapshot) {
        guard let unit = getAVCUnits()?.first(where: { $0.guid == guid }) else { return }

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

    /// AV/C General PLUG_INFO (opcode 0x02, subfunction 0x00).
    private func captureUnitPlugs(guid: UInt64, into snap: inout AVCDeviceSnapshot) {
        guard let response = sendAVCStatus(guid: guid,
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
    private func captureSubunits(guid: UInt64, into snap: inout AVCDeviceSnapshot) {
        guard snap.inventory.subunits.isEmpty else { return }
        guard let response = sendAVCStatus(guid: guid, frame: Self.subunitInfoCommand) else {
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
    private func captureMusicSubunitPlugs(guid: UInt64, into snap: inout AVCDeviceSnapshot) {
        guard let response = sendAVCStatus(
                guid: guid,
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
                    guid: guid,
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

    private func captureBridgeCoSection(node: UInt16, device: FWDeviceInfo,
                                        into snap: inout AVCDeviceSnapshot) {
        let base = (UInt64(ASFWMCPBeBoBBootRomInformation.addressHigh) << 32)
            | UInt64(ASFWMCPBeBoBBootRomInformation.addressLow)

        // Try the extended record first; older BridgeCo firmware exposes only
        // the base 80-byte record and refuses the longer read.
        let lengths = [ASFWMCPBeBoBBootRomInformation.sizeWithDebugger,
                       ASFWMCPBeBoBBootRomInformation.sizeWithoutDebugger]
        for length in lengths {
            guard let data = readNodeBlock(node: node, base: base, offset: 0, length: length) else {
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
    private func sendAVCStatus(guid: UInt64, frame: [UInt8]) -> [UInt8]? {
        guard frame.first == 0x01 else { return nil }  // AV/C STATUS
        guard let response = sendRawFCPCommand(guid: guid,
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
