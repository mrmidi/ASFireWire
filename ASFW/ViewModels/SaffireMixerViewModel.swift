//
//  SaffireMixerViewModel.swift
//  ASFW
//
//  Created by ASFireWire Project on 2026-02-08.
//

import Foundation
import Combine

class SaffireMixerViewModel: ObservableObject {
    
    // MARK: - Published Properties
    
    @Published var outputState: OutputGroupState = OutputGroupState()
    @Published var inputParams: InputParams = InputParams()
    @Published var isLoading: Bool = false
    @Published var errorMessage: String?
    @Published var lastUpdateTime: Date?

    // Resolved device target (runtime, from discovery).
    @Published var deviceGUID: UInt64?
    @Published var deviceNodeId: UInt16?
    
    // MARK: - Private Properties
    
    private let connector: ASFWDriverConnector
    private var cancellables = Set<AnyCancellable>()
    private var refreshTimer: Timer?

    private static let focusriteVendorId: UInt32 = 0x00130e
    private static let saffirePro24DspModelId: UInt32 = 0x000008
    
    // MARK: - Initialization
    
    init(connector: ASFWDriverConnector) {
        self.connector = connector
        setupObservers()
    }
    
    deinit {
        stopAutoRefresh()
    }
    
    // MARK: - Setup
    
    private func setupObservers() {
        // Observe connection state changes
        connector.$isConnected
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

        guard let devices = connector.getDiscoveredDevices() else { return }

        // If we already have a GUID but nodeId is missing, refresh just nodeId.
        if let guid = deviceGUID,
           let device = devices.first(where: { $0.guid == guid }) {
            deviceNodeId = UInt16(device.nodeId)
            return
        }

        // Otherwise, attempt to find the Saffire Pro 24 DSP.
        if let device = devices.first(where: { $0.vendorId == Self.focusriteVendorId && $0.modelId == Self.saffirePro24DspModelId }) {
            deviceGUID = device.guid
            deviceNodeId = UInt16(device.nodeId)
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
        
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            
            // Read output group state
            if let outputState = self.connector.getSaffireOutputGroup(destinationID: nodeId) {
                DispatchQueue.main.async {
                    self.outputState = outputState
                }
            } else {
                DispatchQueue.main.async {
                    self.errorMessage = "Failed to read output state"
                }
            }
            
            // Read input parameters
            if let inputParams = self.connector.getSaffireInputParams(destinationID: nodeId) {
                DispatchQueue.main.async {
                    self.inputParams = inputParams
                }
            } else {
                DispatchQueue.main.async {
                    self.errorMessage = "Failed to read input parameters"
                }
            }
            
            DispatchQueue.main.async {
                self.isLoading = false
                self.lastUpdateTime = Date()
            }
        }
    }
    
    /// Update output group state on device
    func updateOutputState(_ newState: OutputGroupState) {
        resolveTargetIfNeeded()
        guard let nodeId = deviceNodeId else {
            errorMessage = "No Saffire Pro 24 DSP found"
            return
        }

        errorMessage = nil
        
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            
            if self.connector.setSaffireOutputGroup(destinationID: nodeId, newState) {
                DispatchQueue.main.async {
                    self.outputState = newState
                    self.lastUpdateTime = Date()
                }
            } else {
                DispatchQueue.main.async {
                    self.errorMessage = "Failed to update output state"
                }
            }
        }
    }
    
    /// Update input parameters on device
    func updateInputParams(_ newParams: InputParams) {
        resolveTargetIfNeeded()
        guard let nodeId = deviceNodeId else {
            errorMessage = "No Saffire Pro 24 DSP found"
            return
        }

        errorMessage = nil
        
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            
            if self.connector.setSaffireInputParams(destinationID: nodeId, newParams) {
                DispatchQueue.main.async {
                    self.inputParams = newParams
                    self.lastUpdateTime = Date()
                }
            } else {
                DispatchQueue.main.async {
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
            self?.refresh()
        }
        
        // Initial refresh
        refresh()
    }
    
    func stopAutoRefresh() {
        refreshTimer?.invalidate()
        refreshTimer = nil
    }
}
