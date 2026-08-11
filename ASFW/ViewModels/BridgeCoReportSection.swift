//
//  BridgeCoReportSection.swift
//  ASFW
//
//  The BridgeCo (BeBoB) family section of the AV/C Device Report.
//
//  Renders only what is genuinely BridgeCo: the BootROM information block,
//  the boot-state and firmware verdicts derived from it, and reference stream
//  geometry for models whose firmware cannot be asked. The generic AV/C body
//  lives in AVCReportTextFormatter.
//

import Foundation

enum BridgeCoReportSection {

    static func render(_ s: BridgeCoSection, into e: AVCReportTextFormatter.Emitter) {
        // Called directly rather than bound to locals: turning a main-actor
        // isolated static method into a function value crosses isolation and
        // is diagnosed under the target's MainActor-by-default setting.
        func pad(_ t: String, _ w: Int) -> String { AVCReportTextFormatter.pad(t, w) }
        func leftPad(_ t: String, _ w: Int) -> String { AVCReportTextFormatter.leftPad(t, w) }
        func version(_ v: UInt32) -> String { AVCReportTextFormatter.version(v) }

        // MARK: Boot state
        e.head("BRIDGECO — BOOT STATE")
        if let boot = s.bootloaderIdentity {
            e.line("State:  BOOTLOADER")
            e.line("Device: \(boot.name)")
            e.line(String(format: "This unit is enumerating as model 0x%06X (bootloader).",
                          boot.bootloaderModelId))
            e.line(String(format: "After a successful boot cue it re-enumerates as 0x%06X.",
                          boot.bootedModelId))
            e.line("It cannot stream audio in this state.")
            e.line("  cross-validated: linux bebob.c:464-465")
        } else if let special = s.specialFirmware {
            e.line("State:  BOOTED (special firmware)")
            e.line("Device: \(special.name)")
            e.line("The device has loaded its operational firmware.")
        } else {
            e.line("State:  BOOTED (no bootloader identity known for this model)")
        }

        // MARK: BootROM
        e.head("BRIDGECO — BOOTROM INFORMATION  (0xFFFFC8020000)")
        if let rom = s.bootRom {
            e.line("Magic:              bridgeCo  (recognised)")
            e.line(String(format: "Protocol version:   0x%08X", rom.protocolVersion))
            e.line("Bootloader:         \(version(rom.bootloaderVersion))"
                   + "  built \(rom.bootloaderTimestamp)")
            e.line(String(format: "Hardware model ID:  0x%06X", rom.hardwareModelId))
            e.line("Hardware revision:  \(version(rom.hardwareRevision))")
            e.line("Software build:     \(rom.softwareTimestamp)")
            e.line(String(format: "Software ID:        0x%08X", rom.softwareId))
            e.line("Software revision:  \(version(rom.softwareVersion))")
            e.line(String(format: "Image base:         0x%08X", rom.imageBaseAddress))
            e.line(String(format: "Image max size:     %d bytes", rom.imageMaximumSize))
            e.line(String(format: "BootROM GUID:       0x%016llX", rom.guid))
            if let dbg = rom.debugger {
                e.line("Debugger:           \(version(dbg.version))  built \(dbg.timestamp)"
                       + String(format: "  id=0x%08X", dbg.id))
            } else {
                e.line("Debugger:           <absent>")
            }
        } else if s.unrecognized {
            e.line("<read succeeded but the BridgeCo magic was not present>")
            if let raw = s.raw {
                e.line("First 32 bytes: " + AVCReportTextFormatter.hexDump(raw.prefix(32)))
            }
        } else {
            e.line("<unreadable>")
        }

        // MARK: Firmware verdict
        e.head("BRIDGECO — FIRMWARE VERDICT")
        switch s.bootCueSupported {
        case .some(true):
            e.line("Boot cue:   SUPPORTED")
            e.line("The BootROM software build date is on or after 20070401, which is")
            e.line("Linux's gate for firmware 5058 or later. Devices at this vintage keep")
            e.line("their firmware in flash, so booting them needs only a three-quadlet")
            e.line("cue write — not a full firmware blob upload.")
            e.line("  cross-validated: linux bebob_maudio.c:99-113")
        case .some(false):
            e.line("Boot cue:   NOT SUPPORTED")
            e.line("The BootROM software build date is earlier than 20070401 (firmware")
            e.line("older than 5058). This unit expects a full firmware blob upload at")
            e.line("every power-on, which ASFW does not implement. Update the firmware")
            e.line("with the vendor's updater before expecting ASFW to drive it.")
            e.line("  cross-validated: linux bebob_maudio.c:99-113")
        case .none:
            e.line("Boot cue:   UNKNOWN (no readable BootROM software build date)")
        }

        // MARK: Reference geometry
        guard let geo = s.referenceGeometry else { return }
        e.head("BRIDGECO — REFERENCE STREAM GEOMETRY  (NOT queried from the device)")
        e.line("This device's stream formation cannot be queried — its firmware does")
        e.line("not answer plug-info commands. The table below comes from the")
        e.line("reference stack and is what ASFW would have to hard-code.")
        e.line("Source: \(geo.citation)")
        for mode in geo.modes {
            e.line("")
            e.line("  Mode: \(mode.mode)")
            e.line("    " + pad("rate", 12) + leftPad("capture", 9) + leftPad("playback", 10)
                   + leftPad("MIDI", 6))
            e.line("    " + pad("", 12) + leftPad("(dev->host)", 9) + leftPad("(host->dev)", 10)
                   + leftPad("slots", 6))
            for f in mode.formations {
                e.line("    " + pad(AVCReportTextFormatter.rateLabel(f.rateHz), 12)
                       + leftPad(String(f.capturePcm), 9)
                       + leftPad(String(f.playbackPcm), 10)
                       + leftPad(String(f.midiSlots), 6))
            }
        }
        e.line("")
        e.line("  Channel counts are PCM; each direction carries the MIDI slots in")
        e.line("  addition, so DBS = PCM + MIDI slots.")
    }
}
