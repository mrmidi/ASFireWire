//
//  DebugViewModel.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import Foundation
import Combine
import SwiftUI

@MainActor
@Observable
final class DebugViewModel {
    var isConnected: Bool = false
    var controllerStatus: ControllerStatus?
    var busResetHistory: [BusResetPacketSnapshot] = []
    var asyncStatusMessage: String?
    var asyncErrorMessage: String?
    var asyncInProgress: Bool = false
    var sharedStatus: DriverStatus?
    var topologyCache: TopologySnapshot?
    var avcUnits: [ASFWDriverConnector.AVCUnitInfo] = []
    var romExplorerVM: RomExplorerViewModel
    var diagnosticsStore: DiagnosticsStore
    var diceReportStore: DiceReportStore
    var mcpVM: ASFWMCPControlViewModel

    @ObservationIgnored var connectorObservable = ASFWDriverConnector.Observable()  // Internal access for TopologyViewModel
    @ObservationIgnored private var driverViewModel: DriverViewModel?
    @ObservationIgnored private let statusFetchQueue = DispatchQueue(label: "net.mrmidi.ASFW.debug.fetch", qos: .userInitiated)
    @ObservationIgnored private var cancellables = Set<AnyCancellable>()
    
    init() {
        romExplorerVM = RomExplorerViewModel(connectorObservable: connectorObservable)
        diagnosticsStore = DiagnosticsStore(connectorObservable: connectorObservable)
        diceReportStore = DiceReportStore(connectorObservable: connectorObservable)
        mcpVM = ASFWMCPControlViewModel(connectorObservable: connectorObservable)
        connectorObservable.$isConnected
            .receive(on: DispatchQueue.main)
            .sink { [weak self] connected in
                self?.isConnected = connected
                self?.driverViewModel?.updateDriverConnection(connected)
                if !connected {
                    self?.sharedStatus = nil
                } else {
                    self?.fetchDriverVersion()
                }
            }
            .store(in: &cancellables)
        
        connectorObservable.$latestStatus
            .receive(on: DispatchQueue.main)
            .sink { [weak self] status in
                guard let self, let status else { return }
                self.handleStatusUpdate(status)
            }
            .store(in: &cancellables)
        
        observeConnectorLogs()
    }
    
    func setDriverViewModel(_ viewModel: DriverViewModel) {
        self.driverViewModel = viewModel
        viewModel.updateDriverConnection(isConnected)
        if isConnected {
            fetchDriverVersion()
        }
    }
    
    private func observeConnectorLogs() {
        connectorObservable.$logMessages
            .sink { [weak self] messages in
                guard let self = self, let driverVM = self.driverViewModel else { return }
                
                // Forward only new messages
                for message in messages {
                    let level: DriverViewModel.LogEntry.Level
                    switch message.level {
                    case .info: level = .info
                    case .warning: level = .warning
                    case .error: level = .error
                    case .success: level = .success
                    }
                    
                    driverVM.log(message.message, source: .userClient, level: level)
                }
            }
            .store(in: &cancellables)
    }
    
    func connect() {
        _ = connectorObservable.connector.connect(forceAttempt: false)
    }
    
    func disconnect() {
        connectorObservable.connector.disconnect()
        sharedStatus = nil
    }
    
    func manualRefresh() {
        fetchLatestSnapshots()
        fetchAVCUnits()
    }

    private func handleStatusUpdate(_ status: DriverStatus) {
        sharedStatus = status
        fetchLatestSnapshots()
    }

    func dumpDebugSnapshot() {
        print("[DebugVM] sharedStatus=\(String(describing: sharedStatus))")
        print("[DebugVM] controllerStatus=\(String(describing: controllerStatus?.stateName))")
        print("[DebugVM] connected=\(isConnected)")
    }

    private func fetchLatestSnapshots() {
        let connector = connectorObservable.connector
        Task.detached { [weak self, weak connector] in
            guard let self, let connector else { return }
            let status = await connector.getControllerStatus()
            let history = await connector.getBusResetHistory(startIndex: 0, count: 10) ?? []
            let topology = await connector.getTopologySnapshot()
            Task { @MainActor in
                self.controllerStatus = status
                self.busResetHistory = history
                self.topologyCache = topology
            }
        }
    }

    func fetchTopology() async {
        let topology = await connectorObservable.connector.getTopologySnapshot()
        topologyCache = topology
    }

    private func fetchDriverVersion() {
        let connector = connectorObservable.connector
        Task.detached { [weak self, weak connector] in
            guard let self, let connector else { return }
            // Retry a few times if needed, as connection might be fresh
            var version: DriverVersionInfo? = nil
            for _ in 0..<3 {
                version = await connector.getDriverVersion()
                if version != nil { break }
                try? await Task.sleep(until: .now.advanced(by: .milliseconds(100)))
            }
            
            if let v = version {
                Task { @MainActor in
                    self.driverViewModel?.driverVersion = v
                }
            }
        }
    }

    func fetchAVCUnits() {
        let connector = connectorObservable.connector
        Task.detached { [weak self, weak connector] in
            guard let self, let connector else { return }
            let units = await connector.getAVCUnits() ?? []
            Task { @MainActor in
                self.avcUnits = units
            }
        }
    }
    
    func getSubunitCapabilities(guid: UInt64, type: UInt8, id: UInt8) async -> ASFWDriverConnector.AVCMusicCapabilities? {
        return await self.connectorObservable.connector.getSubunitCapabilities(guid: guid, type: type, id: id)
    }


    func performAsyncRead(destinationID: UInt16,
                          addressHigh: UInt16,
                          addressLow: UInt32,
                          length: UInt32) {
        guard isConnected else {
            asyncErrorMessage = "Driver connection is not available."
            asyncStatusMessage = nil
            return
        }

        asyncInProgress = true
        asyncErrorMessage = nil
        asyncStatusMessage = "Issuing async read…"

        let connector = connectorObservable.connector
        Task.detached(priority: .userInitiated) { [weak self, weak connector] in
            guard let self, let connector else { return }
            let handle = await connector.asyncRead(destinationID: destinationID,
                                                  addressHigh: addressHigh,
                                                  addressLow: addressLow,
                                                  length: length)
            Task { @MainActor in
                self.asyncInProgress = false
                if let handle = handle {
                    let message = String(format: "Async read handle 0x%04X (len=%u)", handle, length)
                    self.asyncStatusMessage = message
                    self.asyncErrorMessage = nil
                    self.driverViewModel?.log(message, source: .userClient, level: .info)
                } else {
                    let error = self.connectorObservable.lastError ?? "Async read failed"
                    self.asyncErrorMessage = error
                    self.asyncStatusMessage = nil
                    self.driverViewModel?.log(error, source: .userClient, level: .error)
                }
            }
        }
    }

    func performAsyncWrite(destinationID: UInt16,
                           addressHigh: UInt16,
                           addressLow: UInt32,
                           payload: Data) {
        guard isConnected else {
            asyncErrorMessage = "Driver connection is not available."
            asyncStatusMessage = nil
            return
        }

        asyncInProgress = true
        asyncErrorMessage = nil
        asyncStatusMessage = "Issuing async write…"

        let connector = connectorObservable.connector
        Task.detached(priority: .userInitiated) { [weak self, weak connector] in
            guard let self, let connector else { return }
            let handle = await connector.asyncWrite(destinationID: destinationID,
                                                   addressHigh: addressHigh,
                                                   addressLow: addressLow,
                                                   payload: payload)
            Task { @MainActor in
                self.asyncInProgress = false
                if let handle = handle {
                    let message = String(format: "Async write handle 0x%04X (bytes=%u)", handle, payload.count)
                    self.asyncStatusMessage = message
                    self.asyncErrorMessage = nil
                    self.driverViewModel?.log(message, source: .userClient, level: .info)
                } else {
                    let error = self.connectorObservable.lastError ?? "Async write failed"
                    self.asyncErrorMessage = error
                    self.asyncStatusMessage = nil
                    self.driverViewModel?.log(error, source: .userClient, level: .error)
                }
            }
        }
    }

    func performAsyncBlockRead(destinationID: UInt16,
                               addressHigh: UInt16,
                               addressLow: UInt32,
                               length: UInt32) {
        guard isConnected else {
            asyncErrorMessage = "Driver connection is not available."
            asyncStatusMessage = nil
            return
        }

        asyncInProgress = true
        asyncErrorMessage = nil
        asyncStatusMessage = "Issuing async block read..."

        let connector = connectorObservable.connector
        Task.detached(priority: .userInitiated) { [weak self, weak connector] in
            guard let self, let connector else { return }
            let handle = await connector.asyncBlockRead(destinationID: destinationID,
                                                       addressHigh: addressHigh,
                                                       addressLow: addressLow,
                                                       length: length)
            Task { @MainActor in
                self.asyncInProgress = false
                if let handle = handle {
                    let message = String(format: "Async block read handle 0x%04X (len=%u)", handle, length)
                    self.asyncStatusMessage = message
                    self.asyncErrorMessage = nil
                    self.driverViewModel?.log(message, source: .userClient, level: .info)
                } else {
                    let error = self.connectorObservable.lastError ?? "Async block read failed"
                    self.asyncErrorMessage = error
                    self.asyncStatusMessage = nil
                    self.driverViewModel?.log(error, source: .userClient, level: .error)
                }
            }
        }
    }

    func performAsyncBlockWrite(destinationID: UInt16,
                                addressHigh: UInt16,
                                addressLow: UInt32,
                                payload: Data) {
        guard isConnected else {
            asyncErrorMessage = "Driver connection is not available."
            asyncStatusMessage = nil
            return
        }

        asyncInProgress = true
        asyncErrorMessage = nil
        asyncStatusMessage = "Issuing async block write..."

        let connector = connectorObservable.connector
        Task.detached(priority: .userInitiated) { [weak self, weak connector] in
            guard let self = self, let connector else { return }
            let handle = await connector.asyncBlockWrite(destinationID: destinationID,
                                                        addressHigh: addressHigh,
                                                        addressLow: addressLow,
                                                        payload: payload)
            Task { @MainActor in
                self.asyncInProgress = false
                if let handle = handle {
                    let message = String(format: "Async block write handle 0x%04X (bytes=%u)", handle, payload.count)
                    self.asyncStatusMessage = message
                    self.asyncErrorMessage = nil
                    self.driverViewModel?.log(message, source: .userClient, level: .info)
                } else {
                    let error = self.connectorObservable.lastError ?? "Async block write failed"
                    self.asyncErrorMessage = error
                    self.asyncStatusMessage = nil
                    self.driverViewModel?.log(error, source: .userClient, level: .error)
                }
            }
        }
    }

    func fetchTransactionResult(handle: UInt16) async -> ASFWDriverConnector.AsyncTransactionResult? {
        let connector = connectorObservable.connector
        return await withCheckedContinuation { continuation in
            Task.detached(priority: .userInitiated) { [weak connector] in
                guard let connector else {
                    Task { @MainActor in
                        continuation.resume(returning: nil)
                    }
                    return
                }
                let result = await connector.getTransactionResult(handle: handle)
                Task { @MainActor in
                    continuation.resume(returning: result)
                }
            }
        }
    }

    func decodeResponseCode(_ code: UInt8) -> String {
        switch code {
        case 0: return "Complete"
        case 1: return "Conflict"
        case 2: return "Data error"
        case 3: return "Type error"
        case 4: return "Address error"
        case 7: return "Rejected"
        default: return String(format: "Unknown (0x%X)", code)
        }
    }

    isolated
    deinit {
        connectorObservable.connector.disconnect()
    }
}
