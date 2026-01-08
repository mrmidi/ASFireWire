import Foundation
import IOKit

extension ASFWDriverConnector {
    // MARK: - IRM Test Methods (Phase 0.5)
    
    /// Trigger IRM allocation for testing (channel 0, 84 bandwidth units to match Apple logs)
    /// Check Console.app logs for results
    func testIRMAllocation() -> Bool {
        guard isConnected else {
            log("testIRMAllocation: Not connected", level: .warning)
            return false
        }
        guard connection != 0 else { return false }

        let kr = IOConnectCallScalarMethod(
            connection,
            Method.testIRMAllocation.rawValue,
            nil, 0,  // No inputs
            nil, nil // No outputs
        )

        if kr != KERN_SUCCESS {
            print("[Connector] ❌ testIRMAllocation failed: \(interpretIOReturn(kr))")
            log("testIRMAllocation failed: \(interpretIOReturn(kr))", level: .error)
            return false
        }

        print("[Connector] ✅ testIRMAllocation triggered - check Console.app logs")
        log("IRM allocation test triggered - check Console.app logs", level: .info)
        return true
    }

    /// Release IRM resources (channel 0, 84 bandwidth units)
    /// Check Console.app logs for results
    func testIRMRelease() -> Bool {
        guard isConnected else {
            log("testIRMRelease: Not connected", level: .warning)
            return false
        }
        guard connection != 0 else { return false }

        let kr = IOConnectCallScalarMethod(
            connection,
            Method.testIRMRelease.rawValue,
            nil, 0,  // No inputs
            nil, nil // No outputs
        )

        if kr != KERN_SUCCESS {
            print("[Connector] ❌ testIRMRelease failed: \(interpretIOReturn(kr))")
            log("testIRMRelease failed: \(interpretIOReturn(kr))", level: .error)
            return false
        }

        print("[Connector] ✅ testIRMRelease triggered - check Console.app logs")
        log("IRM release test triggered - check Console.app logs", level: .info)
        return true
    }
    
    // MARK: - CMP Test Methods (Phase 0.5)
    
    /// Connect oPCR[0] (device→host stream) - increments p2p count
    func testCMPConnectOPCR() -> Bool {
        guard isConnected, connection != 0 else { return false }
        
        let kr = IOConnectCallScalarMethod(connection, Method.testCMPConnectOPCR.rawValue, nil, 0, nil, nil)
        if kr != KERN_SUCCESS {
            print("[Connector] ❌ testCMPConnectOPCR failed: \(interpretIOReturn(kr))")
            return false
        }
        print("[Connector] ✅ CMP oPCR connect triggered - check Console.app")
        return true
    }
    
    /// Disconnect oPCR[0] (device→host stream) - decrements p2p count
    func testCMPDisconnectOPCR() -> Bool {
        guard isConnected, connection != 0 else { return false }
        
        let kr = IOConnectCallScalarMethod(connection, Method.testCMPDisconnectOPCR.rawValue, nil, 0, nil, nil)
        if kr != KERN_SUCCESS {
            print("[Connector] ❌ testCMPDisconnectOPCR failed: \(interpretIOReturn(kr))")
            return false
        }
        print("[Connector] ✅ CMP oPCR disconnect triggered - check Console.app")
        return true
    }
    
    /// Connect iPCR[0] (host→device stream) - increments p2p count, sets channel 1
    func testCMPConnectIPCR() -> Bool {
        guard isConnected, connection != 0 else { return false }
        
        let kr = IOConnectCallScalarMethod(connection, Method.testCMPConnectIPCR.rawValue, nil, 0, nil, nil)
        if kr != KERN_SUCCESS {
            print("[Connector] ❌ testCMPConnectIPCR failed: \(interpretIOReturn(kr))")
            return false
        }
        print("[Connector] ✅ CMP iPCR connect triggered - check Console.app")
        return true
    }
    
    /// Disconnect iPCR[0] (host→device stream) - decrements p2p count
    func testCMPDisconnectIPCR() -> Bool {
        guard isConnected, connection != 0 else { return false }
        
        let kr = IOConnectCallScalarMethod(connection, Method.testCMPDisconnectIPCR.rawValue, nil, 0, nil, nil)
        if kr != KERN_SUCCESS {
            print("[Connector] ❌ testCMPDisconnectIPCR failed: \(interpretIOReturn(kr))")
            return false
        }
        print("[Connector] ✅ CMP iPCR disconnect triggered - check Console.app")
        return true
    }
    
    // MARK: - IT DMA Allocation (Phase 1.5)
    
    /// Allocate IT DMA memory without CMP connection
    /// WARNING: This only allocates DMA buffers, it does NOT start hardware transmission.
    /// CMP iPCR connection must be done separately (TODO).
    func allocateITDMA(channel: UInt8 = 1) -> Bool {
        guard isConnected, connection != 0 else { return false }
        
        var input: [UInt64] = [UInt64(channel)]
        let kr = IOConnectCallScalarMethod(connection, Method.startIsochTransmit.rawValue, &input, 1, nil, nil)
        if kr != KERN_SUCCESS {
            print("[Connector] ❌ allocateITDMA failed: \(interpretIOReturn(kr))")
            return false
        }
        print("[Connector] ✅ IT DMA allocated for channel \(channel) - check Console.app")
        return true
    }
    
    /// Deallocate IT DMA memory
    func deallocateITDMA() -> Bool {
        guard isConnected, connection != 0 else { return false }
        
        let kr = IOConnectCallScalarMethod(connection, Method.stopIsochTransmit.rawValue, nil, 0, nil, nil)
        if kr != KERN_SUCCESS {
            print("[Connector] ❌ deallocateITDMA failed: \(interpretIOReturn(kr))")
            return false
        }
        print("[Connector] ✅ IT DMA deallocated - check Console.app")
        return true
    }
}

