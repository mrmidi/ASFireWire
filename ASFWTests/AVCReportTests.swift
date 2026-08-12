import Foundation
import Testing
@testable import ASFW

struct AVCProbePolicyTests {
    @Test func bootedSpecialModelIsNotMistakenForBootloader() {
        // 0x00010071 is the booted 1814; only 0x00010070 is the bootloader.
        #expect(BridgeCoKnownDevice.bootloader(vendorId: 0x00_0D6C, modelId: 0x0001_0071) == nil)
        #expect(BridgeCoKnownDevice.bootloader(vendorId: 0x00_0D6C,
                                               modelId: 0x0001_0070)?.bootedModelId == 0x0001_0071)
    }
}

struct BridgeCoFirmwareVerdictTests {

    private func section(softwareTimestamp: String?,
                         modelId: UInt32 = 0x0001_0071) -> BridgeCoSection {
        var s = BridgeCoSection()
        s.vendorId = 0x00_0D6C
        s.modelId = modelId
        if let stamp = softwareTimestamp {
            s.bootRom = ASFWMCPBeBoBBootRomInformation(
                protocolVersion: 1, bootloaderVersion: 0, guid: 0,
                hardwareModelId: modelId, hardwareRevision: 0,
                softwareTimestamp: stamp, softwareId: 0, softwareVersion: 0,
                imageBaseAddress: 0, imageMaximumSize: 0,
                bootloaderTimestamp: "20060101T000000Z", debugger: nil)
        }
        return s
    }

    /// Linux gates the boot cue on the BootROM software build date, refusing
    /// anything before 20070401 (firmware 5058). bebob_maudio.c:99-113.
    @Test func buildOnTheGateDateSupportsTheBootCue() {
        #expect(section(softwareTimestamp: "20070401T120000Z").bootCueSupported == true)
    }

    @Test func laterBuildSupportsTheBootCue() {
        #expect(section(softwareTimestamp: "20080915T083000Z").bootCueSupported == true)
    }

    @Test func earlierBuildDoesNotSupportTheBootCue() {
        #expect(section(softwareTimestamp: "20070331T235959Z").bootCueSupported == false)
        #expect(section(softwareTimestamp: "20051212T000000Z").bootCueSupported == false)
    }

    @Test func absentBootRomYieldsNoVerdict() {
        #expect(section(softwareTimestamp: nil).bootCueSupported == nil)
    }

    @Test func malformedTimestampYieldsNoVerdict() {
        #expect(section(softwareTimestamp: "notadate").bootCueSupported == nil)
        #expect(section(softwareTimestamp: "2007").bootCueSupported == nil)
    }
}

struct AVCSnapshotMembershipTests {

    /// Membership is behavioural, not identity-based: the report exists to
    /// describe devices ASFW has never seen.
    @Test func aFamilySectionAloneQualifiesTheDevice() {
        var snap = AVCDeviceSnapshot()
        #expect(!snap.isAVCDevice)
        snap.bridgeCo = BridgeCoSection()
        #expect(snap.isAVCDevice)
        #expect(snap.familyLabels == ["BridgeCo / BeBoB"])
    }

    @Test func genericPlugCountsAloneQualifyTheDevice() {
        var snap = AVCDeviceSnapshot()
        snap.inventory.unitPlugs = AVCUnitPlugCounts(isochronousInput: 1, isochronousOutput: 1,
                                                     externalInput: 0, externalOutput: 0)
        #expect(snap.isAVCDevice)
        // No vendor family recognised -- still a valid AV/C report.
        #expect(snap.familyLabels.isEmpty)
    }

    @Test func subunitsAloneQualifyTheDevice() {
        var snap = AVCDeviceSnapshot()
        snap.inventory.subunits = [AVCSubunitEntry(type: 0x0C, id: 0,
                                                   sourcePlugs: 1, destinationPlugs: 1)]
        #expect(snap.isAVCDevice)
        #expect(snap.inventory.subunits[0].typeName == "Music")
    }
}

struct BridgeCoReferenceGeometryTests {

    /// Values transcribed from linux bebob_maudio.c:227-254 ch_table.
    /// OUT (host->device / playback): SPDIF {6,6,4}, ADAT {12,8,4}
    /// IN  (device->host / capture):  SPDIF {10,10,2}, ADAT {16,12,2}
    /// with i/2 selecting the group: {44.1,48} {88.2,96} {176.4,192}.
    @Test func fireWire1814GeometryMatchesTheReferenceTable() throws {
        let geo = try #require(BridgeCoKnownDevice.referenceGeometry(vendorId: 0x00_0D6C,
                                                                     modelId: 0x0001_0071))
        let spdif = try #require(geo.modes.first { $0.mode.hasPrefix("S/PDIF") }).formations
        let adat = try #require(geo.modes.first { $0.mode == "ADAT" }).formations

        #expect(spdif.map(\.rateHz) == [44_100, 48_000, 88_200, 96_000, 176_400, 192_000])
        #expect(spdif.map(\.capturePcm) == [10, 10, 10, 10, 2, 2])
        #expect(spdif.map(\.playbackPcm) == [6, 6, 6, 6, 4, 4])

        #expect(adat.map(\.capturePcm) == [16, 16, 12, 12, 2, 2])
        #expect(adat.map(\.playbackPcm) == [12, 12, 8, 8, 4, 4])

        #expect(spdif.allSatisfy { $0.midiSlots == 1 })
        #expect(adat.allSatisfy { $0.midiSlots == 1 })
    }

    /// 32 kHz is absent from the special firmware's table (the loop writes
    /// formations[i+1], so index 0 is never filled).
    @Test func thirtyTwoKilohertzIsAbsent() throws {
        let geo = try #require(BridgeCoKnownDevice.referenceGeometry(vendorId: 0x00_0D6C,
                                                                     modelId: 0x0001_0071))
        for mode in geo.modes {
            #expect(!mode.formations.contains { $0.rateHz == 32_000 })
        }
    }

    /// Reference geometry only exists for devices that cannot be queried.
    @Test func referenceGeometryIsOnlyForUnprobableDevices() {
        for geo in BridgeCoKnownDevice.referenceGeometry {
            #expect(BridgeCoKnownDevice.specialFirmware.contains {
                $0.vendorId == geo.vendorId && $0.modelId == geo.modelId
            }, "reference geometry is only documented for special firmware")
        }
    }
}

struct AVCReportTextFormatterTests {

    private func snapshot(vendorId: UInt32, modelId: UInt32,
                          withBridgeCo: Bool = true) -> AVCDeviceSnapshot {
        var snap = AVCDeviceSnapshot()
        snap.guid = 0x0001_0203_0405_0607
        snap.vendorId = vendorId
        snap.modelId = modelId
        snap.vendorName = "M-Audio"
        snap.modelName = "FireWire 1814"
        let restricted = BridgeCoKnownDevice.specialFirmware.contains {
            $0.vendorId == vendorId && $0.modelId == modelId
        } || BridgeCoKnownDevice.bootloaders.contains {
            $0.vendorId == vendorId && $0.bootloaderModelId == modelId
        }
        snap.policy = restricted
            ? AVCProbePolicy(tier: .memoryOnly, rationale: "Quarantined by runtime catalog.")
            : AVCProbePolicy(tier: .avcStatus, rationale: "Runtime catalog permits STATUS probes.")
        if withBridgeCo {
            var section = BridgeCoSection()
            section.vendorId = vendorId
            section.modelId = modelId
            section.bootRom = ASFWMCPBeBoBBootRomInformation(
                protocolVersion: 1, bootloaderVersion: 0x0100_0000, guid: 0,
                hardwareModelId: modelId, hardwareRevision: 0,
                softwareTimestamp: "20080101T000000Z", softwareId: 0, softwareVersion: 0,
                imageBaseAddress: 0, imageMaximumSize: 0,
                bootloaderTimestamp: "20070101T000000Z", debugger: nil)
            snap.bridgeCo = section
        }
        return snap
    }

    @Test func genericBodyIsPresentForEveryDevice() {
        let text = AVCReportTextFormatter.format(
            snapshot: snapshot(vendorId: 0x00_0D6C, modelId: 0x0001_0071),
            appVersion: "test", driverVersion: nil)
        #expect(text.contains("ASFW AV/C DEVICE REPORT"))
        #expect(text.contains("AV/C UNIT INVENTORY"))
        #expect(text.contains("PROBE POLICY"))
    }

    @Test func restrictedDeviceReportStatesProbingWasSuppressed() {
        let text = AVCReportTextFormatter.format(
            snapshot: snapshot(vendorId: 0x00_0D6C, modelId: 0x0001_0071),
            appVersion: "test", driverVersion: nil)
        #expect(text.contains("memory-only (no AV/C)"))
        #expect(text.contains("not queried"))
        // Reference geometry must be labelled, never presented as queried.
        #expect(text.contains("NOT queried from the device"))
        #expect(text.contains("bebob_maudio.c:227-254"))
    }

    @Test func familySectionIsOmittedForNonBridgeCoDevices() {
        let text = AVCReportTextFormatter.format(
            snapshot: snapshot(vendorId: 0x00_03DB, modelId: 0x01_EEEE, withBridgeCo: false),
            appVersion: "test", driverVersion: nil)
        #expect(!text.contains("BRIDGECO"))
        #expect(text.contains("generic AV/C (no vendor family recognised)"))
        // The generic body still renders.
        #expect(text.contains("AV/C UNIT INVENTORY"))
    }

    @Test func bootloaderStateIsCalledOut() {
        let text = AVCReportTextFormatter.format(
            snapshot: snapshot(vendorId: 0x00_0D6C, modelId: 0x0001_0070),
            appVersion: "test", driverVersion: nil)
        #expect(text.contains("BOOTLOADER"))
        #expect(text.contains("0x010071"))
        #expect(text.contains("cannot stream audio in this state"))
    }

    @Test func firmwareVerdictIsPrinted() {
        let text = AVCReportTextFormatter.format(
            snapshot: snapshot(vendorId: 0x00_0D6C, modelId: 0x0001_0071),
            appVersion: "test", driverVersion: nil)
        #expect(text.contains("Boot cue:   SUPPORTED"))
    }
}
