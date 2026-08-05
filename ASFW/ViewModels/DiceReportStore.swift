//
//  DiceReportStore.swift
//  ASFW
//
//  Drives the DICE Report tab.
//
//  Deliberately does NOT filter by vendor or model: the point of the report is
//  to describe devices ASFW does not support yet. A node qualifies as DICE if
//  its general section table reads back sanely, which is the same test the
//  protocol itself relies on.
//

import Combine
import Foundation

@MainActor
final class DiceReportStore: ObservableObject {
    @Published var isRefreshing = false
    @Published var error: String?
    @Published var reportText: String =
        "No DICE report yet. Connect a device and click Refresh."
    @Published var deviceCount: Int = 0

    private let connector: ASFWDriverConnector
    private var refreshTask: Task<Void, Never>?

    init(connector: ASFWDriverConnector) {
        self.connector = connector
    }

    func refresh() {
        guard connector.isConnected else {
            error = "Not connected to the ASFW driver."
            reportText = "ASFW driver is not connected. Connect it from the toolbar, then refresh."
            return
        }
        guard !isRefreshing else { return }
        isRefreshing = true
        error = nil

        // The connector owns observable connection state and is main-actor
        // isolated by the target's default isolation. Keep every user-client
        // call on that boundary; passing it to a global GCD queue raced
        // disconnect/error publication. The task yields once so SwiftUI can
        // render the refreshing state before the synchronous legacy reads run.
        refreshTask?.cancel()
        refreshTask = Task { @MainActor [weak self] in
            guard let self, !Task.isCancelled else { return }
            self.performRefresh()
        }
    }

    deinit {
        refreshTask?.cancel()
    }

    private func performRefresh() {
        let devices = connector.getDiscoveredDevices() ?? []
        let driverVersion = connector.getDriverVersion().map { v in
            "\(v.semanticVersion) (\(v.gitCommitShort) on \(v.gitBranch)"
                + (v.gitDirty ? ", dirty" : "") + ") built \(v.buildTimestamp)"
        }

        var reports = [String]()
        var found = 0
        var unstableGuids = [UInt64]()

        for device in devices {
            guard !Task.isCancelled else {
                isRefreshing = false
                return
            }
            switch connector.captureDiceSnapshot(guid: device.guid) {
            case .captured(let snapshot):
                // A device with no readable general section table is not DICE.
                guard snapshot.generalSections != nil else { continue }
                found += 1
                reports.append(DiceReportTextFormatter.format(
                    snapshot: snapshot,
                    appVersion: Self.reportGeneratorVersion(),
                    driverVersion: driverVersion
                ))
            case .unstable(let guid):
                unstableGuids.append(guid)
            case .unavailable:
                continue
            }
        }

        let text: String
        if reports.isEmpty {
            text = """
            No stable DICE device report found.

            Scanned \(devices.count) discovered node(s); none completed a stable read of the
            DICE section table at 0xFFFFE0000000.

            If you expected a DICE device here, check that it is powered, that the
            driver shows it under Device Discovery, and that no other application
            currently owns it.
            """
        } else if reports.count == 1 {
            text = reports[0]
        } else {
            text = reports.enumerated()
                .map { "########## DICE DEVICE \($0.offset + 1) of \(reports.count) ##########\n\n\($0.element)" }
                .joined(separator: "\n\n")
        }

        if !unstableGuids.isEmpty {
            error = "Bus reset or node-ID change interrupted \(unstableGuids.count) DICE capture(s); retry once the bus is stable."
        }
        reportText = text
        deviceCount = found
        isRefreshing = false
        refreshTask = nil
    }

    private static func reportGeneratorVersion() -> String {
        let bundle = Bundle.main
        let marketing = bundle.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "unknown"
        let build = bundle.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "unknown"
        return "\(marketing) (build \(build))"
    }
}
