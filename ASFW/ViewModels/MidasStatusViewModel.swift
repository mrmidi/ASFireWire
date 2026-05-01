import Foundation
import Combine
import AppKit

@MainActor
final class MidasStatusViewModel: ObservableObject {
    @Published var coreAudioStatus: MidasCoreAudioStatus?
    @Published var publishedDiceStatus: MidasPublishedDiceStatus?
    @Published var diceProbeDiagnostic: MidasDiceProbeDiagnostic?
    @Published var discoveredIdentity: MidasDiscoveredIdentity?
    @Published var diceSnapshot: AlesisDiceSnapshot?
    @Published var profile: FireWireAudioDeviceProfile?
    @Published var isRefreshing = false
    @Published var lastUpdated: Date?
    @Published var statusMessage = "Midas state has not been checked yet."
    @Published var userClientAvailable = false

    private let connector: ASFWDriverConnector

    init(connector: ASFWDriverConnector) {
        self.connector = connector
        self.profile = Self.defaultProfile()
    }

    func refresh() {
        isRefreshing = true
        userClientAvailable = connector.isConnected
        coreAudioStatus = Self.findCoreAudioStatus()
        publishedDiceStatus = Self.readPublishedDiceStatus()
        diceProbeDiagnostic = Self.readDiceProbeDiagnostic()
        profile = Self.defaultProfile()

        guard connector.isConnected else {
            discoveredIdentity = nil
            diceSnapshot = nil
            lastUpdated = Date()
            isRefreshing = false
            statusMessage = Self.message(coreAudio: coreAudioStatus,
                                         publishedDice: publishedDiceStatus,
                                         probeDiagnostic: diceProbeDiagnostic,
                                         identity: nil,
                                         diceSnapshot: nil,
                                         userClientAvailable: false)
            return
        }

        let connector = self.connector
        DispatchQueue.global(qos: .userInitiated).async {
            let device = connector.getMidasVeniceDevice()
            let profile = device.flatMap(FireWireDeviceProfiles.bestMatch(for:)) ?? Self.defaultProfile()
            let snapshot = device.flatMap { connector.refreshMidasDiceSnapshot(device: $0) }
            let identity = device.map { MidasDiscoveredIdentity.make(from: $0, profile: profile) }

            Task { @MainActor in
                self.profile = profile
                self.discoveredIdentity = identity
                self.diceSnapshot = snapshot
                self.lastUpdated = Date()
                self.isRefreshing = false
                self.statusMessage = Self.message(coreAudio: self.coreAudioStatus,
                                                  publishedDice: self.publishedDiceStatus,
                                                  probeDiagnostic: self.diceProbeDiagnostic,
                                                  identity: identity,
                                                  diceSnapshot: snapshot,
                                                  userClientAvailable: true)
            }
        }
    }

    func captureDiagnostics(using driverViewModel: DriverViewModel) {
        driverViewModel.captureDiagnostics()
    }

    func copyStatusSummary(lifecycle: MaintenanceLifecycleStatus) {
        var lines = [
            "Midas Venice status",
            "Profile: \(profile?.displayName ?? "not matched")",
            "CoreAudio: \(coreAudioStatus?.channelSummary ?? "not visible")",
            "Sample rate: \(coreAudioStatus?.sampleRateSummary ?? "unknown")",
            "ASFW lifecycle: \(lifecycle.summary)",
            "Debug user-client: \(userClientAvailable ? "connected" : "unavailable")"
        ]

        if let publishedDiceStatus {
            let published = publishedDiceStatus.inner
            lines.append("Published DICE: \(published.protocolName), \(published.channelSummary), \(published.slotSummary), \(published.isoSummary)")
        } else {
            lines.append("Published DICE: unavailable")
        }

        if let diceProbeDiagnostic {
            lines.append("Last DICE probe: \(diceProbeDiagnostic.probeState), \(diceProbeDiagnostic.humanFailReason)")
            lines.append("Probe caps: \(diceProbeDiagnostic.channelSummary), streams \(diceProbeDiagnostic.streamSummary), source \(diceProbeDiagnostic.capsSource)")
            lines.append("Probe IDs: \(diceProbeDiagnostic.vendorHex) / \(diceProbeDiagnostic.modelHex), GUID \(diceProbeDiagnostic.guidHex)")
        } else {
            lines.append("Last DICE probe: unavailable")
        }

        if let discoveredIdentity {
            lines.append("Discovery: \(discoveredIdentity.displayName), GUID \(discoveredIdentity.guidHex)")
        } else {
            lines.append("Discovery: unavailable")
        }

        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(lines.joined(separator: "\n"), forType: .string)
        statusMessage = "Midas status copied."
    }

    private static func defaultProfile() -> FireWireAudioDeviceProfile? {
        FireWireDeviceProfiles.lookup(vendorId: MidasConstants.vendorID,
                                      modelId: MidasConstants.veniceF32ModelID,
                                      unitSpecifierId: MidasConstants.veniceUnitSpecifierID,
                                      unitVersion: MidasConstants.veniceUnitVersion)
        ?? FireWireDeviceProfiles.lookup(vendorId: MidasConstants.vendorID,
                                         modelId: MidasConstants.veniceF32ModelID)
    }

    private static func findCoreAudioStatus() -> MidasCoreAudioStatus? {
        AudioSystem.shared.devices
            .first { device in
                device.name.localizedCaseInsensitiveContains("Midas")
                || device.name.localizedCaseInsensitiveContains("Venice")
            }
            .map(MidasCoreAudioStatus.make(from:))
    }

    private static func readPublishedDiceStatus() -> MidasPublishedDiceStatus? {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/sbin/ioreg")
        process.arguments = ["-p", "IOService", "-l", "-w0", "-r", "-c", "ASFWAudioNub"]

        let output = Pipe()
        process.standardOutput = output
        process.standardError = Pipe()

        do {
            try process.run()
        } catch {
            return nil
        }
        process.waitUntilExit()

        let data = output.fileHandleForReading.readDataToEndOfFile()
        guard let string = String(data: data, encoding: .utf8) else { return nil }
        return MidasPublishedDiceStatus.parse(string)
    }

    private static func readDiceProbeDiagnostic() -> MidasDiceProbeDiagnostic? {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/sbin/ioreg")
        process.arguments = ["-p", "IOService", "-l", "-w0", "-r", "-c", "ASFWDriver"]

        let output = Pipe()
        process.standardOutput = output
        process.standardError = Pipe()

        do {
            try process.run()
        } catch {
            return nil
        }
        process.waitUntilExit()

        let data = output.fileHandleForReading.readDataToEndOfFile()
        guard let string = String(data: data, encoding: .utf8) else { return nil }
        return MidasDiceProbeDiagnostic.parse(string)
    }

    private static func message(coreAudio: MidasCoreAudioStatus?,
                                publishedDice: MidasPublishedDiceStatus?,
                                probeDiagnostic: MidasDiceProbeDiagnostic?,
                                identity: MidasDiscoveredIdentity?,
                                diceSnapshot: AlesisDiceSnapshot?,
                                userClientAvailable: Bool) -> String {
        if coreAudio != nil && publishedDice != nil {
            return "CoreAudio and ASFW-published DICE state are visible."
        }
        if let probeDiagnostic {
            if probeDiagnostic.failReason == "unsupported_multi_stream_geometry" {
                return "Midas is discovered. ASFW found unsupported multi-stream DICE geometry, so CoreAudio was not published."
            }
            if probeDiagnostic.probeState == "failed" {
                return "Midas is discovered. ASFW refused CoreAudio publication: \(probeDiagnostic.humanFailReason)."
            }
            if probeDiagnostic.probeState == "refreshing" || probeDiagnostic.probeState == "refresh_pending" {
                return "Midas is discovered. ASFW is waiting for DICE stream caps to settle."
            }
        }
        if coreAudio != nil {
            return "CoreAudio shows Midas, but ASFW has not published DICE runtime state."
        }
        if !userClientAvailable {
            return "Midas is recognized in the device library. Live discovery needs the debug user-client."
        }
        if identity == nil {
            return "No Midas discovery record is currently visible from the debug user-client."
        }
        if diceSnapshot == nil {
            return "Midas identity is visible. Read-only DICE register details are not available yet."
        }
        return "Midas identity and read-only DICE discovery are visible."
    }
}
