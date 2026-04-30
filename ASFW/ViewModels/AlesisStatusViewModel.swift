import Foundation
import Combine
import AppKit

@MainActor
final class AlesisStatusViewModel: ObservableObject {
    @Published var coreAudioStatus: AlesisCoreAudioStatus?
    @Published var publishedDiceStatus: AlesisPublishedDiceStatus?
    @Published var discoveredIdentity: AlesisDiscoveredIdentity?
    @Published var diceSnapshot: AlesisDiceSnapshot?
    @Published var isRefreshing = false
    @Published var lastUpdated: Date?
    @Published var statusMessage = "Alesis state has not been checked yet."
    @Published var userClientAvailable = false

    private let connector: ASFWDriverConnector

    init(connector: ASFWDriverConnector) {
        self.connector = connector
    }

    func refresh() {
        isRefreshing = true
        userClientAvailable = connector.isConnected
        coreAudioStatus = Self.findCoreAudioStatus()
        publishedDiceStatus = Self.readPublishedDiceStatus()

        guard connector.isConnected else {
            discoveredIdentity = nil
            diceSnapshot = nil
            lastUpdated = Date()
            isRefreshing = false
            statusMessage = Self.message(coreAudio: coreAudioStatus,
                                         publishedDice: publishedDiceStatus,
                                         identity: nil,
                                         diceSnapshot: nil,
                                         userClientAvailable: false)
            return
        }

        let connector = self.connector
        DispatchQueue.global(qos: .userInitiated).async {
            let device = connector.getAlesisMultiMixDevice()
            let snapshot = device.flatMap { connector.refreshAlesisDiceSnapshot(device: $0) }
            let identity = device.map(AlesisDiscoveredIdentity.make(from:))

            Task { @MainActor in
                self.discoveredIdentity = identity
                self.diceSnapshot = snapshot
                self.lastUpdated = Date()
                self.isRefreshing = false
                self.statusMessage = Self.message(coreAudio: self.coreAudioStatus,
                                                  publishedDice: self.publishedDiceStatus,
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
            "Alesis MultiMix Firewire status",
            "CoreAudio: \(coreAudioStatus?.channelSummary ?? "not visible")",
            "Sample rate: \(coreAudioStatus?.sampleRateSummary ?? "unknown")",
            "ASFW lifecycle: \(lifecycle.summary)",
            "Debug user-client: \(userClientAvailable ? "connected" : "unavailable")"
        ]

        if let publishedDiceStatus {
            lines.append("Published DICE: \(publishedDiceStatus.protocolName), \(publishedDiceStatus.channelSummary), \(publishedDiceStatus.slotSummary), \(publishedDiceStatus.isoSummary)")
        } else {
            lines.append("Published DICE: unavailable")
        }

        if let discoveredIdentity {
            lines.append("Discovery: \(discoveredIdentity.displayName), GUID \(discoveredIdentity.guidHex)")
        } else {
            lines.append("Discovery: unavailable")
        }

        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(lines.joined(separator: "\n"), forType: .string)
        statusMessage = "Alesis status copied."
    }

    private static func findCoreAudioStatus() -> AlesisCoreAudioStatus? {
        AudioSystem.shared.devices
            .first { device in
                device.name.localizedCaseInsensitiveContains(AlesisConstants.coreAudioDeviceName)
                || device.name.localizedCaseInsensitiveContains("MultiMix")
            }
            .map(AlesisCoreAudioStatus.make(from:))
    }

    private static func readPublishedDiceStatus() -> AlesisPublishedDiceStatus? {
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
        return AlesisPublishedDiceStatus.parse(string)
    }

    private static func message(coreAudio: AlesisCoreAudioStatus?,
                                publishedDice: AlesisPublishedDiceStatus?,
                                identity: AlesisDiscoveredIdentity?,
                                diceSnapshot: AlesisDiceSnapshot?,
                                userClientAvailable: Bool) -> String {
        if coreAudio == nil {
            return "CoreAudio does not currently show Alesis."
        }
        if publishedDice != nil && !userClientAvailable {
            return "CoreAudio shows Alesis. Published DICE state is available without the debug user-client."
        }
        if !userClientAvailable {
            return "CoreAudio shows Alesis. This driver has not published DICE state, and the debug user-client is unavailable."
        }
        if identity == nil {
            return "CoreAudio shows Alesis. The debug user-client did not report an Alesis discovery record."
        }
        if diceSnapshot == nil {
            return publishedDice == nil
                ? "CoreAudio shows Alesis. DICE details were not available from read-only discovery."
                : "CoreAudio shows Alesis. Published DICE state is available; live DICE register details were not available."
        }
        return "Alesis audio and read-only DICE discovery are visible."
    }
}
