import Foundation

extension ASFWDriverConnector {
    func allocateSBP2AddressRange(addressHigh: UInt16,
                                  addressLow: UInt32,
                                  length: UInt32) -> UInt64? {
        let (status, outputs) = performSBP2ScalarCall(
            .allocateAddressRange,
            inputs: [UInt64(addressHigh), UInt64(addressLow), UInt64(length)],
            outputCount: 1
        )
        guard status == KERN_SUCCESS, let handle = outputs.first else {
            reportSBP2Failure("allocateSBP2AddressRange", status: status)
            return nil
        }
        return handle
    }

    @discardableResult
    func deallocateSBP2AddressRange(handle: UInt64) -> Bool {
        let (status, _) = performSBP2ScalarCall(
            .deallocateAddressRange,
            inputs: [handle]
        )
        guard status == KERN_SUCCESS else {
            reportSBP2Failure("deallocateSBP2AddressRange", status: status)
            return false
        }
        return true
    }

    func readSBP2AddressRange(handle: UInt64,
                              offset: UInt32,
                              length: UInt32) -> Data? {
        let call = SBP2DataCall(
            method: .readIncomingData,
            scalars: [handle, UInt64(offset), UInt64(length)],
            outputCapacity: Int(length)
        )
        let (status, output) = performSBP2DataCall(call)
        guard status == KERN_SUCCESS else {
            reportSBP2Failure("readSBP2AddressRange", status: status)
            return nil
        }
        return output
    }

    @discardableResult
    func writeSBP2AddressRange(handle: UInt64,
                               offset: UInt32,
                               data: Data) -> Bool {
        guard data.count <= Int(UInt32.max) else {
            let message = "writeSBP2AddressRange rejected: payload exceeds UInt32"
            lastError = message
            log(message, level: .error)
            return false
        }

        let call = SBP2DataCall(
            method: .writeLocalData,
            scalars: [handle, UInt64(offset), UInt64(data.count)],
            input: data
        )
        let (status, _) = performSBP2DataCall(call)
        guard status == KERN_SUCCESS else {
            reportSBP2Failure("writeSBP2AddressRange", status: status)
            return false
        }
        return true
    }
}
