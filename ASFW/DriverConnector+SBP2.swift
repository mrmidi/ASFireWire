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
}
