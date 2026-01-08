import Foundation
import IOKit

extension ASFWDriverConnector {
    // MARK: - Legacy command helpers

    func asyncRead(destinationID: UInt16,
                   addressHigh: UInt16,
                   addressLow: UInt32,
                   length: UInt32) -> UInt16? {
        guard isConnected else {
            log("asyncRead: Not connected", level: .warning)
            return nil
        }

        var inputs: [UInt64] = [
            UInt64(destinationID),
            UInt64(addressHigh),
            UInt64(addressLow),
            UInt64(length)
        ]

        var output: UInt64 = 0
        var outputCount: UInt32 = 1

        let kr = inputs.withUnsafeMutableBufferPointer { buffer -> kern_return_t in
            IOConnectCallScalarMethod(
                connection,
                Method.asyncRead.rawValue,
                buffer.baseAddress,
                UInt32(buffer.count),
                &output,
                &outputCount)
        }

        guard kr == KERN_SUCCESS else {
            let errorMsg = "asyncRead failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return nil
        }

        let handle = UInt16(truncatingIfNeeded: output)
        log(String(format: "AsyncRead issued (handle=0x%04X)", handle), level: .success)
        return handle
    }

    func asyncWrite(destinationID: UInt16,
                    addressHigh: UInt16,
                    addressLow: UInt32,
                    payload: Data) -> UInt16? {
        guard isConnected else {
            log("asyncWrite: Not connected", level: .warning)
            return nil
        }

        var scalars: [UInt64] = [
            UInt64(destinationID),
            UInt64(addressHigh),
            UInt64(addressLow),
            UInt64(payload.count)
        ]

        var output: UInt64 = 0
        var outputCount: UInt32 = 1

        let kr = payload.withUnsafeBytes { payloadPtr in
            scalars.withUnsafeMutableBufferPointer { scalarPtr -> kern_return_t in
                IOConnectCallMethod(
                    connection,
                    Method.asyncWrite.rawValue,
                    scalarPtr.baseAddress,
                    UInt32(scalarPtr.count),
                    payloadPtr.baseAddress,
                    payload.count,
                    &output,
                    &outputCount,
                    nil,
                    nil)
            }
        }

        guard kr == KERN_SUCCESS else {
            let errorMsg = "asyncWrite failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return nil
        }

        let handle = UInt16(truncatingIfNeeded: output)
        log(String(format: "AsyncWrite issued (handle=0x%04X, bytes=%u)", handle, payload.count), level: .success)
        return handle
    }

    func asyncCompareSwap(
        destinationID: UInt16,
        addressHigh: UInt16,
        addressLow: UInt32,
        compareValue: Data,
        newValue: Data
    ) -> (handle: UInt16?, locked: Bool)? {
        guard isConnected else {
            log("asyncCompareSwap: Not connected", level: .warning)
            return nil
        }

        // Validate size (must be 4 or 8 bytes)
        guard compareValue.count == newValue.count else {
            log("asyncCompareSwap: compareValue and newValue size mismatch", level: .error)
            lastError = "Compare and new values must be the same size"
            return nil
        }

        guard compareValue.count == 4 || compareValue.count == 8 else {
            log("asyncCompareSwap: Invalid size (must be 4 or 8 bytes)", level: .error)
            lastError = "Size must be 4 (32-bit) or 8 (64-bit) bytes"
            return nil
        }

        let size = UInt8(compareValue.count)

        // Build operand: compareValue + newValue concatenated
        var operand = Data()
        operand.append(compareValue)
        operand.append(newValue)

        var scalars: [UInt64] = [
            UInt64(destinationID),
            UInt64(addressHigh),
            UInt64(addressLow),
            UInt64(size)
        ]

        var outputs: [UInt64] = [0, 0]  // handle, locked
        var outputCount: UInt32 = 2

        let kr = operand.withUnsafeBytes { operandPtr in
            scalars.withUnsafeMutableBufferPointer { scalarPtr -> kern_return_t in
                IOConnectCallMethod(
                    connection,
                    Method.asyncCompareSwap.rawValue,
                    scalarPtr.baseAddress,
                    UInt32(scalarPtr.count),
                    operandPtr.baseAddress,
                    operand.count,
                    &outputs,
                    &outputCount,
                    nil,
                    nil)
            }
        }

        guard kr == KERN_SUCCESS else {
            let errorMsg = "asyncCompareSwap failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return nil
        }

        let handle = UInt16(truncatingIfNeeded: outputs[0])
        let locked = outputs[1] != 0

        log(String(format: "AsyncCompareSwap issued (handle=0x%04X, size=%u)", handle, size), level: .success)
        return (handle, locked)
    }
}
