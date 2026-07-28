import Foundation

extension ASFWDriverConnector {
    @discardableResult
    func submitSBP2Command(handle: UInt64, request: SBP2CommandRequest) -> Bool {
        let encoded: Data
        do {
            encoded = try SBP2CommandWireCodec.encode(request)
        } catch {
            let message = "submitSBP2Command rejected: \(error)"
            lastError = message
            log(message, level: .error)
            return false
        }

        let call = SBP2DataCall(
            method: .submitSBP2Command,
            scalars: [handle],
            input: encoded
        )
        let (status, _) = performSBP2DataCall(call)
        guard status == KERN_SUCCESS else {
            reportSBP2Failure("submitSBP2Command", status: status)
            return false
        }
        return true
    }

    func getSBP2CommandResult(handle: UInt64,
                              request: SBP2CommandRequest) -> SBP2CommandResult? {
        let outputCapacity: Int
        do {
            outputCapacity = try SBP2CommandWireCodec.resultCapacity(for: request)
        } catch {
            let message = "getSBP2CommandResult rejected: \(error)"
            lastError = message
            log(message, level: .error)
            return nil
        }

        let call = SBP2DataCall(
            method: .getSBP2CommandResult,
            scalars: [handle],
            outputCapacity: outputCapacity
        )
        let (status, output) = performSBP2DataCall(call)
        guard status == KERN_SUCCESS else {
            if status != kIOReturnNotFound {
                reportSBP2Failure("getSBP2CommandResult", status: status)
            }
            return nil
        }

        do {
            return try SBP2CommandWireCodec.decodeResult(output)
        } catch {
            let message = "getSBP2CommandResult returned malformed data: \(error)"
            lastError = message
            log(message, level: .error)
            return nil
        }
    }
}
