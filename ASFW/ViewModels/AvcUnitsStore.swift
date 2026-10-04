import Combine
import Foundation

/// The AV/C Units screen's data: the units the driver knows, each with its cached discovery document.
///
/// `reload()` reads only what the driver already holds; it sends nothing to a device. `reScan()` asks the driver to
/// run discovery again (the caller confirms that first) and reloads when it finishes.
@MainActor
final class AvcUnitsStore: ObservableObject {
    @Published private(set) var units: [AvcUnitDashboard] = []
    @Published var selectedGUID: UInt64?
    @Published private(set) var isLoading = false
    @Published private(set) var lastUpdated: Date?
    @Published private(set) var message: String?

    private let connector: any AvcReportSource

    init(connector: any AvcReportSource) { self.connector = connector }

    var isConnected: Bool { connector.isConnected }

    var selected: AvcUnitDashboard? {
        units.first { $0.guid == selectedGUID } ?? units.first
    }

    func reload() async {
        guard !isLoading else { return }
        guard connector.isConnected else {
            message = "Not connected to the ASFW driver."
            units = []
            return
        }
        isLoading = true
        defer { isLoading = false }
        guard let wireUnits = connector.getAVCUnits() else {
            message = "Could not read the AV/C units from the driver."
            return
        }
        let devices = connector.getDiscoveredDevices() ?? []
        var built: [AvcUnitDashboard] = []
        for unit in wireUnits {
            await Task.yield()
            let device = devices.first { $0.guid == unit.guid }
            let document = connector.getAVCDiscoveryDocument(guid: unit.guid).flatMap(AvcUnitDocument.decode)
            built.append(AvcUnitDashboard(
                guid: unit.guid, nodeID: unit.nodeID, vendorID: unit.vendorID, modelID: unit.modelID,
                vendorName: device?.vendorName, modelName: device?.modelName, deviceState: device?.stateString,
                wireUnit: unit, document: document))
        }
        units = built
        if selectedGUID == nil || !built.contains(where: { $0.guid == selectedGUID }) { selectedGUID = built.first?.guid }
        lastUpdated = Date()
        message = nil
    }

    /// Run discovery again on the driver, wait for it to finish (bounded), then reload.
    func reScan() async {
        guard connector.isConnected else { return }
        guard connector.reScanAVCUnits() else {
            message = "The driver did not start a re-scan."
            return
        }
        let deadline = ContinuousClock.now.advanced(by: .seconds(60))
        // Give the driver a moment to mark the units as running before polling their state.
        try? await Task.sleep(for: .milliseconds(300))
        while ContinuousClock.now < deadline, !Task.isCancelled {
            guard let current = connector.getAVCUnits() else { break }
            if !current.contains(where: { $0.diagnosticState == 1 }) { break }
            try? await Task.sleep(for: .milliseconds(250))
        }
        await reload()
    }
}
