//
//  DriverConnector+Isoch.swift
//  ASFW
//
//  Isoch Receive control and metrics
//

import Foundation
import IOKit

// MARK: - Isoch RX Metrics Model

/// Isoch Receive metrics snapshot (matches C++ IsochRxSnapshot exactly: 216 bytes)
struct IsochRxMetrics {
    var totalPackets: UInt64 = 0
    var dataPackets: UInt64 = 0      // 80-byte with samples
    var emptyPackets: UInt64 = 0     // 16-byte empty
    var drops: UInt64 = 0            // DBC discontinuities  
    var errors: UInt64 = 0           // CIP parse errors
    
    // Latency histogram [<100µs, 100-500µs, 500-1000µs, >1000µs]
    var latencyHist: (UInt64, UInt64, UInt64, UInt64) = (0, 0, 0, 0)
    
    // Last poll cycle
    var lastPollLatencyUs: UInt32 = 0
    var lastPollPackets: UInt32 = 0
    
    // CIP header snapshot
    var cipSID: UInt8 = 0
    var cipDBS: UInt8 = 0
    var cipFDF: UInt8 = 0
    var cipSYT: UInt16 = 0xFFFF
    var cipDBC: UInt8 = 0

    // Recording health
    var decodedFrames: UInt64 = 0
    var rxQueueUnderreadEvents: UInt64 = 0
    var rxQueueUnderreadFrames: UInt64 = 0
    var rxQueueProducerDropEvents: UInt64 = 0
    var rxQueueProducerDropFrames: UInt64 = 0
    var transportAnchorSampleFrame: UInt64 = 0
    var transportAnchorHostTicks: UInt64 = 0
    var startupAlignmentSampleTime: UInt64 = 0
    var startupInputSampleOffset: UInt64 = 0
    var startupOutputSampleOffset: UInt64 = 0
    var sytClockLostCount: UInt64 = 0
    var rxQueueFillFrames: UInt32 = 0
    var rxQueueCapacityFrames: UInt32 = 0
    var corrHostNanosPerSampleQ8: UInt32 = 0
    var transportHostNanosPerSampleQ8: UInt32 = 0
    var sytClockUpdateSeq: UInt32 = 0
    var halInputLatencyFrames: UInt32 = 0
    var halInputSafetyOffsetFrames: UInt32 = 0
    var transportTimingValid: Bool = false
    var startupAlignmentValid: Bool = false
    var activeRxProfile: UInt8 = 1
    var sytClockEstablished: Bool = false
    var sytClockActive: Bool = false
    var sytStartupQualified: Bool = false
    
    // Computed properties
    var packetsPerSecond: Double {
        // This is an instant value, not real rate. GUI should compute from delta.
        0
    }
    
    var dataRatio: Double {
        guard totalPackets > 0 else { return 0 }
        return Double(dataPackets) / Double(totalPackets)
    }

    var transportRateHz: Double? {
        guard transportHostNanosPerSampleQ8 > 0 else { return nil }
        let nanosPerSample = Double(transportHostNanosPerSampleQ8) / 256.0
        guard nanosPerSample > 0 else { return nil }
        return 1_000_000_000.0 / nanosPerSample
    }

    var activeRxProfileName: String {
        switch activeRxProfile {
        case 0: return "A"
        case 2: return "C"
        default: return "B"
        }
    }
}

// MARK: - Driver Connector Extension

extension ASFWDriverConnector {
    
    // MARK: - Isoch Receive Control
    
    /// Start isochronous receive on specified channel
    func startIsochReceive(channel: UInt8) -> Bool {
        guard isConnected, connection != 0 else { return false }
        
        var input: [UInt64] = [UInt64(channel)]
        let kr = IOConnectCallScalarMethod(
            connection,
            Method.startIsochReceive.rawValue,
            &input, 1,
            nil, nil
        )
        
        if kr != KERN_SUCCESS {
            log("startIsochReceive failed: \(interpretIOReturn(kr))", level: .error)
            return false
        }
        log("Started isoch receive on channel \(channel)", level: .info)
        return true
    }
    
    /// Stop isochronous receive
    func stopIsochReceive() -> Bool {
        guard isConnected, connection != 0 else { return false }
        
        let kr = IOConnectCallScalarMethod(
            connection,
            Method.stopIsochReceive.rawValue,
            nil, 0,
            nil, nil
        )
        
        if kr != KERN_SUCCESS {
            log("stopIsochReceive failed: \(interpretIOReturn(kr))", level: .error)
            return false
        }
        log("Stopped isoch receive", level: .info)
        return true
    }
    
    // MARK: - Isoch RX Metrics
    
    /// Fetch current isoch receive metrics from driver
    func getIsochRxMetrics() -> IsochRxMetrics? {
        guard isConnected, connection != 0 else { return nil }
        
        guard let data = callStruct(.getIsochRxMetrics, initialCap: 256) else {
            log("getIsochRxMetrics: callStruct failed", level: .warning)
            return nil
        }
        
        guard data.count >= 216 else {
            log("getIsochRxMetrics: Invalid data size \(data.count)", level: .warning)
            return nil
        }
        
        return data.withUnsafeBytes { ptr -> IsochRxMetrics in
            var m = IsochRxMetrics()
            let base = ptr.baseAddress!
            
            m.totalPackets = base.load(fromByteOffset: 0, as: UInt64.self)
            m.dataPackets = base.load(fromByteOffset: 8, as: UInt64.self)
            m.emptyPackets = base.load(fromByteOffset: 16, as: UInt64.self)
            m.drops = base.load(fromByteOffset: 24, as: UInt64.self)
            m.errors = base.load(fromByteOffset: 32, as: UInt64.self)
            
            m.latencyHist = (
                base.load(fromByteOffset: 40, as: UInt64.self),
                base.load(fromByteOffset: 48, as: UInt64.self),
                base.load(fromByteOffset: 56, as: UInt64.self),
                base.load(fromByteOffset: 64, as: UInt64.self)
            )
            
            m.lastPollLatencyUs = base.load(fromByteOffset: 72, as: UInt32.self)
            m.lastPollPackets = base.load(fromByteOffset: 76, as: UInt32.self)
            
            m.cipSID = base.load(fromByteOffset: 80, as: UInt8.self)
            m.cipDBS = base.load(fromByteOffset: 81, as: UInt8.self)
            m.cipFDF = base.load(fromByteOffset: 82, as: UInt8.self)
            // pad1 at 83
            m.cipSYT = base.load(fromByteOffset: 84, as: UInt16.self)
            m.cipDBC = base.load(fromByteOffset: 86, as: UInt8.self)
            // pad2 at 87

            m.decodedFrames = base.load(fromByteOffset: 88, as: UInt64.self)
            m.rxQueueUnderreadEvents = base.load(fromByteOffset: 96, as: UInt64.self)
            m.rxQueueUnderreadFrames = base.load(fromByteOffset: 104, as: UInt64.self)
            m.rxQueueProducerDropEvents = base.load(fromByteOffset: 112, as: UInt64.self)
            m.rxQueueProducerDropFrames = base.load(fromByteOffset: 120, as: UInt64.self)
            m.transportAnchorSampleFrame = base.load(fromByteOffset: 128, as: UInt64.self)
            m.transportAnchorHostTicks = base.load(fromByteOffset: 136, as: UInt64.self)
            m.startupAlignmentSampleTime = base.load(fromByteOffset: 144, as: UInt64.self)
            m.startupInputSampleOffset = base.load(fromByteOffset: 152, as: UInt64.self)
            m.startupOutputSampleOffset = base.load(fromByteOffset: 160, as: UInt64.self)
            m.sytClockLostCount = base.load(fromByteOffset: 168, as: UInt64.self)
            m.rxQueueFillFrames = base.load(fromByteOffset: 176, as: UInt32.self)
            m.rxQueueCapacityFrames = base.load(fromByteOffset: 180, as: UInt32.self)
            m.corrHostNanosPerSampleQ8 = base.load(fromByteOffset: 184, as: UInt32.self)
            m.transportHostNanosPerSampleQ8 = base.load(fromByteOffset: 188, as: UInt32.self)
            m.sytClockUpdateSeq = base.load(fromByteOffset: 192, as: UInt32.self)
            m.halInputLatencyFrames = base.load(fromByteOffset: 196, as: UInt32.self)
            m.halInputSafetyOffsetFrames = base.load(fromByteOffset: 200, as: UInt32.self)
            m.transportTimingValid = base.load(fromByteOffset: 204, as: UInt8.self) != 0
            m.startupAlignmentValid = base.load(fromByteOffset: 205, as: UInt8.self) != 0
            m.activeRxProfile = base.load(fromByteOffset: 206, as: UInt8.self)
            m.sytClockEstablished = base.load(fromByteOffset: 207, as: UInt8.self) != 0
            m.sytClockActive = base.load(fromByteOffset: 208, as: UInt8.self) != 0
            m.sytStartupQualified = base.load(fromByteOffset: 209, as: UInt8.self) != 0
            
            return m
        }
    }
    
    /// Reset isochronous receive metrics
    func resetIsochRxMetrics() -> Bool {
        guard isConnected, connection != 0 else { return false }
        
        // Use a new selector for reset (need to add to Method enum first)
        // See: kMethodResetIsochRxMetrics = 35
        let kr = IOConnectCallScalarMethod(
            connection,
            35, // Hardcoded for now until Method enum updated
            nil, 0,
            nil, nil
        )
        
        if kr != KERN_SUCCESS {
            log("resetIsochRxMetrics failed: \(interpretIOReturn(kr))", level: .error)
            return false
        }
        log("Reset isoch metrics", level: .info)
        return true
    }
}
