//
//  DriverConnector+Dice.swift
//  ASFW
//
//  Read-only capture of a DICE/TCAT device's register spaces, for the DICE
//  Report tab. Every read is an ordinary async block read against a live
//  generation -- no writes, no stream state is touched.
//

import Foundation

enum DiceSnapshotCaptureResult {
    case captured(DiceSnapshot)
    case unavailable(guid: UInt64)
    case unstable(guid: UInt64)
}

extension ASFWDriverConnector {

    // MARK: - Snapshot

    /// Capture only a stable view of a GUID. The legacy user-client async read
    /// selector has no generation argument, so a before/after discovery check
    /// is the boundary that prevents node-ID reuse after a bus reset from being
    /// presented as one coherent device report.
    func captureDiceSnapshot(guid: UInt64, attempts: Int = 2) -> DiceSnapshotCaptureResult {
        for _ in 0..<max(1, attempts) {
            guard let before = getDiscoveredDevices()?.first(where: { $0.guid == guid }) else {
                return .unavailable(guid: guid)
            }
            let snapshot = captureDiceSnapshotOnce(device: before)
            guard let after = getDiscoveredDevices()?.first(where: { $0.guid == guid }) else {
                continue
            }
            if after.nodeId == before.nodeId, after.generation == before.generation {
                return .captured(snapshot)
            }
        }
        return .unstable(guid: guid)
    }

    private func captureDiceSnapshotOnce(device: FWDeviceInfo) -> DiceSnapshot {
        var snap = DiceSnapshot()
        snap.nodeId = UInt16(device.nodeId)
        snap.generation = device.generation
        snap.guid = device.guid
        snap.vendorName = device.vendorName
        snap.modelName = device.modelName

        let node = UInt16(0xFFC0) | UInt16(device.nodeId)
        captureGeneralSpace(node: node, into: &snap)
        captureExtensionSpace(node: node, into: &snap)
        return snap
    }

    // MARK: General space

    private func captureGeneralSpace(node: UInt16, into snap: inout DiceSnapshot) {
        let base = DiceWire.generalBase
        guard let tableData = readNodeBlock(node: node, base: base, offset: 0, length: 40) else {
            snap.mark("general.table", .unreadable)
            snap.notes.append("General section table unreadable at 0xFFFFE0000000 — device may not be DICE.")
            return
        }
        guard let table = DiceSectionTable.decode(tableData,
                                                  names: DiceSectionTable.generalNames,
                                                  requiresPresentSection: true) else {
            snap.mark("general.table", .malformed)
            snap.notes.append("General section table is malformed or empty — device is not treated as DICE.")
            return
        }
        snap.generalSections = table
        snap.mark("general.table", .decoded)

        if let global = table["global"], global.isPresent {
            if global.byteSize < 0x60 {
                snap.mark("general.global", .malformed)
                snap.notes.append("Global section is shorter than its required 96-byte core.")
            } else if let d = readNodeBlock(node: node, base: base,
                                             offset: global.byteOffset, length: global.byteSize) {
                snap.global = DiceGlobal.decode(d, size: global.byteSize)
                snap.mark("general.global", .decoded)
            } else {
                snap.mark("general.global", global.byteSize > Self.maxNodeDiagnosticReadBytes ? .bounded : .unreadable)
                snap.notes.append("Global section unreadable.")
            }
        } else {
            snap.mark("general.global", .absent)
            snap.notes.append("Global section absent.")
        }

        if let tx = table["tx"], tx.isPresent {
            if tx.byteSize < 8 {
                snap.mark("general.tx", .malformed)
                snap.notes.append("TX section is shorter than its NUMBER/SIZE header.")
            } else if let d = readNodeBlock(node: node, base: base,
                                             offset: tx.byteOffset, length: tx.byteSize) {
                let r = DiceStreamSection.decode(d, isTx: true)
                snap.txCount = r.count
                snap.txSizeQuadlets = r.sizeQuadlets
                snap.txStreams = r.entries
                noteStreamAllocation(&snap, label: "TX", section: tx, count: r.count,
                                     sizeQuadlets: r.sizeQuadlets)
                snap.mark("general.tx", .decoded)
            } else {
                snap.mark("general.tx", tx.byteSize > Self.maxNodeDiagnosticReadBytes ? .bounded : .unreadable)
                snap.notes.append("TX section unreadable.")
            }
        } else {
            snap.mark("general.tx", .absent)
        }

        if let rx = table["rx"], rx.isPresent {
            if rx.byteSize < 8 {
                snap.mark("general.rx", .malformed)
                snap.notes.append("RX section is shorter than its NUMBER/SIZE header.")
            } else if let d = readNodeBlock(node: node, base: base,
                                             offset: rx.byteOffset, length: rx.byteSize) {
                let r = DiceStreamSection.decode(d, isTx: false)
                snap.rxCount = r.count
                snap.rxSizeQuadlets = r.sizeQuadlets
                snap.rxStreams = r.entries
                noteStreamAllocation(&snap, label: "RX", section: rx, count: r.count,
                                     sizeQuadlets: r.sizeQuadlets)
                snap.mark("general.rx", .decoded)
            } else {
                snap.mark("general.rx", rx.byteSize > Self.maxNodeDiagnosticReadBytes ? .bounded : .unreadable)
                snap.notes.append("RX section unreadable.")
            }
        } else {
            snap.mark("general.rx", .absent)
        }

        if let xs = table["ext_sync"], xs.isPresent {
            if xs.byteSize < 16 {
                snap.mark("general.ext_sync", .malformed)
                snap.notes.append("EXT_SYNC section is shorter than 16 bytes.")
            } else if let d = readNodeBlock(node: node, base: base,
                                             offset: xs.byteOffset, length: 16) {
                snap.extSync = DiceExtSync.decode(d)
                snap.mark("general.ext_sync", .decoded)
            } else {
                snap.mark("general.ext_sync", .unreadable)
                snap.notes.append("EXT_SYNC section unreadable.")
            }
        } else {
            snap.mark("general.ext_sync", .absent)
        }
    }

    /// A section is allocated for the firmware's maximum stream count while
    /// NUMBER reports how many are live. Recording the discrepancy is useful
    /// evidence from an unknown device; it is not an error.
    private func noteStreamAllocation(_ snap: inout DiceSnapshot, label: String,
                                      section: DiceSection, count: UInt32,
                                      sizeQuadlets: UInt32) {
        guard sizeQuadlets > 0, section.sizeQuadlets >= 2 else { return }
        let blocks = (Int(section.sizeQuadlets) - 2) / Int(sizeQuadlets)
        if blocks > Int(count) {
            snap.notes.append("\(label) section is allocated for \(blocks) stream block(s) but NUMBER reports \(count).")
        }
    }

    // MARK: Extension (EAP) space

    private func captureExtensionSpace(node: UInt16, into snap: inout DiceSnapshot) {
        let base = DiceWire.extensionBase
        guard let tableData = readNodeBlock(node: node, base: base, offset: 0, length: 72) else {
            snap.mark("eap.table", .unreadable)
            snap.notes.append("No extended application (EAP) space at 0xFFFFE0200000.")
            return
        }
        guard let table = DiceSectionTable.decode(tableData,
                                                  names: DiceSectionTable.extensionNames) else {
            snap.mark("eap.table", .malformed)
            snap.notes.append("EAP section table is malformed.")
            return
        }
        // An all-zero table means the space answered but carries nothing.
        guard table.sections.contains(where: { $0.isPresent }) else {
            snap.mark("eap.table", .absent)
            snap.notes.append("EAP section table present but empty.")
            return
        }
        snap.extensionSections = table
        snap.mark("eap.table", .decoded)

        guard let capsSection = table["caps"], capsSection.isPresent else {
            snap.mark("eap.caps", .absent)
            snap.notes.append("EAP capabilities absent; remaining EAP sections skipped.")
            return
        }
        guard capsSection.byteSize >= 12 else {
            snap.mark("eap.caps", .malformed)
            snap.notes.append("EAP capabilities section is shorter than 12 bytes; remaining EAP sections skipped.")
            return
        }
        guard let capsData = readNodeBlock(node: node, base: base,
                                           offset: capsSection.byteOffset,
                                           length: capsSection.byteSize),
              let caps = DiceEapCaps.decode(capsData) else {
            snap.mark("eap.caps", capsSection.byteSize > Self.maxNodeDiagnosticReadBytes ? .bounded : .unreadable)
            snap.notes.append("EAP capabilities unreadable; remaining EAP sections skipped.")
            return
        }
        snap.eapCaps = caps
        snap.mark("eap.caps", .decoded)

        let maxEntries = Int(caps.routerMaxEntries)
        let formatBytes = 8 + (Int(caps.maxTxStreams) + Int(caps.maxRxStreams))
            * DiceEapFormatEntry.size

        if let s = table["stream_format"], s.isPresent {
            if s.byteSize < formatBytes {
                snap.mark("eap.stream_format", .malformed)
                snap.notes.append("EAP stream-format staging section is shorter than its advertised stream layout.")
            } else if formatBytes > Self.maxNodeDiagnosticReadBytes {
                snap.mark("eap.stream_format", .bounded)
            } else if let d = readNodeBlock(node: node, base: base, offset: s.byteOffset,
                                             length: formatBytes) {
                snap.eapStreamFormats = DiceEapStreamFormats.decode(d)
                snap.mark("eap.stream_format", .decoded)
            } else {
                snap.mark("eap.stream_format", .unreadable)
            }
        } else {
            snap.mark("eap.stream_format", .absent)
        }

        if caps.routerExposed, let s = table["router"], s.isPresent {
            let required = 4 + maxEntries * 4
            if s.byteSize < 4 {
                snap.mark("eap.router", .malformed)
            } else if required > Self.maxNodeDiagnosticReadBytes {
                snap.mark("eap.router", .bounded)
            } else if let header = readNodeBlock(node: node, base: base, offset: s.byteOffset, length: 4) {
                let count = min(Int(DiceWire.u32(header, 0) ?? 0), maxEntries)
                let actualBytes = 4 + count * 4
                if s.byteSize < actualBytes {
                    snap.mark("eap.router", .malformed)
                    snap.notes.append("EAP router section ends before its declared route count.")
                } else if let d = readNodeBlock(node: node, base: base, offset: s.byteOffset,
                                                 length: actualBytes) {
                    snap.eapRouter = DiceRouterEntry.decodeAll(d, hasLeadingCount: true, maxEntries: maxEntries)
                    snap.mark("eap.router", .decoded)
                } else {
                    snap.mark("eap.router", .unreadable)
                }
            } else {
                snap.mark("eap.router", .unreadable)
            }
        } else {
            snap.mark("eap.router", caps.routerExposed ? .absent : .skipped)
        }

        if caps.peakAvailable, let s = table["peak"], s.isPresent {
            let required = maxEntries * 4
            if s.byteSize < required {
                snap.mark("eap.peak", .malformed)
            } else if required > Self.maxNodeDiagnosticReadBytes {
                snap.mark("eap.peak", .bounded)
            } else if let d = readNodeBlock(node: node, base: base, offset: s.byteOffset,
                                             length: required) {
                snap.eapPeak = DiceRouterEntry.decodeAll(d, hasLeadingCount: false,
                                                         maxEntries: maxEntries)
                snap.mark("eap.peak", .decoded)
            } else {
                snap.mark("eap.peak", .unreadable)
            }
        } else {
            snap.mark("eap.peak", caps.peakAvailable ? .absent : .skipped)
        }

        if caps.mixerExposed, let s = table["mixer"], s.isPresent {
            let need = 4 + 4 * DiceMixer.maxOutputs * DiceMixer.maxInputs
            if s.byteSize < need {
                snap.mark("eap.mixer", .malformed)
            } else if let d = readNodeBlock(node: node, base: base, offset: s.byteOffset,
                                             length: need) {
                snap.eapMixer = DiceMixer.decode(d, caps: caps)
                snap.mark("eap.mixer", .decoded)
            } else {
                snap.mark("eap.mixer", .unreadable)
            }
        } else {
            snap.mark("eap.mixer", caps.mixerExposed ? .absent : .skipped)
        }

        if let s = table["standalone"], s.isPresent {
            if s.byteSize < 20 {
                snap.mark("eap.standalone", .malformed)
            } else if let d = readNodeBlock(node: node, base: base, offset: s.byteOffset,
                                             length: 20) {
                snap.eapStandalone = DiceStandalone.decode(d)
                snap.mark("eap.standalone", .decoded)
            } else {
                snap.mark("eap.standalone", .unreadable)
            }
        } else {
            snap.mark("eap.standalone", .absent)
        }

        // Current configuration holds a router and a stream layout per rate
        // mode at fixed sub-offsets. Read only those windows rather than the
        // whole (24 KB) section.
        if let s = table["current_config"], s.isPresent {
            for mode in DiceRateMode.allCases {
                let rOff = s.byteOffset + mode.routerOffset
                let routerKey = "eap.current_router.\(mode.rawValue)"
                let routerBytes = 4 + maxEntries * 4
                if mode.routerOffset + 4 > s.byteSize {
                    snap.mark(routerKey, .absent)
                } else if routerBytes > Self.maxNodeDiagnosticReadBytes {
                    snap.mark(routerKey, .bounded)
                } else if let header = readNodeBlock(node: node, base: base, offset: rOff, length: 4) {
                    let count = min(Int(DiceWire.u32(header, 0) ?? 0), maxEntries)
                    let actualBytes = 4 + count * 4
                    if mode.routerOffset + actualBytes > s.byteSize {
                        snap.mark(routerKey, .malformed)
                    } else if let d = readNodeBlock(node: node, base: base, offset: rOff,
                                                     length: actualBytes) {
                    snap.eapCurrentRouters[mode] =
                        DiceRouterEntry.decodeAll(d, hasLeadingCount: true, maxEntries: maxEntries)
                        snap.mark(routerKey, .decoded)
                    } else {
                        snap.mark(routerKey, .unreadable)
                    }
                } else {
                    snap.mark(routerKey, .unreadable)
                }
                let sOff = s.byteOffset + mode.streamOffset
                let formatKey = "eap.current_format.\(mode.rawValue)"
                if mode.streamOffset + 8 > s.byteSize {
                    snap.mark(formatKey, .absent)
                } else if formatBytes > Self.maxNodeDiagnosticReadBytes {
                    snap.mark(formatKey, .bounded)
                } else if mode.streamOffset + formatBytes > s.byteSize {
                    snap.mark(formatKey, .malformed)
                } else if let d = readNodeBlock(node: node, base: base, offset: sOff,
                                                 length: formatBytes) {
                    snap.eapCurrentFormats[mode] = DiceEapStreamFormats.decode(d)
                    snap.mark(formatKey, .decoded)
                } else {
                    snap.mark(formatKey, .unreadable)
                }
            }
        } else {
            for mode in DiceRateMode.allCases {
                snap.mark("eap.current_router.\(mode.rawValue)", .absent)
                snap.mark("eap.current_format.\(mode.rawValue)", .absent)
            }
        }

        if let app = table["application"], app.isPresent {
            snap.mark("eap.application", .skipped)
            snap.notes.append(String(format:
                "EAP application section is vendor-specific (%d bytes at offset 0x%06X) and is not decoded.",
                app.byteSize, app.byteOffset))
        } else {
            snap.mark("eap.application", .absent)
        }
        snap.mark("eap.cmd", table["cmd"]?.isPresent == true ? .skipped : .absent)
    }
}
