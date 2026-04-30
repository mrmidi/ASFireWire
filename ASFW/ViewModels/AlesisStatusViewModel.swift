import Foundation
import Combine

@MainActor
final class AlesisStatusViewModel: ObservableObject {
    @Published var coreAudioStatus: AlesisCoreAudioStatus?
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

        guard connector.isConnected else {
            discoveredIdentity = nil
            diceSnapshot = nil
            lastUpdated = Date()
            isRefreshing = false
            statusMessage = coreAudioStatus == nil
                ? "CoreAudio does not currently show Alesis."
                : "CoreAudio shows Alesis. Debug user-client is unavailable, so DICE details are hidden."
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
                                                  identity: identity,
                                                  diceSnapshot: snapshot)
            }
        }
    }

    func captureDiagnostics(using driverViewModel: DriverViewModel) {
        driverViewModel.captureDiagnostics()
    }

    private static func findCoreAudioStatus() -> AlesisCoreAudioStatus? {
        AudioSystem.shared.devices
            .first { device in
                device.name.localizedCaseInsensitiveContains(AlesisConstants.coreAudioDeviceName)
                || device.name.localizedCaseInsensitiveContains("MultiMix")
            }
            .map(AlesisCoreAudioStatus.make(from:))
    }

    private static func message(coreAudio: AlesisCoreAudioStatus?,
                                identity: AlesisDiscoveredIdentity?,
                                diceSnapshot: AlesisDiceSnapshot?) -> String {
        if coreAudio == nil {
            return "CoreAudio does not currently show Alesis."
        }
        if identity == nil {
            return "CoreAudio shows Alesis. The debug user-client did not report an Alesis discovery record."
        }
        if diceSnapshot == nil {
            return "CoreAudio shows Alesis. DICE details were not available from read-only discovery."
        }
        return "Alesis audio and read-only DICE discovery are visible."
    }
}
