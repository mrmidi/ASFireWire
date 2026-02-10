//
//  AudioDebugViewModel.swift
//  ASFW
//
//  Created for ASFireWire Audio Debugging.
//

import Foundation
import Combine
import CoreAudio

class AudioDebugViewModel: ObservableObject {
    @Published var devices: [AudioWrapperDevice] = []
    @Published var selectedDevice: AudioWrapperDevice?
    @Published var selectedDeviceStreams: [AudioStream] = []
    
    // Auto-select ASFW device if found
    private let targetDeviceName = "FireWire" // Adjust based on your driver's actual name
    
    init() {
        refreshDevices()
    }
    
    func refreshDevices() {
        devices = AudioSystem.shared.devices
        
        // Try to find our driver device
        if let target = devices.first(where: { $0.transportType == .fireWire || $0.name.contains("ASFW") || $0.name.contains("FireWire") }) {
            selectDevice(target)
        } else if let first = devices.first {
            selectDevice(first)
        }
    }
    
    func selectDevice(_ device: AudioWrapperDevice) {
        selectedDevice = device
        selectedDeviceStreams = device.inputStreams + device.outputStreams
    }
}
