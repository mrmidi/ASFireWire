//
//  AVCReportStore.swift
//  ASFW
//
//  Drives the AV/C Device Report tab.
//
//  Deliberately does NOT filter by vendor or model: the point of the report is
//  to describe devices ASFW does not support yet. A node qualifies when it
//  answers a generic AV/C probe or exposes a recognised family memory block,
//  which is the same test protocol bring-up relies on.
//

import Combine
import Foundation

@MainActor
final class AVCReportStore: ObservableObject {
    @Published var isRefreshing = false
    @Published var error: String?
    @Published var reportText: String =
        "No AV/C report yet. Connect a device and click Refresh."
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
        var skipped = 0

        for device in devices {
            guard !Task.isCancelled else {
                isRefreshing = false
                return
            }
            switch connector.captureAVCSnapshot(guid: device.guid) {
            case .captured(let snapshot):
                found += 1
                reports.append(AVCReportTextFormatter.format(
                    snapshot: snapshot,
                    appVersion: Self.reportGeneratorVersion(),
                    driverVersion: driverVersion
                ))
            case .notAVC:
                skipped += 1
            case .unstable(let guid):
                unstableGuids.append(guid)
            case .unavailable:
                continue
            }
        }

        let text: String
        if reports.isEmpty {
            text = """
            No AV/C device report found.

            Scanned \(devices.count) discovered node(s); \(skipped) answered but exposed
            neither an AV/C plug inventory nor a recognised vendor family block.

            A device qualifies when it answers AV/C PLUG_INFO, reports subunits, or
            carries a family memory block such as the BridgeCo BootROM at
            0xFFFFC8020000. DICE devices are register-based and will not appear here —
            use the DICE Report for those.

            If you expected an AV/C device, check that it is powered, that the driver
            shows it under Device Discovery, and that no other application owns it.
            """
        } else if reports.count == 1 {
            text = reports[0]
        } else {
            text = reports.enumerated()
                .map { "########## AV/C DEVICE \($0.offset + 1) of \(reports.count) ##########\n\n\($0.element)" }
                .joined(separator: "\n\n")
        }

        if !unstableGuids.isEmpty {
            error = "Bus reset or node-ID change interrupted \(unstableGuids.count) AV/C capture(s); retry once the bus is stable."
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
