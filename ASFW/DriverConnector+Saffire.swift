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
        static let outputGroup: UInt32 = 0x000c
        static let inputParams: UInt32 = 0x0058
        static let swNotice: UInt32 = 0x05ec
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
        // Create input structure
        var input = Data(capacity: 12)
        input.append(contentsOf: withUnsafeBytes(of: destinationID.bigEndian) { Data($0) })
        input.append(contentsOf: withUnsafeBytes(of: addressHigh.bigEndian) { Data($0) })
        input.append(contentsOf: withUnsafeBytes(of: addressLow.bigEndian) { Data($0) })
        input.append(contentsOf: withUnsafeBytes(of: length.bigEndian) { Data($0) })
        
        return callStruct(.asyncRead, input: input, initialCap: Int(length) + 128)
    }
    
    /// Synchronous async write helper
    private func performSyncAsyncWrite(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, payload: Data) -> Bool {
        var input = Data(capacity: 12 + payload.count)
        input.append(contentsOf: withUnsafeBytes(of: destinationID.bigEndian) { Data($0) })
        input.append(contentsOf: withUnsafeBytes(of: addressHigh.bigEndian) { Data($0) })
        input.append(contentsOf: withUnsafeBytes(of: addressLow.bigEndian) { Data($0) })
        input.append(contentsOf: withUnsafeBytes(of: UInt32(payload.count).bigEndian) { Data($0) })
        input.append(payload)
        
        return callStruct(.asyncWrite, input: input) != nil
    }
}
