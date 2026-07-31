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

final class DiceReportStore: ObservableObject {
    @Published var isRefreshing = false
    @Published var error: String?
    @Published var reportText: String =
        "No DICE report yet. Connect a device and click Refresh."
    @Published var deviceCount: Int = 0

    private let connector: ASFWDriverConnector

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

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self else { return }

            let devices = self.connector.getDiscoveredDevices() ?? []
            // Dumping the struct verbatim produces an unreadable one-liner in a
            // report a human has to read.
            let versionText = self.connector.getDriverVersion().map { v in
                "\(v.semanticVersion) (\(v.gitCommitShort) on \(v.gitBranch)"
                    + (v.gitDirty ? ", dirty" : "") + ") built \(v.buildTimestamp)"
            }

            var reports = [String]()
            var found = 0

            for device in devices {
                let snapshot = self.connector.captureDiceSnapshot(device: device)
                // A device with no readable general section table is not DICE.
                guard snapshot.generalSections != nil else { continue }
                found += 1
                reports.append(DiceReportTextFormatter.format(snapshot: snapshot,
                                                              appVersion: versionText))
            }

            let text: String
            if reports.isEmpty {
                text = """
                No DICE device found.

                Scanned \(devices.count) discovered node(s); none answered a read of the
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

            DispatchQueue.main.async {
                self.reportText = text
                self.deviceCount = found
                self.isRefreshing = false
            }
        }
    }
}
