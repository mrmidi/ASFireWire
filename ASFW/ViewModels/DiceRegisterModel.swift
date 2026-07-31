//
//  DiceRegisterModel.swift
//  ASFW
//
//  Decoded model of a DICE/TCAT device's register spaces.
//
//  Two independent address spaces are modelled:
//
//    general   0xFFFFE0000000  global / tx / rx / ext_sync
//    extension 0xFFFFE0200000  caps / cmd / mixer / peak / router /
//                              stream_format / current_config /
//                              standalone / application   ("EAP")
//
//  Both begin with a section table of (offset, size) pairs measured in
//  QUADLETS. Nothing may assume a fixed section base: on a Saffire Pro 24 DSP
//  the global section starts at 0x28, not 0.
//
//  Byte layouts cross-checked against the TCAT protocol implementation in
//  references/alsa-userspace-control-protocols-impl/protocols/dice/src/tcat/
//  and the register map in
//  references/linux-sound-firewire-stack/firewire/dice/dice-interface.h.
//  No reference code is copied.
//

import Foundation

// MARK: - Primitives

enum DiceWire {
    /// Base of the general ("DICE driver") register space.
    static let generalBase: UInt64 = 0xFFFF_E000_0000
    /// Base of the extended application register space.
    static let extensionBase: UInt64 = 0xFFFF_E020_0000

    static func u32(_ data: Data, _ offset: Int) -> UInt32? {
        guard offset >= 0, offset + 4 <= data.count else { return nil }
        let b = data[data.startIndex.advanced(by: offset)..<data.startIndex.advanced(by: offset + 4)]
        return b.reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
    }

    /// DICE firmware runs little-endian and byte-swaps *every* quadlet onto the
    /// bus, so text loses its ordering and must be swapped back per quadlet.
    /// dice-interface.h:15-17.
    static func text(_ data: Data, _ offset: Int, _ length: Int) -> String {
        guard offset >= 0, offset + length <= data.count else { return "" }
        var out = [UInt8]()
        out.reserveCapacity(length)
        var pos = offset
        while pos + 4 <= offset + length {
            let base = data.startIndex.advanced(by: pos)
            out.append(contentsOf: (0..<4).reversed().map { data[base.advanced(by: $0)] })
            pos += 4
        }
        return String(decoding: out, as: UTF8.self)
    }

    /// Name blocks are `\`-separated and terminated by `\\`
    /// (dice-interface.h:216-219).
    ///
    /// Split on a *single* backslash and stop at the first empty element: the
    /// terminating pair produces one, and so does the NUL padding after it.
    /// A consequence worth knowing is that the format cannot represent an empty
    /// name -- one would be indistinguishable from the terminator -- which is
    /// why devices write "Unused" instead. Same semantics as the reference
    /// implementation, snd-firewire-ctl-services tcat.rs:548-563.
    static func labels(_ data: Data, _ offset: Int, _ length: Int) -> [String] {
        var out = [String]()
        for chunk in text(data, offset, length).components(separatedBy: "\\") {
            let name = chunk.trimmingCharacters(in: CharacterSet(charactersIn: "\0"))
            if name.isEmpty { break }
            out.append(name)
        }
        return out
    }

    static func nulTerminated(_ data: Data, _ offset: Int, _ length: Int) -> String {
        let raw = text(data, offset, length)
        return String(raw.prefix(while: { $0 != "\0" }))
    }
}

// MARK: - Enumerations

enum DiceClockSource: UInt32, CaseIterable {
    case aes1 = 0, aes2, aes3, aes4, aesAny, adat, tdif
    case wordClock, arx1, arx2, arx3, arx4, internalClock

    var label: String {
        switch self {
        case .aes1: return "aes1"
        case .aes2: return "aes2"
        case .aes3: return "aes3"
        case .aes4: return "aes4"
        case .aesAny: return "aes_any"
        case .adat: return "adat"
        case .tdif: return "tdif"
        case .wordClock: return "wc"
        case .arx1: return "arx1"
        case .arx2: return "arx2"
        case .arx3: return "arx3"
        case .arx4: return "arx4"
        case .internalClock: return "internal"
        }
    }

    static func name(_ raw: UInt32) -> String {
        DiceClockSource(rawValue: raw)?.label ?? "reserved(\(raw))"
    }
}

enum DiceClockRate {
    static func name(_ index: UInt32) -> String {
        switch index {
        case 0: return "32000"
        case 1: return "44100"
        case 2: return "48000"
        case 3: return "88200"
        case 4: return "96000"
        case 5: return "176400"
        case 6: return "192000"
        case 7: return "any-low"
        case 8: return "any-mid"
        case 9: return "any-high"
        case 10: return "none"
        default: return "reserved(\(index))"
        }
    }
}

/// dice-interface.h:194-213. Rates occupy bits 0-6, sources bits 16-28.
enum DiceClockCaps {
    static func rates(_ caps: UInt32) -> [String] {
        (0...6).compactMap { caps & (1 << $0) != 0 ? DiceClockRate.name(UInt32($0)) : nil }
    }

    static func sources(_ caps: UInt32) -> [String] {
        (0...12).compactMap {
            caps & (1 << (16 + $0)) != 0 ? DiceClockSource.name(UInt32($0)) : nil
        }
    }
}

/// dice-interface.h:144-170. Lock flags in the low half, slip flags in the high
/// half, in a different order from the clock-source enumeration.
enum DiceExtStatusBits {
    static let order: [String] = ["aes1", "aes2", "aes3", "aes4", "adat", "tdif",
                                  "arx1", "arx2", "arx3", "arx4", "wc"]

    static func locked(_ v: UInt32) -> [String] {
        order.enumerated().compactMap { v & (1 << $0.offset) != 0 ? $0.element : nil }
    }

    static func slipped(_ v: UInt32) -> [String] {
        order.enumerated().compactMap { v & (1 << (16 + $0.offset)) != 0 ? $0.element : nil }
    }
}

enum DiceNotificationBits {
    static func names(_ v: UInt32) -> [String] {
        var out = [String]()
        if v & 0x01 != 0 { out.append("RX_CFG_CHG") }
        if v & 0x02 != 0 { out.append("TX_CFG_CHG") }
        if v & 0x10 != 0 { out.append("LOCK_CHG") }
        if v & 0x20 != 0 { out.append("CLOCK_ACCEPTED") }
        if v & 0x40 != 0 { out.append("EXT_STATUS") }
        let known: UInt32 = 0x73
        if v & ~known != 0 {
            out.append(String(format: "vendor(0x%08x)", v & ~known))
        }
        return out
    }
}

// MARK: - Section table

struct DiceSection {
    let offsetQuadlets: UInt32
    let sizeQuadlets: UInt32

    var byteOffset: Int { Int(offsetQuadlets) * 4 }
    var byteSize: Int { Int(sizeQuadlets) * 4 }
    var isPresent: Bool { sizeQuadlets > 0 }
}

struct DiceSectionTable {
    let names: [String]
    let sections: [DiceSection]

    subscript(_ name: String) -> DiceSection? {
        guard let i = names.firstIndex(of: name) else { return nil }
        return sections[i]
    }

    static func decode(_ data: Data, names: [String]) -> DiceSectionTable? {
        var out = [DiceSection]()
        for i in 0..<names.count {
            guard let off = DiceWire.u32(data, i * 8),
                  let size = DiceWire.u32(data, i * 8 + 4) else { return nil }
            out.append(DiceSection(offsetQuadlets: off, sizeQuadlets: size))
        }
        return DiceSectionTable(names: names, sections: out)
    }

    static let generalNames = ["global", "tx", "rx", "ext_sync", "unused2"]
    static let extensionNames = ["caps", "cmd", "mixer", "peak", "router",
                                 "stream_format", "current_config",
                                 "standalone", "application"]
}

// MARK: - General space

struct DiceGlobal {
    var ownerNodeId: UInt32 = 0
    var ownerRaw: UInt64 = 0
    var notification: UInt32 = 0
    var nickname: String = ""
    var clockSelect: UInt32 = 0
    var enable: UInt32 = 0
    var status: UInt32 = 0
    var extendedStatus: UInt32 = 0
    var measuredRate: UInt32 = 0
    var version: UInt32 = 0
    var clockCaps: UInt32 = 0
    var clockSourceNames: [String] = []

    var clockSourceIndex: UInt32 { clockSelect & 0xFF }
    var clockRateIndex: UInt32 { (clockSelect >> 8) & 0xFF }
    var sourceLocked: Bool { status & 0x01 != 0 }
    var nominalRateIndex: UInt32 { (status >> 8) & 0xFF }
    var hasOwner: Bool { (ownerRaw >> 48) != 0xFFFF }

    /// `size` is the section's declared byte size; registers past it are absent
    /// on older firmware (dice-interface.h:26-32, 178-182).
    static func decode(_ d: Data, size: Int) -> DiceGlobal {
        var g = DiceGlobal()
        let hi = DiceWire.u32(d, 0x00) ?? 0
        let lo = DiceWire.u32(d, 0x04) ?? 0
        g.ownerRaw = (UInt64(hi) << 32) | UInt64(lo)
        g.ownerNodeId = hi >> 16
        g.notification = DiceWire.u32(d, 0x08) ?? 0
        g.nickname = DiceWire.nulTerminated(d, 0x0C, 64)
        g.clockSelect = DiceWire.u32(d, 0x4C) ?? 0
        g.enable = DiceWire.u32(d, 0x50) ?? 0
        g.status = DiceWire.u32(d, 0x54) ?? 0
        g.extendedStatus = DiceWire.u32(d, 0x58) ?? 0
        g.measuredRate = DiceWire.u32(d, 0x5C) ?? 0
        if size > 0x60 { g.version = DiceWire.u32(d, 0x60) ?? 0 }
        if size > 0x64 { g.clockCaps = DiceWire.u32(d, 0x64) ?? 0 }
        if size > 0x68 { g.clockSourceNames = DiceWire.labels(d, 0x68, min(256, max(0, size - 0x68))) }
        return g
    }
}

struct DiceStreamEntry {
    var index: Int = 0
    var isoChannel: Int32 = -1
    var sequenceStart: UInt32?   // RX only
    var pcmChannels: UInt32 = 0
    var midiPorts: UInt32 = 0
    var speed: UInt32?           // TX only
    var names: [String] = []
    /// Absent when the device's per-stream block is too short to contain the
    /// AC3 registers -- a register is only supported if its offset lies inside
    /// the declared size (dice-interface.h:26-32). A 70-quadlet block ends at
    /// 0x118, exactly where AC3_CAPABILITIES would start, so reading it anyway
    /// would return the next stream's data.
    var ac3Capabilities: UInt32?
    var ac3Enable: UInt32?

    var speedLabel: String {
        guard let s = speed else { return "-" }
        switch s {
        case 0: return "S100"
        case 1: return "S200"
        case 2: return "S400"
        case 3: return "S800"
        default: return "reserved(\(s))"
        }
    }
}

/// TX and RX share a layout apart from RX inserting SEQ_START at 0x0C, which
/// shifts the channel counts down one quadlet (dice-interface.h:231-346).
/// Per-stream register offsets are relative to the SECTION base and already sit
/// past NUMBER/SIZE, so stream *i* lives at `base + offset + i * size * 4`.
enum DiceStreamSection {
    static func decode(_ d: Data, isTx: Bool) -> (count: UInt32, sizeQuadlets: UInt32, entries: [DiceStreamEntry]) {
        let count = DiceWire.u32(d, 0x00) ?? 0
        let size = DiceWire.u32(d, 0x04) ?? 0
        guard count > 0, size > 0 else { return (count, size, []) }

        var entries = [DiceStreamEntry]()
        let stride = Int(size) * 4
        for i in 0..<Int(count) {
            let b = i * stride
            var e = DiceStreamEntry()
            e.index = i
            e.isoChannel = Int32(bitPattern: DiceWire.u32(d, b + 0x08) ?? 0xFFFF_FFFF)
            if isTx {
                e.pcmChannels = DiceWire.u32(d, b + 0x0C) ?? 0
                e.midiPorts = DiceWire.u32(d, b + 0x10) ?? 0
                e.speed = DiceWire.u32(d, b + 0x14)
            } else {
                e.sequenceStart = DiceWire.u32(d, b + 0x0C)
                e.pcmChannels = DiceWire.u32(d, b + 0x10) ?? 0
                e.midiPorts = DiceWire.u32(d, b + 0x14) ?? 0
            }
            // Names occupy 0x18..0x118; clamp to whatever the block actually has.
            e.names = DiceWire.labels(d, b + 0x18, min(256, max(0, stride - 0x18)))
            if stride > 0x11C {
                e.ac3Capabilities = DiceWire.u32(d, b + 0x118)
                e.ac3Enable = DiceWire.u32(d, b + 0x11C)
            }
            entries.append(e)
        }
        return (count, size, entries)
    }
}

struct DiceExtSync {
    var clockSource: UInt32 = 0
    var locked: Bool = false
    var rateIndex: UInt32 = 0
    var adatUserData: UInt8?

    static func decode(_ d: Data) -> DiceExtSync {
        var x = DiceExtSync()
        x.clockSource = DiceWire.u32(d, 0x00) ?? 0
        x.locked = (DiceWire.u32(d, 0x04) ?? 0) != 0
        x.rateIndex = DiceWire.u32(d, 0x08) ?? 0
        let ad = DiceWire.u32(d, 0x0C) ?? 0
        // Bit 4 set means the bits are unavailable (dice-interface.h:373-376).
        x.adatUserData = (ad & 0x10) != 0 ? nil : UInt8(ad & 0x0F)
        return x
    }
}

// MARK: - Extension (EAP) space

enum DiceAsicType: UInt32 {
    case diceII = 0
    case tcd2210 = 1
    case tcd2220 = 2

    var label: String {
        switch self {
        case .diceII: return "DICE II"
        case .tcd2210: return "TCD2210 (DICE Mini)"
        case .tcd2220: return "TCD2220 (DICE Jr)"
        }
    }
}

struct DiceEapCaps {
    // Router
    var routerExposed = false
    var routerReadOnly = false
    var routerStorable = false
    var routerMaxEntries: UInt16 = 0
    // Mixer
    var mixerExposed = false
    var mixerReadOnly = false
    var mixerStorable = false
    var mixerInputDeviceId: UInt8 = 0
    var mixerOutputDeviceId: UInt8 = 0
    var mixerInputCount: UInt8 = 0
    var mixerOutputCount: UInt8 = 0
    // General
    var dynamicStreamFormat = false
    var storageAvailable = false
    var peakAvailable = false
    var maxTxStreams: UInt8 = 0
    var maxRxStreams: UInt8 = 0
    var streamFormatStorable = false
    var asicTypeRaw: UInt32 = 0

    var asicLabel: String {
        DiceAsicType(rawValue: asicTypeRaw)?.label ?? "unknown(\(asicTypeRaw))"
    }

    /// caps_section.rs: quadlet 0 router, 1 mixer, 2 general.
    static func decode(_ d: Data) -> DiceEapCaps? {
        guard let r = DiceWire.u32(d, 0x00),
              let m = DiceWire.u32(d, 0x04),
              let g = DiceWire.u32(d, 0x08) else { return nil }
        var c = DiceEapCaps()
        c.routerExposed = r & 0x01 != 0
        c.routerReadOnly = r & 0x02 != 0
        c.routerStorable = r & 0x04 != 0
        c.routerMaxEntries = UInt16((r & 0xFFFF_0000) >> 16)

        c.mixerExposed = m & 0x01 != 0
        c.mixerReadOnly = m & 0x02 != 0
        c.mixerStorable = m & 0x04 != 0
        c.mixerInputDeviceId = UInt8((m >> 8) & 0x0F)
        c.mixerOutputDeviceId = UInt8((m >> 12) & 0x0F)
        c.mixerInputCount = UInt8((m >> 16) & 0xFF)
        c.mixerOutputCount = UInt8((m >> 24) & 0xFF)

        c.dynamicStreamFormat = g & 0x01 != 0
        c.storageAvailable = g & 0x02 != 0
        c.peakAvailable = g & 0x04 != 0
        c.maxTxStreams = UInt8((g & 0x0000_00F0) >> 4)
        c.maxRxStreams = UInt8((g & 0x0000_0F00) >> 8)
        c.streamFormatStorable = g & 0x1000 != 0
        c.asicTypeRaw = (g >> 16) & 0xFF
        return c
    }
}

/// router_entry.rs: one quadlet — dst in [7:0], src in [15:8], peak in [31:16];
/// each block byte packs id in [7:4] and channel in [3:0].
struct DiceRouterEntry {
    var dstId: UInt8
    var dstChannel: UInt8
    var srcId: UInt8
    var srcChannel: UInt8
    var peak: UInt16

    static func dstName(_ id: UInt8) -> String {
        switch id {
        case 0: return "AES"
        case 1: return "ADAT"
        case 2: return "MixerTx0"
        case 3: return "MixerTx1"
        case 4: return "Ins0"
        case 5: return "Ins1"
        case 10: return "ArmApbAudio"
        case 11: return "Avs0"
        case 12: return "Avs1"
        default: return "reserved(\(id))"
        }
    }

    static func srcName(_ id: UInt8) -> String {
        switch id {
        case 0: return "AES"
        case 1: return "ADAT"
        case 2: return "Mixer"
        case 4: return "Ins0"
        case 5: return "Ins1"
        case 10: return "ArmAprAudio"
        case 11: return "Avs0"
        case 12: return "Avs1"
        case 15: return "Mute"
        default: return "reserved(\(id))"
        }
    }

    var dstLabel: String { "\(DiceRouterEntry.dstName(dstId)):\(dstChannel)" }
    var srcLabel: String { "\(DiceRouterEntry.srcName(srcId)):\(srcChannel)" }

    static func decode(_ v: UInt32) -> DiceRouterEntry {
        let dst = UInt8(v & 0xFF)
        let src = UInt8((v >> 8) & 0xFF)
        return DiceRouterEntry(dstId: (dst & 0xF0) >> 4, dstChannel: dst & 0x0F,
                               srcId: (src & 0xF0) >> 4, srcChannel: src & 0x0F,
                               peak: UInt16((v >> 16) & 0xFFFF))
    }

    /// `hasLeadingCount` distinguishes the router section (quadlet 0 is the
    /// entry count) from the peak section (entries only, length taken from
    /// `routerMaxEntries`). router_section.rs:24-34, peak_section.rs:30-38.
    static func decodeAll(_ d: Data, hasLeadingCount: Bool, maxEntries: Int) -> [DiceRouterEntry] {
        var base = 0
        var count = maxEntries
        if hasLeadingCount {
            count = Int(DiceWire.u32(d, 0) ?? 0)
            base = 4
        }
        count = min(count, max(0, (d.count - base) / 4))
        return (0..<count).compactMap { i in
            DiceWire.u32(d, base + i * 4).map { DiceRouterEntry.decode($0) }
        }
    }
}

/// stream_format_entry.rs: 268 bytes — pcm and midi counts each occupy a full
/// quadlet, 256 bytes of labels, then a 32-bit AC3 mask.
struct DiceEapFormatEntry {
    var pcmChannels: UInt32 = 0
    var midiPorts: UInt32 = 0
    var names: [String] = []
    var ac3Mask: UInt32 = 0

    static let size = 268

    static func decode(_ d: Data, at offset: Int) -> DiceEapFormatEntry? {
        guard offset + size <= d.count else { return nil }
        var e = DiceEapFormatEntry()
        e.pcmChannels = (DiceWire.u32(d, offset) ?? 0) & 0xFF
        e.midiPorts = (DiceWire.u32(d, offset + 4) ?? 0) & 0xFF
        e.names = DiceWire.labels(d, offset + 8, 256)
        e.ac3Mask = DiceWire.u32(d, offset + 264) ?? 0
        return e
    }
}

struct DiceEapStreamFormats {
    var txEntries: [DiceEapFormatEntry] = []
    var rxEntries: [DiceEapFormatEntry] = []

    static func decode(_ d: Data) -> DiceEapStreamFormats {
        var s = DiceEapStreamFormats()
        let txCount = Int(DiceWire.u32(d, 0) ?? 0)
        let rxCount = Int(DiceWire.u32(d, 4) ?? 0)
        var pos = 8
        for _ in 0..<txCount {
            guard let e = DiceEapFormatEntry.decode(d, at: pos) else { break }
            s.txEntries.append(e)
            pos += DiceEapFormatEntry.size
        }
        for _ in 0..<rxCount {
            guard let e = DiceEapFormatEntry.decode(d, at: pos) else { break }
            s.rxEntries.append(e)
            pos += DiceEapFormatEntry.size
        }
        return s
    }
}

/// current_config_section.rs:29-34. Router and stream configuration for each of
/// the three rate modes, at fixed offsets inside the section.
enum DiceRateMode: Int, CaseIterable {
    case low = 0, middle, high

    var label: String {
        switch self {
        case .low: return "low (32-48k)"
        case .middle: return "middle (88.2-96k)"
        case .high: return "high (176.4-192k)"
        }
    }

    var routerOffset: Int {
        switch self {
        case .low: return 0x0000
        case .middle: return 0x2000
        case .high: return 0x4000
        }
    }

    var streamOffset: Int {
        switch self {
        case .low: return 0x1000
        case .middle: return 0x3000
        case .high: return 0x5000
        }
    }
}

struct DiceStandalone {
    var clockSource: UInt32 = 0
    var aesHighRate: Bool = false
    var adatModeRaw: UInt32 = 0
    var wordClockRaw: UInt32 = 0
    var internalRateIndex: UInt32 = 0

    var adatModeLabel: String {
        switch adatModeRaw & 0x03 {
        case 0: return "Normal"
        case 1: return "SMUX2"
        case 2: return "SMUX4"
        default: return "Auto"
        }
    }

    var wordClockModeLabel: String {
        switch wordClockRaw & 0x03 {
        case 0: return "Normal"
        case 1: return "Low"
        case 2: return "Middle"
        default: return "High"
        }
    }

    var wordClockNumerator: UInt16 { UInt16((wordClockRaw >> 4) & 0xFFF) }
    var wordClockDenominator: UInt16 { UInt16((wordClockRaw >> 16) & 0xFFFF) }

    /// standalone_section.rs:11-15 (offsets) and :63-74 (fields).
    static func decode(_ d: Data) -> DiceStandalone {
        var s = DiceStandalone()
        s.clockSource = DiceWire.u32(d, 0x00) ?? 0
        s.aesHighRate = (DiceWire.u32(d, 0x04) ?? 0) != 0
        s.adatModeRaw = DiceWire.u32(d, 0x08) ?? 0
        s.wordClockRaw = DiceWire.u32(d, 0x0C) ?? 0
        s.internalRateIndex = DiceWire.u32(d, 0x10) ?? 0
        return s
    }
}

/// mixer_section.rs:18-25. Saturation bitmask at 0x00, then a fixed
/// 16-output x 18-input grid of quadlets at 0x04 regardless of the advertised
/// counts; only the advertised sub-rectangle is meaningful.
struct DiceMixer {
    static let maxOutputs = 16
    static let maxInputs = 18

    var saturation: UInt32 = 0
    var coefficients: [[UInt16]] = []

    static func decode(_ d: Data, caps: DiceEapCaps) -> DiceMixer {
        var m = DiceMixer()
        m.saturation = DiceWire.u32(d, 0x00) ?? 0
        let outs = min(Int(caps.mixerOutputCount), maxOutputs)
        let ins = min(Int(caps.mixerInputCount), maxInputs)
        guard outs > 0, ins > 0 else { return m }
        m.coefficients = (0..<outs).map { o in
            (0..<ins).map { i in
                let pos = 0x04 + 4 * (o * maxInputs + i)
                return UInt16((DiceWire.u32(d, pos) ?? 0) & 0xFFFF)
            }
        }
        return m
    }
}

// MARK: - Snapshot

struct DiceSnapshot {
    var nodeId: UInt16 = 0
    var generation: UInt32 = 0
    var guid: UInt64 = 0
    var vendorName: String = ""
    var modelName: String = ""

    // General space
    var generalSections: DiceSectionTable?
    var global: DiceGlobal?
    var txCount: UInt32 = 0
    var txSizeQuadlets: UInt32 = 0
    var txStreams: [DiceStreamEntry] = []
    var rxCount: UInt32 = 0
    var rxSizeQuadlets: UInt32 = 0
    var rxStreams: [DiceStreamEntry] = []
    var extSync: DiceExtSync?

    // Extension space
    var extensionSections: DiceSectionTable?
    var eapCaps: DiceEapCaps?
    var eapStreamFormats: DiceEapStreamFormats?
    var eapRouter: [DiceRouterEntry] = []
    var eapPeak: [DiceRouterEntry] = []
    var eapMixer: DiceMixer?
    var eapStandalone: DiceStandalone?
    var eapCurrentRouters: [DiceRateMode: [DiceRouterEntry]] = [:]
    var eapCurrentFormats: [DiceRateMode: DiceEapStreamFormats] = [:]

    var notes: [String] = []

    /// The rate mode the device is currently in, derived from the nominal rate
    /// in GLOBAL_STATUS. Peak meters and the active router belong to this mode.
    var activeRateMode: DiceRateMode? {
        guard let index = global?.nominalRateIndex else { return nil }
        switch index {
        case 0, 1, 2: return .low
        case 3, 4: return .middle
        case 5, 6: return .high
        default: return nil
        }
    }

    /// Peak entries mirror router entries positionally, but the section is
    /// sized by routerMaxEntries. Everything past the active router's entry
    /// count is uninitialised and must not be presented as data.
    var meaningfulPeakCount: Int {
        guard let mode = activeRateMode,
              let routes = eapCurrentRouters[mode], routes.isEmpty == false else {
            return eapPeak.count
        }
        return min(routes.count, eapPeak.count)
    }

    /// TCAT encodes vendor, category, product and serial into the GUID's chip
    /// ID (tcat/config_rom.rs:13-23). Category is the value that separates
    /// device families -- notably Weiss uses 0x00 where most DICE uses 0x04.
    var tcatVendorId: UInt32 { UInt32((guid >> 40) & 0xFF_FFFF) }
    var tcatCategory: UInt8 { UInt8((guid >> 32) & 0xFF) }
    var tcatProductId: UInt16 { UInt16((guid >> 22) & 0x3FF) }
    var tcatSerial: UInt32 { UInt32(guid & 0x3F_FFFF) }
}
