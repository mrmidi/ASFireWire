import Foundation
import IOKit

extension ASFWDriverConnector {

    // MARK: - SBP-2 Address Space Management

    /// Allocate an address range in the driver's SBP-2 address space.
    /// - Returns: A handle identifying the allocated range, or nil on failure.
    func allocateAddressRange(addressHi: UInt16,
                              addressLo: UInt32,
                              length: UInt32) -> UInt64? {
        guard isConnected else {
            log("allocateAddressRange: Not connected", level: .warning)
            return nil
        }

        var inputs: [UInt64] = [
            UInt64(addressHi),
            UInt64(addressLo),
            UInt64(length)
        ]

        var output: UInt64 = 0
        var outputCount: UInt32 = 1

        let kr = inputs.withUnsafeMutableBufferPointer { buffer -> kern_return_t in
            IOConnectCallScalarMethod(
                connection,
                Method.allocateAddressRange.rawValue,
                buffer.baseAddress,
                UInt32(buffer.count),
                &output,
                &outputCount)
        }

        guard kr == KERN_SUCCESS else {
            let errorMsg = "allocateAddressRange failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return nil
        }

        log(String(format: "SBP2 address range allocated (handle=0x%llX, len=%u)", output, length), level: .success)
        return output
    }

    /// Deallocate a previously allocated address range.
    /// - Returns: true on success.
    func deallocateAddressRange(handle: UInt64) -> Bool {
        guard isConnected else {
            log("deallocateAddressRange: Not connected", level: .warning)
            return false
        }

        var inputs: [UInt64] = [handle]

        let kr = inputs.withUnsafeMutableBufferPointer { buffer -> kern_return_t in
            IOConnectCallScalarMethod(
                connection,
                Method.deallocateAddressRange.rawValue,
                buffer.baseAddress,
                UInt32(buffer.count),
                nil,
                nil)
        }

        guard kr == KERN_SUCCESS else {
            let errorMsg = "deallocateAddressRange failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return false
        }

        log(String(format: "SBP2 address range deallocated (handle=0x%llX)", handle), level: .success)
        return true
    }

    /// Read data from an address range that was written by a remote device.
    /// - Returns: The data read from the range, or nil on failure.
    func readIncomingData(handle: UInt64,
                         offset: UInt32,
                         length: UInt32) -> Data? {
        guard isConnected else {
            log("readIncomingData: Not connected", level: .warning)
            return nil
        }

        var scalars: [UInt64] = [
            handle,
            UInt64(offset),
            UInt64(length)
        ]

        var outSize: Int = max(Int(length), 1)
        var out = Data(count: outSize)

        func doCall() -> kern_return_t {
            out.withUnsafeMutableBytes { outPtr in
                scalars.withUnsafeMutableBufferPointer { scalarPtr -> kern_return_t in
                    IOConnectCallMethod(
                        connection,
                        Method.readIncomingData.rawValue,
                        scalarPtr.baseAddress,
                        UInt32(scalarPtr.count),
                        nil,
                        0,
                        nil,
                        nil,
                        outPtr.baseAddress,
                        &outSize)
                }
            }
        }

        var kr = doCall()
        if kr == kIOReturnNoSpace {
            out = Data(count: outSize)
            kr = doCall()
        }

        guard kr == KERN_SUCCESS else {
            let errorMsg = "readIncomingData failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return nil
        }

        out.count = outSize
        log(String(format: "SBP2 readIncomingData (handle=0x%llX, offset=%u, %zu bytes)", handle, offset, outSize), level: .success)
        return out
    }

    /// Write data to a local address range for a remote device to read.
    /// - Returns: true on success.
    func writeLocalData(handle: UInt64,
                        offset: UInt32,
                        data: Data) -> Bool {
        guard isConnected else {
            log("writeLocalData: Not connected", level: .warning)
            return false
        }

        var scalars: [UInt64] = [
            handle,
            UInt64(offset),
            UInt64(data.count)
        ]

        let kr = data.withUnsafeBytes { dataPtr -> kern_return_t in
            scalars.withUnsafeMutableBufferPointer { scalarPtr -> kern_return_t in
                IOConnectCallMethod(
                    connection,
                    Method.writeLocalData.rawValue,
                    scalarPtr.baseAddress,
                    UInt32(scalarPtr.count),
                    dataPtr.baseAddress,
                    data.count,
                    nil,
                    nil,
                    nil,
                    nil)
            }
        }

        guard kr == KERN_SUCCESS else {
            let errorMsg = "writeLocalData failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return false
        }

        log(String(format: "SBP2 writeLocalData (handle=0x%llX, offset=%u, %zu bytes)", handle, offset, data.count), level: .success)
        return true
    }

    // MARK: - SBP-2 Session Management

    /// Create an SBP-2 session for a discovered unit.
    /// - Parameters:
    ///   - guidHi: Upper 32 bits of the device GUID.
    ///   - guidLo: Lower 32 bits of the device GUID.
    ///   - romOffset: Config ROM offset of the unit directory.
    /// - Returns: Session handle, or nil on failure.
    func createSBP2Session(guidHi: UInt32, guidLo: UInt32, romOffset: UInt32) -> UInt64? {
        guard isConnected else {
            log("createSBP2Session: Not connected", level: .warning)
            return nil
        }

        var inputs: [UInt64] = [UInt64(guidHi), UInt64(guidLo), UInt64(romOffset)]
        var output: UInt64 = 0
        var outputCount: UInt32 = 1

        let kr = inputs.withUnsafeMutableBufferPointer { buffer -> kern_return_t in
            IOConnectCallScalarMethod(
                connection,
                Method.createSBP2Session.rawValue,
                buffer.baseAddress,
                UInt32(buffer.count),
                &output,
                &outputCount)
        }

        guard kr == KERN_SUCCESS else {
            let errorMsg = "createSBP2Session failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return nil
        }

        log(String(format: "SBP2 session created (handle=0x%llX)", output), level: .success)
        return output
    }

    /// Start SBP-2 login for a session.
    /// - Returns: true if login was initiated successfully.
    @discardableResult
    func startSBP2Login(handle: UInt64) -> Bool {
        guard isConnected else {
            log("startSBP2Login: Not connected", level: .warning)
            return false
        }

        var inputs: [UInt64] = [handle]

        let kr = inputs.withUnsafeMutableBufferPointer { buffer -> kern_return_t in
            IOConnectCallScalarMethod(
                connection,
                Method.startSBP2Login.rawValue,
                buffer.baseAddress,
                UInt32(buffer.count),
                nil,
                nil)
        }

        guard kr == KERN_SUCCESS else {
            let errorMsg = "startSBP2Login failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return false
        }

        log(String(format: "SBP2 login started (handle=0x%llX)", handle), level: .success)
        return true
    }

    /// Get the current state of an SBP-2 session.
    /// - Returns: Tuple of (loginState, loginID, generation, lastError, reconnectPending).
    func getSBP2SessionState(handle: UInt64) -> (loginState: UInt8, loginID: UInt16, generation: UInt16, lastError: Int32, reconnectPending: Bool)? {
        guard isConnected else {
            log("getSBP2SessionState: Not connected", level: .warning)
            return nil
        }

        var inputs: [UInt64] = [handle]
        var outputs: [UInt64] = [0, 0, 0, 0, 0]
        var outputCount: UInt32 = 5

        let kr = inputs.withUnsafeMutableBufferPointer { inBuf -> kern_return_t in
            outputs.withUnsafeMutableBufferPointer { outBuf -> kern_return_t in
                IOConnectCallScalarMethod(
                    connection,
                    Method.getSBP2SessionState.rawValue,
                    inBuf.baseAddress,
                    UInt32(inBuf.count),
                    outBuf.baseAddress,
                    &outputCount)
            }
        }

        guard kr == KERN_SUCCESS else {
            return nil
        }

        return (
            loginState: UInt8(outputs[0] & 0xFF),
            loginID: UInt16(outputs[1] & 0xFFFF),
            generation: UInt16(outputs[2] & 0xFFFF),
            lastError: Int32(truncatingIfNeeded: outputs[3]),
            reconnectPending: outputs[4] != 0
        )
    }

    /// Submit a SCSI INQUIRY command to an SBP-2 session.
    /// - Returns: true if inquiry was submitted successfully.
    @discardableResult
    func submitSBP2Inquiry(handle: UInt64, allocationLength: UInt8 = 96) -> Bool {
        guard isConnected else {
            log("submitSBP2Inquiry: Not connected", level: .warning)
            return false
        }

        var inputs: [UInt64] = [handle, UInt64(allocationLength)]

        let kr = inputs.withUnsafeMutableBufferPointer { buffer -> kern_return_t in
            IOConnectCallScalarMethod(
                connection,
                Method.submitSBP2Inquiry.rawValue,
                buffer.baseAddress,
                UInt32(buffer.count),
                nil,
                nil)
        }

        guard kr == KERN_SUCCESS else {
            let errorMsg = "submitSBP2Inquiry failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return false
        }

        log(String(format: "SBP2 INQUIRY submitted (handle=0x%llX, allocLen=%u)", handle, allocationLength), level: .success)
        return true
    }

    /// Get the result of a completed INQUIRY command (destructive read).
    /// - Returns: Raw INQUIRY data, or nil if not ready.
    func getSBP2InquiryResult(handle: UInt64) -> Data? {
        guard isConnected else {
            log("getSBP2InquiryResult: Not connected", level: .warning)
            return nil
        }

        var scalars: [UInt64] = [handle]
        var outSize: Int = 256
        var out = Data(count: outSize)

        func doCall() -> kern_return_t {
            out.withUnsafeMutableBytes { outPtr in
                scalars.withUnsafeMutableBufferPointer { scalarPtr -> kern_return_t in
                    IOConnectCallMethod(
                        connection,
                        Method.getSBP2InquiryResult.rawValue,
                        scalarPtr.baseAddress,
                        UInt32(scalarPtr.count),
                        nil,
                        0,
                        nil,
                        nil,
                        outPtr.baseAddress,
                        &outSize)
                }
            }
        }

        var kr = doCall()
        if kr == kIOReturnNoSpace {
            out = Data(count: outSize)
            kr = doCall()
        }

        guard kr == KERN_SUCCESS else {
            return nil
        }

        out.count = outSize

        // Parse vendor/product from raw INQUIRY data for logging
        if out.count >= 36 {
            let vendor = String(data: out[8..<16], encoding: .ascii)?.trimmingCharacters(in: .controlCharacters) ?? "???"
            let product = String(data: out[16..<32], encoding: .ascii)?.trimmingCharacters(in: .controlCharacters) ?? "???"
            let revision = String(data: out[32..<36], encoding: .ascii)?.trimmingCharacters(in: .controlCharacters) ?? "???"
            log(String(format: "SBP2 INQUIRY result: %@ %@ (rev %@, %zu bytes)", vendor, product, revision, outSize), level: .success)
        }

        return out
    }

    /// Release an SBP-2 session.
    /// - Returns: true on success.
    @discardableResult
    func releaseSBP2Session(handle: UInt64) -> Bool {
        guard isConnected else {
            log("releaseSBP2Session: Not connected", level: .warning)
            return false
        }

        var inputs: [UInt64] = [handle]

        let kr = inputs.withUnsafeMutableBufferPointer { buffer -> kern_return_t in
            IOConnectCallScalarMethod(
                connection,
                Method.releaseSBP2Session.rawValue,
                buffer.baseAddress,
                UInt32(buffer.count),
                nil,
                nil)
        }

        guard kr == KERN_SUCCESS else {
            let errorMsg = "releaseSBP2Session failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return false
        }

        log(String(format: "SBP2 session released (handle=0x%llX)", handle), level: .success)
        return true
    }
}
