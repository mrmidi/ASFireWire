import Foundation
import Combine

@MainActor
final class DeviceLibraryViewModel: ObservableObject {
    @Published var searchText = ""
    @Published var selectedFamily: FireWireProtocolFamily?
    @Published var selectedStatus: FireWireProfileSupportStatus?
    @Published var discoveredDevices: [ASFWDriverConnector.FWDeviceInfo] = []
    @Published var userClientAvailable = false
    @Published var lastUpdated: Date?

    private let connector: ASFWDriverConnector

    init(connector: ASFWDriverConnector) {
        self.connector = connector
    }

    var filteredProfiles: [FireWireAudioDeviceProfile] {
        FireWireDeviceProfiles.all.filter { profile in
            if let selectedFamily, profile.protocolFamily != selectedFamily { return false }
            if let selectedStatus, profile.supportStatus != selectedStatus { return false }
            guard !searchText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
                return true
            }
            let q = searchText.lowercased()
            return profile.displayName.lowercased().contains(q)
                || profile.protocolFamily.rawValue.lowercased().contains(q)
                || profile.supportStatus.rawValue.lowercased().contains(q)
                || profile.source.lowercased().contains(q)
                || profile.hexSummary.lowercased().contains(q)
        }
    }

    var matchedProfileIDs: Set<String> {
        Set(discoveredDevices.compactMap { FireWireDeviceProfiles.bestMatch(for: $0)?.id })
    }

    func refreshConnectedDevices() {
        userClientAvailable = connector.isConnected
        discoveredDevices = connector.getDiscoveredDevices() ?? []
        lastUpdated = Date()
    }
}

extension FireWireAudioDeviceProfile {
    var hexSummary: String {
        String(format: "vendor 0x%06X model 0x%06X unit 0x%06X/0x%06X",
               vendorId, modelId, unitSpecifierId, unitVersion)
    }

    var supportSummary: String {
        switch supportStatus {
        case .supportedBinding:
            return "Supported / bound by ASFW"
        case .deferred:
            return "Recognized, binding deferred"
        case .metadataOnly:
            return "Recognized metadata only"
        }
    }
}
