//
//  SaffireMixerViewModel.swift
//  ASFW
//
//  Created by ASFireWire Project on 2026-02-08.
//

import Foundation
import Combine

@MainActor
@Observable
class SaffireMixerViewModel {
    
    // MARK: - Published Properties
    
    var outputState: OutputGroupState = OutputGroupState()
    var inputParams: InputParams = InputParams()
    var isLoading: Bool = false
    var errorMessage: String?
    var lastUpdateTime: Date?

    // Resolved device target (runtime, from discovery).
    var deviceGUID: UInt64?
    var deviceNodeId: UInt16?
    
    // MARK: - Private Properties
    
    private let connectorObservable: ASFWDriverConnector.Observable
    @ObservationIgnored private var cancellables = Set<AnyCancellable>()
    @ObservationIgnored private var refreshTimer: Timer?

    private static let focusriteVendorId: UInt32 = 0x00130e
    private static let saffireModelIds: [UInt32] = [
        0x000005, // Pro 40
        0x000006, // Liquid S56
        0x000007, // Pro 24
        0x000008, // Pro 24 DSP
        0x000009, // Pro 14
        0x000012, // Pro26
        0x0000de, // Pro 40 TCD 3070
    ]

    // MARK: - Initialization
    
    init(connectorObservable: ASFWDriverConnector.Observable) {
        self.connectorObservable = connectorObservable
        setupObservers()
    }
    
    isolated
    deinit {
        stopAutoRefresh()
    }
    
    // MARK: - Setup
    
    private func setupObservers() {
        // Observe connection state changes
        connectorObservable.$isConnected
            .receive(on: DispatchQueue.main)
            .sink { [weak self] connected in
                guard let self else { return }
                if connected {
                    self.resolveTargetIfNeeded()
                    self.refresh()
                } else {
                    self.deviceGUID = nil
                    self.deviceNodeId = nil
                }
            }
            .store(in: &cancellables)
    }

    private func resolveTargetIfNeeded() {
        if deviceGUID != nil && deviceNodeId != nil {
            return
        }
        let connector = connectorObservable.connector
        Task.detached(priority: .userInitiated) { [weak connector, weak self] in
            guard let self, let connector else { return }
            guard let devices = await connector.getDiscoveredDevices() else { return }
            
            Task { @MainActor in
                // If we already have a GUID but nodeId is missing, refresh just nodeId.
                if let guid = self.deviceGUID,
                   let device = devices.first(where: { $0.guid == guid }) {
                    self.deviceNodeId = UInt16(device.nodeId)
                    return
                }
                
                // Otherwise, attempt to find the Saffire Pro 24 DSP.
                if let device = devices.first(where: { $0.vendorId == Self.focusriteVendorId && Self.saffireModelIds.contains($0.modelId) }) {
                    self.deviceGUID = device.guid
                    self.deviceNodeId = UInt16(device.nodeId)
                }
            }
        }
    }
    
    // MARK: - Public Methods
    
    /// Refresh mixer state from device
    func refresh() {
        guard !isLoading else { return }

        resolveTargetIfNeeded()
        guard let nodeId = deviceNodeId else {
            errorMessage = "No Saffire Pro 24 DSP found"
            return
        }
        
        isLoading = true
        errorMessage = nil
        let connector = connectorObservable.connector
        Task.detached(priority: .userInitiated) { [weak self, weak connector] in
            guard let self, let connector else { return }
            
            // Read output group state
            if let outputState = await connector.getSaffireOutputGroup(destinationID: nodeId) {
                Task { @MainActor in
                    self.outputState = outputState
                }
            } else {
                Task { @MainActor in
                    self.errorMessage = "Failed to read output state"
                }
            }
            
            // Read input parameters
            if let inputParams = await connector.getSaffireInputParams(destinationID: nodeId) {
                Task { @MainActor in
                    self.inputParams = inputParams
                }
            } else {
                Task { @MainActor in
                    self.errorMessage = "Failed to read input parameters"
                }
            }
            
            Task { @MainActor in
                self.isLoading = false
                self.lastUpdateTime = Date()
            }
        }
    }
    
    /// Update output group state on device
    func updateOutputState(_ newState: OutputGroupState) {
        resolveTargetIfNeeded()
        guard let nodeId = deviceNodeId else {
            errorMessage = "No Saffire Device found"
            return
        }

        errorMessage = nil
        
        let connector = connectorObservable.connector
        Task.detached(priority: .userInitiated) { [weak self, weak connector] in
            guard let self, let connector else { return }
            
            if await connector.setSaffireOutputGroup(destinationID: nodeId, newState) {
                Task { @MainActor in
                    self.outputState = newState
                    self.lastUpdateTime = Date()
                }
            } else {
                Task { @MainActor in
                    self.errorMessage = "Failed to update output state"
                }
            }
        }
    }
    
    /// Update input parameters on device
    func updateInputParams(_ newParams: InputParams) {
        resolveTargetIfNeeded()
        guard let nodeId = deviceNodeId else {
            errorMessage = "No Saffire Device found"
            return
        }

        errorMessage = nil
        
        let connector = connectorObservable.connector
        Task.detached(priority: .userInitiated) { [weak self, weak connector] in
            guard let self, let connector else { return }
            
            if await connector.setSaffireInputParams(destinationID: nodeId, newParams) {
                Task { @MainActor in
                    self.inputParams = newParams
                    self.lastUpdateTime = Date()
                }
            } else {
                Task { @MainActor in
                    self.errorMessage = "Failed to update input parameters"
                }
            }
        }
    }
    
    // MARK: - Convenience Methods
    
    /// Update master mute
    func setMasterMute(_ enabled: Bool) {
        var newState = outputState
        newState.muteEnabled = enabled
        updateOutputState(newState)
    }
    
    /// Update master dim
    func setMasterDim(_ enabled: Bool) {
        var newState = outputState
        newState.dimEnabled = enabled
        updateOutputState(newState)
    }
    
    /// Update output volume for specific channel
    func setOutputVolume(_ volume: Int8, channel: Int) {
        guard channel >= 0 && channel < 6 else { return }
        var newState = outputState
        newState.volumes[channel] = volume
        updateOutputState(newState)
    }
    
    /// Update output mute for specific channel
    func setOutputMute(_ muted: Bool, channel: Int) {
        guard channel >= 0 && channel < 6 else { return }
        var newState = outputState
        newState.volMutes[channel] = muted
        updateOutputState(newState)
    }
    
    /// Update mic input level for specific channel
    func setMicLevel(_ level: MicInputLevel, channel: Int) {
        guard channel >= 0 && channel < 2 else { return }
        var newParams = inputParams
        newParams.micLevels[channel] = level
        updateInputParams(newParams)
    }
    
    /// Update line input level for specific channel
    func setLineLevel(_ level: LineInputLevel, channel: Int) {
        guard channel >= 0 && channel < 2 else { return }
        var newParams = inputParams
        newParams.lineLevels[channel] = level
        updateInputParams(newParams)
    }
    
    // MARK: - Auto Refresh
    
    func startAutoRefresh(interval: TimeInterval = 1.0) {
        stopAutoRefresh()
        
        refreshTimer = Timer.scheduledTimer(withTimeInterval: interval, repeats: true) { [weak self] _ in
            guard let self else { return }
            Task { @MainActor [weak self] in
                self?.refresh()
            }
        }
        
        // Initial refresh
        refresh()
    }
    
    func stopAutoRefresh() {
        refreshTimer?.invalidate()
        refreshTimer = nil
    }
}
