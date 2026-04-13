//
//  DriverConnector+Saffire.swift
//  ASFW
//
//  Created by ASFireWire Project on 2026-02-08.
//

import Foundation

// MARK: - Saffire Mixer Control

extension ASFWDriverConnector {
    
    // Saffire Pro TCAT application section offsets
    private enum SaffireOffset {
        // Linux DICE reference for Saffire Pro 24 DSP documents the
        // application section offset at 0x6DD4.
        // Source: snd-firewire-ctl-services/protocols/dice/src/focusrite/spro24dsp.rs
        static let appSectionBase: UInt32 = 0x6DD4
        static let outputGroup: UInt32 = appSectionBase + 0x000C
        static let inputParams: UInt32 = appSectionBase + 0x0058
        static let swNotice: UInt32 = appSectionBase + 0x05EC
    }
    
    // Software notice values
    private enum SaffireSwNotice: UInt32 {
        case outputGroupChanged = 0x02
        case inputChanged = 0x04
    }
    
    /// Read output group state from Saffire device
    /// - Returns: OutputGroupState if successful, nil otherwise
    func getSaffireOutputGroup(destinationID: UInt16) -> OutputGroupState? {
        guard isConnected else {
            log("Cannot get Saffire output group: not connected", level: .error)
            return nil
        }
        
        // Read 80 bytes (0x50) from output group offset
        guard let data = readTCATApplicationData(destinationID: destinationID,
                                                 offset: SaffireOffset.outputGroup,
                                                 length: 80) else {
            log("Failed to read Saffire output group", level: .error)
            return nil
        }
        
        return OutputGroupState.fromWire(data)
    }
    
    /// Write output group state to Saffire device
    /// - Parameter state: Output group state to write
    /// - Returns: true if successful
    func setSaffireOutputGroup(destinationID: UInt16, _ state: OutputGroupState) -> Bool {
        guard isConnected else {
            log("Cannot set Saffire output group: not connected", level: .error)
            return false
        }
        
        let data = state.toWire()
        
        // Write to device
        guard writeTCATApplicationData(destinationID: destinationID,
                                       offset: SaffireOffset.outputGroup,
                                       data: data) else {
            log("Failed to write Saffire output group", level: .error)
            return false
        }
        
        // Send software notice to commit changes
        guard sendSaffireSwNotice(destinationID: destinationID, .outputGroupChanged) else {
            log("Failed to send Saffire output group change notice", level: .error)
            return false
        }
        
        return true
    }
    
    /// Read input parameters from Saffire device
    /// - Returns: InputParams if successful, nil otherwise
    func getSaffireInputParams(destinationID: UInt16) -> InputParams? {
        guard isConnected else {
            log("Cannot get Saffire input params: not connected", level: .error)
            return nil
        }
        
        // Read 8 bytes from input params offset
        guard let data = readTCATApplicationData(destinationID: destinationID,
                                                 offset: SaffireOffset.inputParams,
                                                 length: 8) else {
            log("Failed to read Saffire input params", level: .error)
            return nil
        }
        
        return InputParams.fromWire(data)
    }
    
    /// Write input parameters to Saffire device
    /// - Parameter params: Input parameters to write
    /// - Returns: true if successful
    func setSaffireInputParams(destinationID: UInt16, _ params: InputParams) -> Bool {
        guard isConnected else {
            log("Cannot set Saffire input params: not connected", level: .error)
            return false
        }
        
        let data = params.toWire()
        
        // Write to device
        guard writeTCATApplicationData(destinationID: destinationID,
                                       offset: SaffireOffset.inputParams,
                                       data: data) else {
            log("Failed to write Saffire input params", level: .error)
            return false
        }
        
        // Send software notice to commit changes
        guard sendSaffireSwNotice(destinationID: destinationID, .inputChanged) else {
            log("Failed to send Saffire input params change notice", level: .error)
            return false
        }
        
        return true
    }
    
    // MARK: - Private Helpers
    
    /// Read data from TCAT application section
    private func readTCATApplicationData(destinationID: UInt16, offset: UInt32, length: Int) -> Data? {
        // TCAT application section base address
        // For Saffire Pro, this is typically at 0xFFFFE0200000 + offset
        let tcatBaseHigh: UInt16 = 0xFFFF
        let tcatBaseLow: UInt32 = 0xE0200000
        
        let address = tcatBaseLow + offset
        
        // Use async read to fetch data
        var result: Data?
        let semaphore = DispatchSemaphore(value: 0)
        
        DispatchQueue.global(qos: .userInitiated).async {
            if let data = self.performSyncAsyncRead(
                destinationID: destinationID,
                addressHigh: tcatBaseHigh,
                addressLow: address,
                length: UInt32(length)
            ) {
                result = data
            }
            semaphore.signal()
        }
        
        _ = semaphore.wait(timeout: .now() + 2.0)
        return result
    }
    
    /// Write data to TCAT application section
    private func writeTCATApplicationData(destinationID: UInt16, offset: UInt32, data: Data) -> Bool {
        let tcatBaseHigh: UInt16 = 0xFFFF
        let tcatBaseLow: UInt32 = 0xE0200000
        
        let address = tcatBaseLow + offset
        
        var success = false
        let semaphore = DispatchSemaphore(value: 0)
        
        DispatchQueue.global(qos: .userInitiated).async {
            success = self.performSyncAsyncWrite(
                destinationID: destinationID,
                addressHigh: tcatBaseHigh,
                addressLow: address,
                payload: data
            )
            semaphore.signal()
        }
        
        _ = semaphore.wait(timeout: .now() + 2.0)
        return success
    }
    
    /// Send software notice to Saffire device to commit parameter changes
    private func sendSaffireSwNotice(destinationID: UInt16, _ notice: SaffireSwNotice) -> Bool {
        let noticeData = withUnsafeBytes(of: notice.rawValue.bigEndian) { Data($0) }
        return writeTCATApplicationData(destinationID: destinationID,
                                        offset: SaffireOffset.swNotice,
                                        data: noticeData)
    }
    
    /// Synchronous async read helper
    private func performSyncAsyncRead(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, length: UInt32) -> Data? {
        guard let handle = asyncRead(
            destinationID: destinationID,
            addressHigh: addressHigh,
            addressLow: addressLow,
            length: length
        ) else {
            return nil
        }

        let timeout = Date().addingTimeInterval(2.0)
        while Date() < timeout {
            if let result = getTransactionResult(handle: handle, initialPayloadCapacity: Int(length) + 128) {
                guard result.status == 0 && result.responseCode == 0 else {
                    log(String(format: "Saffire read failed status=0x%08X rCode=0x%02X", result.status, result.responseCode), level: .warning)
                    return nil
                }
                return result.payload
            }
            Thread.sleep(forTimeInterval: 0.05)
        }

        log(String(format: "Saffire read timed out waiting for result (handle=0x%04X)", handle), level: .warning)
        return nil
    }
    
    /// Synchronous async write helper
    private func performSyncAsyncWrite(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, payload: Data) -> Bool {
        guard let handle = asyncBlockWrite(
            destinationID: destinationID,
            addressHigh: addressHigh,
            addressLow: addressLow,
            payload: payload
        ) else {
            return false
        }

        let timeout = Date().addingTimeInterval(2.0)
        while Date() < timeout {
            if let result = getTransactionResult(handle: handle) {
                return result.status == 0 && result.responseCode == 0
            }
            Thread.sleep(forTimeInterval: 0.05)
        }

        log(String(format: "Saffire block write timed out waiting for result (handle=0x%04X)", handle), level: .warning)
        return false
    }
}
