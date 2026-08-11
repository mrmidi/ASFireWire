//
//  AVCReportModels.swift
//  ASFW
//
//  Data model for the AV/C Device Report tab -- the family-agnostic core.
//
//  Almost everything a FireWire audio device answers is TA 1394 AV/C, not a
//  vendor protocol: PLUG_INFO, extended plug info, extended stream format and
//  the Music subunit are shared across BridgeCo (BeBoB), Oxford, Fireworks and
//  plain generic AV/C units. FFADO draws the same seam -- bebob::Device,
//  fireworks::Device and oxford::Device all derive from GenericAVC::Device
//  (libffado-2.5.0/src/bebob/bebob_avdevice.h:61, fireworks_device.h:45,
//  oxford_device.h:39) over a shared libavc/general + libavc/streamformat.
//
//  So this file holds the generic probe; per-family extras (the BridgeCo
//  BootROM, and later Oxford/Fireworks equivalents) attach as optional
//  sections. See BridgeCoReportModels.swift for the first such section.
//
//  SAFETY: probe depth is NOT a generic-AV/C or per-family property -- it is a
//  per-device fact. Some firmware hangs permanently on commands it does not
//  implement, requiring a power cycle. M-Audio's "special" firmware (FireWire
//  1814, ProjectMix I/O) is the known case:
//    - linux-sound-firewire-stack/firewire/bebob/bebob_maudio.c:30-34
//      ("It will be freezed when receiving any commands which the firmware
//       can't understand.")
//    - linux-sound-firewire-stack/firewire/bebob/bebob_stream.c:417
//      (map_data_channels is skipped entirely for the special quirk)
//    - libffado-2.5.0/src/bebob/bebob_avdevice.cpp:88-91
//      ("M-Audio Special Devices don't support followed commands")
//  The tier is therefore decided from Config ROM identity alone, before a
//  single byte is sent to the device.
//

import Foundation

// MARK: - Probe policy

/// How much of the device the report is allowed to touch.
enum AVCProbeTier: Equatable {
    /// Memory reads only: Config ROM plus any family memory block. No AV/C,
    /// no FCP. Safe on every device including firmware that hangs on unknown
    /// commands.
    case memoryOnly
    /// Memory reads plus AV/C STATUS commands (never CONTROL). Safe on
    /// firmware that implements the standard AV/C command set.
    case avcStatus

    var label: String {
        switch self {
        case .memoryOnly: return "memory-only (no AV/C)"
        case .avcStatus:  return "memory + AV/C STATUS"
        }
    }
}

struct AVCProbePolicy: Equatable {
    let tier: AVCProbeTier
    /// Why this tier was chosen. Always printed, so the report is honest about
    /// what it did and did not ask the device.
    let rationale: String
}

/// Devices that must not be probed, regardless of protocol family.
///
/// This is a deny-list, not an allow-list: an unknown device gets the full
/// STATUS probe, because refusing to probe unknown hardware would defeat the
/// report's purpose. Entries are added only with a reference citation showing
/// the firmware actually misbehaves.
enum AVCProbeRestrictions {

    struct Entry {
        let vendorId: UInt32
        let modelId: UInt32
        let name: String
        /// Printed verbatim in the report.
        let reason: String
    }

    static let entries: [Entry] = BridgeCoKnownDevice.probeRestrictions

    static func entry(vendorId: UInt32, modelId: UInt32) -> Entry? {
        entries.first { $0.vendorId == vendorId && $0.modelId == modelId }
    }

    /// The probe tier for an identity, decided before any transaction is sent.
    static func policy(vendorId: UInt32, modelId: UInt32) -> AVCProbePolicy {
        if let restricted = entry(vendorId: vendorId, modelId: modelId) {
            return AVCProbePolicy(tier: .memoryOnly, rationale: restricted.reason)
        }
        return AVCProbePolicy(
            tier: .avcStatus,
            rationale: "No probe restriction recorded for this identity; "
                + "STATUS-only AV/C probing is safe.")
    }
}

// MARK: - Generic AV/C inventory

/// Unit-level isochronous and external plug counts.
/// AV/C General PLUG_INFO (opcode 0x02, subfunction 0x00).
/// FFADO libavc/general/avc_plug_info.h.
struct AVCUnitPlugCounts: Equatable {
    let isochronousInput: UInt8
    let isochronousOutput: UInt8
    let externalInput: UInt8
    let externalOutput: UInt8
}

/// One subunit as reported by AV/C SUBUNIT INFO (opcode 0x31).
/// FFADO libavc/general/avc_subunit_info.h.
struct AVCSubunitEntry: Equatable {
    let type: UInt8
    let id: UInt8
    let sourcePlugs: UInt8
    let destinationPlugs: UInt8

    var typeName: String {
        switch type {
        case 0x00: return "Video Monitor"
        case 0x01: return "Audio"
        case 0x03: return "Disc"
        case 0x04: return "Tape Recorder/Player"
        case 0x05: return "Tuner"
        case 0x07: return "Camera"
        case 0x0A: return "Panel"
        case 0x0C: return "Music"
        case 0x1C: return "Vendor Unique"
        default: return String(format: "Unknown (0x%02X)", type)
        }
    }
}

/// One Music subunit plug and its AV/C plug type.
/// AV/C Music Subunit + extended plug info (opcode 0x02 subfunction 0xC0).
/// FFADO libavc/general/avc_extended_plug_info.h.
struct AVCMusicPlug: Equatable {
    let index: UInt8
    let type: UInt8
    let typeName: String
}

/// Where a piece of inventory came from. A report that cannot distinguish
/// "the device answered zero" from "nobody asked" is worse than one that says
/// nothing: an M-Audio FW 1814 in bootloader state printed four zero plug
/// counts that had never been queried at all (2026-08-10).
enum AVCInventoryProvenance: Equatable {
    /// Suppressed by probe policy, or the tier never reached this query.
    case notQueried
    /// Reused from the driver's discovery cache rather than re-asked. The
    /// driver publishes a unit before its probe completes, so this data can be
    /// a partial view of a probe that failed or never finished.
    case driverDiscovery
    /// This report asked the device and it answered.
    case probed

    var label: String {
        switch self {
        case .notQueried: return "not queried"
        case .driverDiscovery: return "from driver discovery, not re-probed"
        case .probed: return "probed by this report"
        }
    }
}

/// Everything the generic AV/C probe learned. Every field here is answerable
/// by any conforming AV/C unit -- nothing in this struct is vendor-specific.
struct AVCUnitInventory {
    var unitPlugs: AVCUnitPlugCounts?
    var subunits: [AVCSubunitEntry] = []
    /// Nil when the device was not asked, empty when it reported none.
    var musicSubunitInputPlugCount: UInt8?
    var musicSubunitInputPlugs: [AVCMusicPlug] = []
    var unitPlugsProvenance: AVCInventoryProvenance = .notQueried
    var subunitsProvenance: AVCInventoryProvenance = .notQueried
}

// MARK: - Snapshot

/// Everything the report managed to learn about one node.
struct AVCDeviceSnapshot {
    var guid: UInt64 = 0
    var nodeId: UInt16 = 0
    var generation: UInt32 = 0
    var vendorId: UInt32 = 0
    var modelId: UInt32 = 0
    var vendorName: String = ""
    var modelName: String = ""

    var policy: AVCProbePolicy = .init(tier: .memoryOnly, rationale: "")
    var inventory = AVCUnitInventory()

    /// Family-specific section. Today only BridgeCo exists; Oxford and
    /// Fireworks attach here the same way when they are implemented.
    var bridgeCo: BridgeCoSection?

    /// Non-fatal observations, printed verbatim.
    var notes: [String] = []

    mutating func note(_ text: String) { notes.append(text) }

    /// A node belongs in this report when it is an AV/C unit at all: either it
    /// answered a generic AV/C probe, or a family memory block identified it.
    /// Identity matching is deliberately not the test -- the report exists to
    /// describe devices ASFW does not support yet.
    var isAVCDevice: Bool {
        inventory.unitPlugs != nil || bridgeCo != nil || !inventory.subunits.isEmpty
    }

    /// Which family sections were recognised, for the report header.
    var familyLabels: [String] {
        var labels = [String]()
        if bridgeCo != nil { labels.append("BridgeCo / BeBoB") }
        return labels
    }
}

enum AVCSnapshotCaptureResult {
    case captured(AVCDeviceSnapshot)
    case notAVC(guid: UInt64)
    case unavailable(guid: UInt64)
    case unstable(guid: UInt64)
}
