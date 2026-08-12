//
//  BridgeCoReportModels.swift
//  ASFW
//
//  The BridgeCo (BeBoB) family section of the AV/C Device Report.
//
//  Only two things about a BeBoB device are genuinely BridgeCo rather than
//  TA 1394 AV/C:
//    1. the BootROM information block at 0xFFFFC8020000 ("bridgeCo" magic),
//       which carries boot state and firmware vintage; and
//    2. per-model quirks -- which firmware hangs on probing, which models boot
//       into a bootloader, and the stream geometry of models that cannot be
//       queried at all.
//  Everything else (PLUG_INFO, extended plug info, stream format, Music
//  subunit) is generic AV/C and lives in AVCReportModels.swift.
//
//  Device IDs cross-validated with the Linux BeBoB id table
//  (linux-sound-firewire-stack/firewire/bebob/bebob.c:366-485) and
//  libffado-2.5.0/src/bebob/bebob_avdevice.cpp:178-200.
//

import Foundation

// MARK: - Section payload

/// What the BridgeCo probe found. Attached to AVCDeviceSnapshot.bridgeCo.
struct BridgeCoSection {
    /// BridgeCo BootROM information block. Present on every BeBoB device,
    /// including ones sitting in bootloader state.
    var bootRom: ASFWMCPBeBoBBootRomInformation?
    /// Set when the block read succeeded but the payload was not a BootROM.
    var unrecognized = false
    /// Raw bytes, so an unrecognized layout can still be reported usefully.
    var raw: Data?

    var vendorId: UInt32 = 0
    var modelId: UInt32 = 0

    /// The M-Audio boot cue is only available on firmware 5058 or later, which
    /// Linux gates on the BootROM software build date being on or after
    /// 20070401 (bebob_maudio.c:99-113). Older units need a full firmware blob
    /// upload instead, which ASFW does not implement.
    var bootCueSupported: Bool? {
        guard let stamp = bootRom?.softwareTimestamp else { return nil }
        let date = String(stamp.prefix(8))
        guard date.count == 8, date.allSatisfy({ $0.isNumber }) else { return nil }
        return date >= "20070401"
    }

    var bootloaderIdentity: BridgeCoKnownDevice.BootloaderIdentity? {
        BridgeCoKnownDevice.bootloader(vendorId: vendorId, modelId: modelId)
    }

    var specialFirmware: BridgeCoKnownDevice.SpecialFirmware? {
        BridgeCoKnownDevice.specialFirmware(vendorId: vendorId, modelId: modelId)
    }

    var referenceGeometry: BridgeCoKnownDevice.ReferenceGeometry? {
        BridgeCoKnownDevice.referenceGeometry(vendorId: vendorId, modelId: modelId)
    }
}

// MARK: - Known-device knowledge

enum BridgeCoKnownDevice {

    static let vendorMAudio: UInt32    = 0x00_0D6C
    static let vendorBridgeCo: UInt32  = 0x00_07F5
    static let vendorTerraTec: UInt32  = 0x00_0AAC
    static let vendorFocusrite: UInt32 = 0x00_130E
    static let vendorEdirol: UInt32    = 0x00_40AB
    static let vendorPreSonus: UInt32  = 0x00_0A92
    static let vendorYamaha: UInt32    = 0x00_A0DE
    static let vendorMackie: UInt32    = 0x00_0FF2
    static let vendorApogee: UInt32    = 0x00_03DB

    /// A device whose firmware hangs on unimplemented AV/C commands.
    /// bebob_maudio.c:30-34; libffado bebob_avdevice.cpp:88-91.
    struct SpecialFirmware {
        let vendorId: UInt32
        let modelId: UInt32
        let name: String
    }

    static let specialFirmware: [SpecialFirmware] = [
        .init(vendorId: vendorMAudio, modelId: 0x0001_0071, name: "M-Audio FireWire 1814"),
        .init(vendorId: vendorMAudio, modelId: 0x0001_0091, name: "M-Audio ProjectMix I/O"),
    ]

    /// Models that enumerate in bootloader state and need a boot cue before
    /// they present themselves as an audio device.
    /// bebob.c:450-451 (BridgeCo 0x00010058 -> 0x00010046) and
    /// bebob.c:464-465 (M-Audio 0x00010070 -> 0x00010071).
    struct BootloaderIdentity {
        let vendorId: UInt32
        let bootloaderModelId: UInt32
        let bootedModelId: UInt32
        let name: String
    }

    static let bootloaders: [BootloaderIdentity] = [
        .init(vendorId: vendorMAudio, bootloaderModelId: 0x0001_0070,
              bootedModelId: 0x0001_0071, name: "M-Audio FireWire 1814"),
        .init(vendorId: vendorBridgeCo, bootloaderModelId: 0x0001_0058,
              bootedModelId: 0x0001_0046, name: "M-Audio FireWire 410"),
    ]

    /// Devices whose stream geometry cannot be queried and is therefore known
    /// only from the reference stacks. Printed as reference-derived, never
    /// presented as something the device told us.
    struct ReferenceFormation {
        let rateHz: UInt32
        let capturePcm: UInt16    // device -> host
        let playbackPcm: UInt16   // host -> device
        let midiSlots: UInt16
    }

    struct ReferenceGeometry {
        let vendorId: UInt32
        let modelId: UInt32
        let citation: String
        /// Keyed by the device's digital-interface mode, which is not
        /// queryable either -- it is a CONTROL-only setting.
        let modes: [(mode: String, formations: [ReferenceFormation])]
    }

    /// M-Audio FireWire 1814 / ProjectMix I/O.
    /// bebob_maudio.c:227-254 (special_stream_formation_set + ch_table).
    /// 32 kHz is absent from the table: the loop fills formations[i+1], so
    /// index 0 (32 kHz) is never written.
    static let referenceGeometry: [ReferenceGeometry] = [
        .init(vendorId: vendorMAudio, modelId: 0x0001_0071,
              citation: "linux bebob_maudio.c:227-254 (ch_table)",
              modes: [
                ("S/PDIF (default after init)", [
                    .init(rateHz: 44_100,  capturePcm: 10, playbackPcm: 6, midiSlots: 1),
                    .init(rateHz: 48_000,  capturePcm: 10, playbackPcm: 6, midiSlots: 1),
                    .init(rateHz: 88_200,  capturePcm: 10, playbackPcm: 6, midiSlots: 1),
                    .init(rateHz: 96_000,  capturePcm: 10, playbackPcm: 6, midiSlots: 1),
                    .init(rateHz: 176_400, capturePcm: 2,  playbackPcm: 4, midiSlots: 1),
                    .init(rateHz: 192_000, capturePcm: 2,  playbackPcm: 4, midiSlots: 1),
                ]),
                ("ADAT", [
                    .init(rateHz: 44_100,  capturePcm: 16, playbackPcm: 12, midiSlots: 1),
                    .init(rateHz: 48_000,  capturePcm: 16, playbackPcm: 12, midiSlots: 1),
                    .init(rateHz: 88_200,  capturePcm: 12, playbackPcm: 8,  midiSlots: 1),
                    .init(rateHz: 96_000,  capturePcm: 12, playbackPcm: 8,  midiSlots: 1),
                    .init(rateHz: 176_400, capturePcm: 2,  playbackPcm: 4,  midiSlots: 1),
                    .init(rateHz: 192_000, capturePcm: 2,  playbackPcm: 4,  midiSlots: 1),
                ]),
              ]),
    ]

    // MARK: Lookups

    static func specialFirmware(vendorId: UInt32, modelId: UInt32) -> SpecialFirmware? {
        specialFirmware.first { $0.vendorId == vendorId && $0.modelId == modelId }
    }

    static func bootloader(vendorId: UInt32, modelId: UInt32) -> BootloaderIdentity? {
        bootloaders.first { $0.vendorId == vendorId && $0.bootloaderModelId == modelId }
    }

    static func referenceGeometry(vendorId: UInt32, modelId: UInt32) -> ReferenceGeometry? {
        referenceGeometry.first { $0.vendorId == vendorId && $0.modelId == modelId }
    }
}
