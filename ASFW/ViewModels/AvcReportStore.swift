import Combine
import Foundation

@MainActor
final class AvcReportStore: ObservableObject {
    @Published private(set) var isRefreshing = false
    @Published var error: String?
    @Published private(set) var reportText = "No AV/C report yet. Connect a device and click Refresh, or open a saved JSON dump."
    @Published private(set) var snapshot: AvcReportSnapshot?
    @Published private(set) var isImported = false
    private let connector: any AvcReportSource

    var deviceCount: Int { snapshot?.devices.count ?? 0 }

    init(connector: any AvcReportSource) { self.connector = connector }

    /// Run normal discovery manually, then export its terminal result.
    func refresh() async {
        guard !isRefreshing else { return }
        guard connector.isConnected else { error = "Not connected to the ASFW driver."; return }
        isRefreshing = true
        error = nil
        defer { isRefreshing = false }
        guard let devices = connector.getDiscoveredDevices(), let initialUnits = connector.getAVCUnits() else {
            error = "Could not read device or AV/C discovery state. The previous report was retained."
            return
        }
        guard initialUnits.allSatisfy({ $0.diagnosticState != nil }) else {
            error = "The installed driver does not expose diagnostic completion. Update the driver to capture a live report."
            return
        }
        guard connector.reScanAVCUnits() else {
            error = "Could not start AV/C diagnostics. The previous report was retained."
            return
        }
        var units = initialUnits
        let deadline = ContinuousClock.now.advanced(by: .seconds(120))
        var timedOut = false
        repeat {
            guard !Task.isCancelled else { return }
            guard connector.isConnected, let current = connector.getAVCUnits() else {
                error = "Driver disconnected during diagnostics. The previous report was retained."
                return
            }
            units = current
            if !units.contains(where: { $0.diagnosticState == 1 }) { break }
            if ContinuousClock.now >= deadline { timedOut = true; break }
            do { try await Task.sleep(for: .milliseconds(200)) } catch { return }
        } while true
        let version = connector.getDriverVersion().map {
            "\($0.semanticVersion) (\($0.gitCommitShort) on \($0.gitBranch)\($0.gitDirty ? ", dirty" : "")) built \($0.buildTimestamp)"
        }
        let candidates = devices.filter { device in
            device.units.contains { $0.specId == 0x00A02D && $0.swVersion == 0x010001 } ||
            units.contains { $0.guid == device.guid }
        }
        var captured: [AvcReportSnapshot.Device] = []
        for device in candidates {
            await Task.yield()
            guard !Task.isCancelled else { return }
            var notes: [String] = []
            let unit = units.first { $0.guid == device.guid && $0.nodeID == UInt16(device.nodeId) }
            if unit == nil {
                notes.append("No AV/C inventory for the current node. Discovery may be incomplete or restricted; no unit was available for manual diagnostics.")
            }
            if let state = unit?.diagnosticState {
                switch state {
                case 2: notes.append("Manual discovery completed.")
                case 3: notes.append("Manual discovery failed; available partial data follows.")
                case 4: notes.append("Refresh sent nothing: this device's probe policy forbids discovery commands. The exchange log is what the driver has sent it since attach.")
                case 1: notes.append("Manual discovery timed out; capture is incomplete.")
                default: notes.append("No terminal manual discovery result was recorded.")
                }
            }
            if timedOut { notes.append("Capture deadline reached while diagnostics were still running.") }
            let rom = connector.getConfigROM(nodeId: device.nodeId, generation: UInt16(truncatingIfNeeded: device.generation))
            if let rom, !rom.isExactGenerationMatch {
                notes.append("Config ROM cache belongs to a different generation; bytes omitted.")
            }
            // Plug counts and subunits are only real once a discovery ran;
            // otherwise they are zeros the device never reported.
            let discoveryRan = [UInt8(2), 3].contains(unit?.diagnosticState ?? 0)
            let exportUnit = discoveryRan ? unit : nil
            if unit != nil && !discoveryRan && unit?.diagnosticState != 1 {
                notes.append("Unit plugs and subunits were not read: this device's probe policy sends it no discovery commands.")
            }
            let exchanges = unit == nil ? nil : connector.getFCPExchangeLog(guid: device.guid)
            if unit != nil && exchanges == nil {
                notes.append("FCP exchange log unavailable; the installed driver may predate it.")
            } else if let exchanges, exchanges.dropped > 0 {
                notes.append("The exchange log was full: \(exchanges.dropped) later exchanges were not kept.")
            }
            let subunits: [AvcReportSnapshot.Subunit] = exportUnit?.subunits.map { subunit in
                let capabilities = subunit.type == 0x0C
                    ? connector.getSubunitCapabilitiesData(guid: device.guid, type: subunit.type, id: subunit.subunitID) : nil
                let summary = capabilities.flatMap { AVCMusicCapabilities(data: $0) }
                    .map { AvcReportTextFormatter.capabilitiesSummary($0) }
                let descriptor = [UInt8(0x01), 0x0C].contains(subunit.type)
                    ? connector.getSubunitDescriptor(guid: device.guid, type: subunit.type, id: subunit.subunitID) : nil
                return AvcReportSnapshot.Subunit(type: subunit.type, id: subunit.subunitID,
                    sourcePlugs: subunit.numSrcPlugs, destinationPlugs: subunit.numDestPlugs,
                    capabilitySummary: summary, capabilities: capabilities, descriptor: descriptor)
            } ?? []
            captured.append(.init(guid: device.guid, nodeID: device.nodeId, generation: device.generation,
                vendorID: device.vendorId, modelID: device.modelId, vendorName: device.vendorName,
                modelName: device.modelName, state: device.stateString,
                romUnits: device.units.map { .init(offset: $0.romOffset, specifierID: $0.specId, version: $0.swVersion) },
                configROM: rom?.isExactGenerationMatch == true ? rom?.data : nil,
                avcUnit: exportUnit.map { .init(isoInputPlugs: $0.isoInputPlugs, isoOutputPlugs: $0.isoOutputPlugs,
                    externalInputPlugs: $0.extInputPlugs, externalOutputPlugs: $0.extOutputPlugs, subunits: subunits) }, notes: notes,
                exchanges: exchanges))
        }
        guard !Task.isCancelled else { return }
        guard connector.isConnected, let after = connector.getDiscoveredDevices(),
              candidates.allSatisfy({ before in
                  after.contains { $0.guid == before.guid && $0.nodeId == before.nodeId && $0.generation == before.generation }
              }) else {
            error = "Device routing changed during capture. The previous report was retained; refresh once discovery is stable."
            return
        }
        let bundle = Bundle.main
        let appVersion = "\(bundle.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "unknown") (build \(bundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "unknown"))"
        setSnapshot(.init(appVersion: appVersion, driverVersion: version, devices: captured), imported: false)
    }

    func openDump(_ url: URL) {
        guard !isRefreshing else { return }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        do {
            let size = try url.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0
            guard size <= 8 * 1024 * 1024 else { throw AvcReportSnapshot.ImportError.invalidSize }
            let report = try AvcReportSnapshot.load(Data(contentsOf: url))
            setSnapshot(report, imported: true)
            error = nil
        } catch { self.error = "Could not open AV/C dump: \(error.localizedDescription)" }
    }

    private func setSnapshot(_ report: AvcReportSnapshot, imported: Bool) {
        snapshot = report
        reportText = AvcReportTextFormatter.format(report)
        isImported = imported
    }
}
