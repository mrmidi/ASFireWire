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
class DebugViewModel: ObservableObject {
    @Published var isConnected: Bool = false
    @Published var controllerStatus: ControllerStatus?
    @Published var busResetHistory: [BusResetPacketSnapshot] = []
    @Published var asyncStatusMessage: String?
    @Published var asyncErrorMessage: String?
    @Published var asyncInProgress: Bool = false
    @Published var sharedStatus: DriverStatus?
    @Published var topologyCache: TopologySnapshot?

    let connector = ASFWDriverConnector()  // Internal access for TopologyViewModel
    private var driverViewModel: DriverViewModel?
    private let statusFetchQueue = DispatchQueue(label: "net.mrmidi.ASFW.debug.fetch", qos: .userInitiated)
    private var cancellables = Set<AnyCancellable>()
    
    init() {
        connector.$isConnected
            .receive(on: DispatchQueue.main)
            .sink { [weak self] connected in
                self?.isConnected = connected
                if !connected {
                    self?.sharedStatus = nil
                    self?.driverViewModel?.driverVersion = nil
                } else {
                    self?.fetchDriverVersion()
                }
            }
            .store(in: &cancellables)
        
        connector.statusPublisher
            .receive(on: DispatchQueue.main)
            .sink { [weak self] status in
                self?.handleStatusUpdate(status)
            }
            .store(in: &cancellables)
        
        observeConnectorLogs()
    }
    
    func setDriverViewModel(_ viewModel: DriverViewModel) {
        self.driverViewModel = viewModel
    }
    
    private func observeConnectorLogs() {
        connector.$logMessages
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
        connector.connect(forceAttempt: false)
    }
    
    func disconnect() {
        connector.disconnect()
        sharedStatus = nil
    }
    
    func manualRefresh() {
        fetchLatestSnapshots()
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
        statusFetchQueue.async { [weak self] in
            guard let self = self else { return }
            let status = self.connector.getControllerStatus()
            let history = self.connector.getBusResetHistory(startIndex: 0, count: 10) ?? []
            let topology = self.connector.getTopologySnapshot()
            Task { @MainActor in
                self.controllerStatus = status
                self.busResetHistory = history
                self.topologyCache = topology
            }
        }
    }

    func fetchTopology() {
        statusFetchQueue.async { [weak self] in
            guard let self = self else { return }
            let topology = self.connector.getTopologySnapshot()
            Task { @MainActor in
                self.topologyCache = topology
            }
        }
    }

    private func fetchDriverVersion() {
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            // Retry a few times if needed, as connection might be fresh
            var version: DriverVersionInfo? = nil
            for _ in 0..<3 {
                version = self.connector.getDriverVersion()
                if version != nil { break }
                Thread.sleep(forTimeInterval: 0.1)
            }
            
            if let v = version {
                Task { @MainActor in
                    self.driverViewModel?.driverVersion = v
                }
            }
        }
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

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            let handle = self.connector.asyncRead(destinationID: destinationID,
                                                  addressHigh: addressHigh,
                                                  addressLow: addressLow,
                                                  length: length)
            DispatchQueue.main.async {
                self.asyncInProgress = false
                if let handle = handle {
                    let message = String(format: "Async read handle 0x%04X (len=%u)", handle, length)
                    self.asyncStatusMessage = message
                    self.asyncErrorMessage = nil
                    self.driverViewModel?.log(message, source: .userClient, level: .info)
                } else {
                    let error = self.connector.lastError ?? "Async read failed"
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

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            let handle = self.connector.asyncWrite(destinationID: destinationID,
                                                   addressHigh: addressHigh,
                                                   addressLow: addressLow,
                                                   payload: payload)
            DispatchQueue.main.async {
                self.asyncInProgress = false
                if let handle = handle {
                    let message = String(format: "Async write handle 0x%04X (bytes=%u)", handle, payload.count)
                    self.asyncStatusMessage = message
                    self.asyncErrorMessage = nil
                    self.driverViewModel?.log(message, source: .userClient, level: .info)
                } else {
                    let error = self.connector.lastError ?? "Async write failed"
                    self.asyncErrorMessage = error
                    self.asyncStatusMessage = nil
                    self.driverViewModel?.log(error, source: .userClient, level: .error)
                }
            }
        }
    }

    nonisolated deinit {
        Task { @MainActor [weak self] in
            guard let self else { return }
            self.connector.disconnect()
        }
    }
}
