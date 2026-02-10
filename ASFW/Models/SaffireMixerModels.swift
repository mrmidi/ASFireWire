//
//  SaffireMixerModels.swift
//  ASFW
//
//  Created by ASFireWire Project on 2026-02-08.
//

import Foundation

// MARK: - Saffire Mixer Models

/// Microphone input level setting
enum MicInputLevel: UInt8, CaseIterable, Identifiable {
    case line = 0        // Gain range: -10dB to +36dB
    case instrument = 1  // Gain range: +13dB to +60dB, headroom: +8dBu
    
    var id: UInt8 { rawValue }
    
    var displayName: String {
        switch self {
        case .line: return "Line"
        case .instrument: return "Instrument"
        }
    }
    
    var description: String {
        switch self {
        case .line: return "-10dB to +36dB"
        case .instrument: return "+13dB to +60dB"
        }
    }
}

/// Line input level setting
enum LineInputLevel: UInt8, CaseIterable, Identifiable {
    case low = 0   // +16 dBu
    case high = 1  // -10 dBV
    
    var id: UInt8 { rawValue }
    
    var displayName: String {
        switch self {
        case .low: return "Low"
        case .high: return "High"
        }
    }
    
    var description: String {
        switch self {
        case .low: return "+16 dBu"
        case .high: return "-10 dBV"
        }
    }
}

/// Optical output interface mode
enum OpticalOutIfaceMode: UInt8, CaseIterable, Identifiable {
    case adat = 0    // ADAT signal
    case spdif = 1   // S/PDIF signal
    case aesEbu = 2  // AES/EBU signal
    
    var id: UInt8 { rawValue }
    
    var displayName: String {
        switch self {
        case .adat: return "ADAT"
        case .spdif: return "S/PDIF"
        case .aesEbu: return "AES/EBU"
        }
    }
}

/// Analog input parameters
struct InputParams: Equatable {
    var micLevels: [MicInputLevel]    // 2 channels
    var lineLevels: [LineInputLevel]  // 2 channels
    
    init() {
        self.micLevels = [.line, .line]
        self.lineLevels = [.low, .low]
    }
    
    /// Parse from big-endian wire format (8 bytes)
    static func fromWire(_ data: Data) -> InputParams {
        var params = InputParams()
        guard data.count >= 8 else { return params }
        
        params.micLevels[0] = MicInputLevel(rawValue: data[0]) ?? .line
        params.micLevels[1] = MicInputLevel(rawValue: data[1]) ?? .line
        params.lineLevels[0] = LineInputLevel(rawValue: data[2]) ?? .low
        params.lineLevels[1] = LineInputLevel(rawValue: data[3]) ?? .low
        
        return params
    }
    
    /// Serialize to big-endian wire format (8 bytes)
    func toWire() -> Data {
        var data = Data(count: 8)
        data[0] = micLevels[0].rawValue
        data[1] = micLevels[1].rawValue
        data[2] = lineLevels[0].rawValue
        data[3] = lineLevels[1].rawValue
        // Bytes 4-7 are reserved (zero)
        return data
    }
}

/// Output group state (dim, mute, volumes)
struct OutputGroupState: Equatable {
    var muteEnabled: Bool
    var dimEnabled: Bool
    var volumes: [Int8]        // Per-output volume (0-127, inverted scale)
    var volMutes: [Bool]       // Per-output mute
    var volHwCtls: [Bool]      // Per-output hardware knob control
    var muteHwCtls: [Bool]     // Per-output hardware mute button
    var dimHwCtls: [Bool]      // Per-output hardware dim button
    var hwKnobValue: Int8      // Current hardware knob value (read-only)
    
    static let kVolMin: Int8 = 0
    static let kVolMax: Int8 = 127
    
    init() {
        self.muteEnabled = false
        self.dimEnabled = false
        self.volumes = Array(repeating: 100, count: 6)
        self.volMutes = Array(repeating: false, count: 6)
        self.volHwCtls = Array(repeating: false, count: 6)
        self.muteHwCtls = Array(repeating: false, count: 6)
        self.dimHwCtls = Array(repeating: false, count: 6)
        self.hwKnobValue = 0
    }
    
    /// Parse from big-endian wire format (0x50 = 80 bytes)
    static func fromWire(_ data: Data) -> OutputGroupState {
        var state = OutputGroupState()
        guard data.count >= 80 else { return state }
        
        // First quadlet: mute/dim status
        let status = data.withUnsafeBytes { $0.load(as: UInt32.self).bigEndian }
        state.muteEnabled = (status & 0x01) != 0
        state.dimEnabled = (status & 0x02) != 0
        
        // Second quadlet: hardware knob value
        let knobData = data.withUnsafeBytes { $0.load(fromByteOffset: 4, as: UInt32.self).bigEndian }
        state.hwKnobValue = Int8(knobData & 0x7F)
        
        // Per-output entries (6 channels, 8 bytes each, starting at offset 8)
        for i in 0..<6 {
            let offset = 8 + i * 8
            
            let volData = data.withUnsafeBytes { $0.load(fromByteOffset: offset, as: UInt32.self).bigEndian }
            state.volumes[i] = Int8(volData & 0x7F)
            state.volMutes[i] = (volData & 0x80) != 0
            
            let flags = data.withUnsafeBytes { $0.load(fromByteOffset: offset + 4, as: UInt32.self).bigEndian }
            state.volHwCtls[i] = (flags & 0x01) != 0
            state.muteHwCtls[i] = (flags & 0x02) != 0
            state.dimHwCtls[i] = (flags & 0x04) != 0
        }
        
        return state
    }
    
    /// Serialize to big-endian wire format (0x50 = 80 bytes)
    func toWire() -> Data {
        var data = Data(count: 80)
        
        // First quadlet: mute/dim status
        var status: UInt32 = 0
        if muteEnabled { status |= 0x01 }
        if dimEnabled { status |= 0x02 }
        data.withUnsafeMutableBytes { $0.storeBytes(of: status.bigEndian, as: UInt32.self) }
        
        // Second quadlet: hardware knob value
        let knobData = UInt32(hwKnobValue) & 0x7F
        data.withUnsafeMutableBytes { $0.storeBytes(of: knobData.bigEndian, toByteOffset: 4, as: UInt32.self) }
        
        // Per-output entries
        for i in 0..<6 {
            let offset = 8 + i * 8
            
            var volData = UInt32(volumes[i]) & 0x7F
            if volMutes[i] { volData |= 0x80 }
            data.withUnsafeMutableBytes { $0.storeBytes(of: volData.bigEndian, toByteOffset: offset, as: UInt32.self) }
            
            var flags: UInt32 = 0
            if volHwCtls[i] { flags |= 0x01 }
            if muteHwCtls[i] { flags |= 0x02 }
            if dimHwCtls[i] { flags |= 0x04 }
            data.withUnsafeMutableBytes { $0.storeBytes(of: flags.bigEndian, toByteOffset: offset + 4, as: UInt32.self) }
        }
        
        return data
    }
}
