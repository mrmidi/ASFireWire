//
//  DriverConnector+Dice.swift
//  ASFW
//
//  Read-only capture of a DICE/TCAT device's register spaces, for the DICE
//  Report tab. Every read is an ordinary async block read against a live
//  generation -- no writes, no stream state is touched.
//

import Foundation

extension ASFWDriverConnector {

    /// Async block reads are bounded by the device's max_rec, so long sections
    /// are fetched in chunks. 512 bytes is safely below every DICE device's
    /// advertised maximum.
    private static let diceChunkBytes = 512
    private static let diceReadTimeout: TimeInterval = 2.0

    // MARK: - Raw access

    /// One chunked block read. Returns nil if any chunk fails, so a partially
    /// read section is never mistaken for a short one.
    ///
    /// Falls back to quadlet reads if the device refuses a block read of this
    /// size. DICE guarantees every register is readable by quadlet
    /// (dice-interface.h:10-13), but max_rec varies, and an unknown device is
    /// exactly the case this report exists to describe -- so slow-but-works
    /// beats fast-but-empty.
    func readDiceBlock(node: UInt16, base: UInt64, offset: Int, length: Int) -> Data? {
        guard length > 0 else { return Data() }
        var out = Data()
        out.reserveCapacity(length)
        var done = 0
        while done < length {
            let chunk = min(Self.diceChunkBytes, length - done)
            let addr = base &+ UInt64(offset + done)
            guard let part = diceSyncRead(node: node,
                                          addressHigh: UInt16((addr >> 32) & 0xFFFF),
                                          addressLow: UInt32(addr & 0xFFFF_FFFF),
                                          length: UInt32(chunk)),
                  part.count >= chunk else {
                return readDiceQuadlets(node: node, base: base,
                                        offset: offset, length: length)
            }
            out.append(part.prefix(chunk))
            done += chunk
        }
        return out
    }

    private func readDiceQuadlets(node: UInt16, base: UInt64,
                                  offset: Int, length: Int) -> Data? {
        var out = Data()
        out.reserveCapacity(length)
        var done = 0
        while done < length {
            let addr = base &+ UInt64(offset + done)
            guard let part = diceSyncRead(node: node,
                                          addressHigh: UInt16((addr >> 32) & 0xFFFF),
                                          addressLow: UInt32(addr & 0xFFFF_FFFF),
                                          length: 4),
                  part.count >= 4 else {
                return nil
            }
            out.append(part.prefix(4))
            done += 4
        }
        return out
    }

    private func diceSyncRead(node: UInt16, addressHigh: UInt16,
                              addressLow: UInt32, length: UInt32) -> Data? {
        guard let handle = asyncRead(destinationID: node,
                                     addressHigh: addressHigh,
                                     addressLow: addressLow,
                                     length: length) else { return nil }
        let deadline = Date().addingTimeInterval(Self.diceReadTimeout)
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

    // MARK: - Snapshot

    func captureDiceSnapshot(device: FWDeviceInfo) -> DiceSnapshot {
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
        guard let tableData = readDiceBlock(node: node, base: base, offset: 0, length: 40),
              let table = DiceSectionTable.decode(tableData,
                                                  names: DiceSectionTable.generalNames) else {
            snap.notes.append("General section table unreadable at 0xFFFFE0000000 — device may not be DICE.")
            return
        }
        snap.generalSections = table

        if let global = table["global"], global.isPresent,
           let d = readDiceBlock(node: node, base: base,
                                 offset: global.byteOffset, length: global.byteSize) {
            snap.global = DiceGlobal.decode(d, size: global.byteSize)
        } else {
            snap.notes.append("Global section unreadable.")
        }

        if let tx = table["tx"], tx.isPresent,
           let d = readDiceBlock(node: node, base: base,
                                 offset: tx.byteOffset, length: tx.byteSize) {
            let r = DiceStreamSection.decode(d, isTx: true)
            snap.txCount = r.count
            snap.txSizeQuadlets = r.sizeQuadlets
            snap.txStreams = r.entries
            noteStreamAllocation(&snap, label: "TX", section: tx, count: r.count,
                                 sizeQuadlets: r.sizeQuadlets)
        }

        if let rx = table["rx"], rx.isPresent,
           let d = readDiceBlock(node: node, base: base,
                                 offset: rx.byteOffset, length: rx.byteSize) {
            let r = DiceStreamSection.decode(d, isTx: false)
            snap.rxCount = r.count
            snap.rxSizeQuadlets = r.sizeQuadlets
            snap.rxStreams = r.entries
            noteStreamAllocation(&snap, label: "RX", section: rx, count: r.count,
                                 sizeQuadlets: r.sizeQuadlets)
        }

        if let xs = table["ext_sync"], xs.isPresent,
           let d = readDiceBlock(node: node, base: base,
                                 offset: xs.byteOffset, length: min(16, xs.byteSize)) {
            snap.extSync = DiceExtSync.decode(d)
        }
    }

    /// A section is allocated for the firmware's maximum stream count while
    /// NUMBER reports how many are live. Recording the discrepancy is useful
    /// evidence from an unknown device; it is not an error.
    private func noteStreamAllocation(_ snap: inout DiceSnapshot, label: String,
                                      section: DiceSection, count: UInt32,
                                      sizeQuadlets: UInt32) {
        guard sizeQuadlets > 0 else { return }
        let blocks = (Int(section.sizeQuadlets) - 2) / Int(sizeQuadlets)
        if blocks > Int(count) {
            snap.notes.append("\(label) section is allocated for \(blocks) stream block(s) but NUMBER reports \(count).")
        }
    }

    // MARK: Extension (EAP) space

    private func captureExtensionSpace(node: UInt16, into snap: inout DiceSnapshot) {
        let base = DiceWire.extensionBase
        guard let tableData = readDiceBlock(node: node, base: base, offset: 0, length: 72),
              let table = DiceSectionTable.decode(tableData,
                                                  names: DiceSectionTable.extensionNames) else {
            snap.notes.append("No extended application (EAP) space at 0xFFFFE0200000.")
            return
        }
        // An all-zero table means the space answered but carries nothing.
        guard table.sections.contains(where: { $0.isPresent }) else {
            snap.notes.append("EAP section table present but empty.")
            return
        }
        snap.extensionSections = table

        guard let capsSection = table["caps"], capsSection.isPresent,
              let capsData = readDiceBlock(node: node, base: base,
                                           offset: capsSection.byteOffset,
                                           length: max(12, capsSection.byteSize)),
              let caps = DiceEapCaps.decode(capsData) else {
            snap.notes.append("EAP capabilities unreadable; remaining EAP sections skipped.")
            return
        }
        snap.eapCaps = caps

        let maxEntries = Int(caps.routerMaxEntries)
        let formatBytes = 8 + (Int(caps.maxTxStreams) + Int(caps.maxRxStreams))
            * DiceEapFormatEntry.size

        if let s = table["stream_format"], s.isPresent,
           let d = readDiceBlock(node: node, base: base, offset: s.byteOffset,
                                 length: min(s.byteSize, formatBytes)) {
            snap.eapStreamFormats = DiceEapStreamFormats.decode(d)
        }

        if caps.routerExposed, let s = table["router"], s.isPresent,
           let d = readDiceBlock(node: node, base: base, offset: s.byteOffset,
                                 length: min(s.byteSize, 4 + maxEntries * 4)) {
            snap.eapRouter = DiceRouterEntry.decodeAll(d, hasLeadingCount: true,
                                                       maxEntries: maxEntries)
        }

        if caps.peakAvailable, let s = table["peak"], s.isPresent,
           let d = readDiceBlock(node: node, base: base, offset: s.byteOffset,
                                 length: min(s.byteSize, maxEntries * 4)) {
            snap.eapPeak = DiceRouterEntry.decodeAll(d, hasLeadingCount: false,
                                                     maxEntries: maxEntries)
        }

        if caps.mixerExposed, let s = table["mixer"], s.isPresent {
            let need = 4 + 4 * DiceMixer.maxOutputs * DiceMixer.maxInputs
            if let d = readDiceBlock(node: node, base: base, offset: s.byteOffset,
                                     length: min(s.byteSize, need)) {
                snap.eapMixer = DiceMixer.decode(d, caps: caps)
            }
        }

        if let s = table["standalone"], s.isPresent,
           let d = readDiceBlock(node: node, base: base, offset: s.byteOffset,
                                 length: min(s.byteSize, 20)) {
            snap.eapStandalone = DiceStandalone.decode(d)
        }

        // Current configuration holds a router and a stream layout per rate
        // mode at fixed sub-offsets. Read only those windows rather than the
        // whole (24 KB) section.
        if let s = table["current_config"], s.isPresent {
            for mode in DiceRateMode.allCases {
                let rOff = s.byteOffset + mode.routerOffset
                if rOff + 4 <= s.byteOffset + s.byteSize,
                   let d = readDiceBlock(node: node, base: base, offset: rOff,
                                         length: min(4 + maxEntries * 4,
                                                     s.byteSize - mode.routerOffset)) {
                    snap.eapCurrentRouters[mode] =
                        DiceRouterEntry.decodeAll(d, hasLeadingCount: true, maxEntries: maxEntries)
                }
                let sOff = s.byteOffset + mode.streamOffset
                if sOff + 8 <= s.byteOffset + s.byteSize,
                   let d = readDiceBlock(node: node, base: base, offset: sOff,
                                         length: min(formatBytes,
                                                     s.byteSize - mode.streamOffset)) {
                    snap.eapCurrentFormats[mode] = DiceEapStreamFormats.decode(d)
                }
            }
        }

        if let app = table["application"], app.isPresent {
            snap.notes.append(String(format:
                "EAP application section is vendor-specific (%d bytes at offset 0x%06X) and is not decoded.",
                app.byteSize, app.byteOffset))
        }
    }
}
