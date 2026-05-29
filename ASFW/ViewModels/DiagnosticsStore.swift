//
//  DiagnosticsStore.swift
//  ASFW
//
//  Created by ASFireWire Project on 29.05.2026.
//

import Foundation
import Combine

final class DiagnosticsStore: ObservableObject {
    @Published var isRefreshing = false
    @Published var error: String?
    @Published var reportText: String = "No diagnostics report loaded yet. Click Refresh to query the driver."
    @Published var lastSnapshot: ASFWDiagnosticsSnapshot?
    @Published var isClearingTrace = false
    
    private let client: ASFWDiagnosticsClient
    private let connector: ASFWDriverConnector
    private var statusCancellable: AnyCancellable?
    
    init(connector: ASFWDriverConnector) {
        self.connector = connector
        self.client = ASFWDiagnosticsClient(connector: connector)
        
        // Refresh diagnostics when driver connects
        statusCancellable = connector.statusPublisher
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in
                self?.refresh()
            }
    }
    
    func refresh() {
        guard connector.isConnected else {
            self.error = "Not connected to ASFW driver. Check connection status."
            self.reportText = "ASFW driver is not connected. Connect the driver using the controls in the toolbar, then try refreshing diagnostics."
            return
        }
        
        guard !isRefreshing else { return }
        isRefreshing = true
        error = nil
        
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            
            do {
                print("[DiagStore] 🔍 Querying driver diagnostics selectors...")
                let snapshot = try self.client.fetchSnapshot()
                print("[DiagStore] ✅ Successfully retrieved diagnostic snapshot.")
                
                let text = DiagnosticsTextFormatter.format(snapshot: snapshot)
                
                DispatchQueue.main.async {
                    self.lastSnapshot = snapshot
                    self.reportText = text
                    self.isRefreshing = false
                }
            } catch {
                print("[DiagStore] ❌ Failed to fetch diagnostics: \(error)")
                let errorDescription = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
                
                DispatchQueue.main.async {
                    self.error = errorDescription
                    self.reportText = "ERROR: Failed to fetch diagnostics.\n\nDetails: \(errorDescription)"
                    self.isRefreshing = false
                }
            }
        }
    }
    
    func clearTrace() {
        guard connector.isConnected else { return }
        guard !isClearingTrace else { return }
        isClearingTrace = true
        
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            
            do {
                print("[DiagStore] 🧹 Clearing async transactions trace...")
                try self.client.clearAsyncTrace()
                print("[DiagStore] ✅ Cleared async transactions trace.")
                
                DispatchQueue.main.async {
                    self.isClearingTrace = false
                    // Re-fetch snapshot immediately to show the cleared trace state
                    self.refresh()
                }
            } catch {
                print("[DiagStore] ❌ Failed to clear async trace: \(error)")
                let errorDescription = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
                
                DispatchQueue.main.async {
                    self.error = "Failed to clear trace: \(errorDescription)"
                    self.isClearingTrace = false
                }
            }
        }
    }
    
    deinit {
        statusCancellable = nil
    }
}
