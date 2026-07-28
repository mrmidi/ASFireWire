import Foundation

extension ASFWDriverConnector {
    func createSBP2Session(guid: UInt64, romOffset: UInt32) -> UInt64? {
        let (status, outputs) = performSBP2ScalarCall(
            .createSBP2Session,
            inputs: [
                UInt64(UInt32(truncatingIfNeeded: guid >> 32)),
                UInt64(UInt32(truncatingIfNeeded: guid)),
                UInt64(romOffset)
            ],
            outputCount: 1
        )
        guard status == KERN_SUCCESS, let handle = outputs.first else {
            reportSBP2Failure("createSBP2Session", status: status)
            return nil
        }
        return handle
    }

    @discardableResult
    func startSBP2Login(handle: UInt64) -> Bool {
        let (status, _) = performSBP2ScalarCall(.startSBP2Login, inputs: [handle])
        guard status == KERN_SUCCESS else {
            reportSBP2Failure("startSBP2Login", status: status)
            return false
        }
        return true
    }

    func getSBP2SessionState(handle: UInt64) -> SBP2SessionState? {
        let (status, values) = performSBP2ScalarCall(
            .getSBP2SessionState,
            inputs: [handle],
            outputCount: 5
        )
        guard status == KERN_SUCCESS, values.count == 5,
              let loginState = SBP2LoginState(rawValue: UInt8(truncatingIfNeeded: values[0])) else {
            if status != kIOReturnNotFound {
                reportSBP2Failure("getSBP2SessionState", status: status)
            }
            return nil
        }

        return SBP2SessionState(
            loginState: loginState,
            loginID: UInt16(truncatingIfNeeded: values[1]),
            generation: UInt16(truncatingIfNeeded: values[2]),
            lastError: Int32(bitPattern: UInt32(truncatingIfNeeded: values[3])),
            reconnectPending: values[4] != 0
        )
    }

    @discardableResult
    func submitSBP2Inquiry(handle: UInt64, allocationLength: UInt8 = 96) -> Bool {
        let (status, _) = performSBP2ScalarCall(
            .submitSBP2Inquiry,
            inputs: [handle, UInt64(allocationLength)]
        )
        guard status == KERN_SUCCESS else {
            reportSBP2Failure("submitSBP2Inquiry", status: status)
            return false
        }
        return true
    }

    func getSBP2InquiryResult(handle: UInt64,
                              allocationLength: UInt8 = 96) -> Data? {
        let call = SBP2DataCall(
            method: .getSBP2InquiryResult,
            scalars: [handle],
            outputCapacity: Int(allocationLength)
        )
        let (status, output) = performSBP2DataCall(call)
        guard status == KERN_SUCCESS else {
            if status != kIOReturnNotFound {
                reportSBP2Failure("getSBP2InquiryResult", status: status)
            }
            return nil
        }
        return output
    }

    @discardableResult
    func submitSBP2TaskManagement(handle: UInt64,
                                  function: SBP2TaskManagementFunction) -> Bool {
        let (status, _) = performSBP2ScalarCall(
            .submitSBP2TaskManagement,
            inputs: [handle, function.rawValue]
        )
        guard status == KERN_SUCCESS else {
            reportSBP2Failure("submitSBP2TaskManagement", status: status)
            return false
        }
        return true
    }

    @discardableResult
    func releaseSBP2Session(handle: UInt64) -> Bool {
        let (status, _) = performSBP2ScalarCall(.releaseSBP2Session, inputs: [handle])
        guard status == KERN_SUCCESS else {
            reportSBP2Failure("releaseSBP2Session", status: status)
            return false
        }
        return true
    }
}
