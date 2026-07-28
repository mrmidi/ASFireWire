import Foundation
import IOKit

struct SBP2DataCall {
    let method: ASFWDriverConnector.Method
    let scalars: [UInt64]
    let input: Data
    let outputCapacity: Int

    init(method: ASFWDriverConnector.Method,
         scalars: [UInt64],
         input: Data = Data(),
         outputCapacity: Int = 0) {
        self.method = method
        self.scalars = scalars
        self.input = input
        self.outputCapacity = outputCapacity
    }
}

extension ASFWDriverConnector {
    func performSBP2ScalarCall(_ method: Method,
                               inputs: [UInt64],
                               outputCount: Int = 0) -> (kern_return_t, [UInt64]) {
        guard connection != 0 else {
            return (kIOReturnNotReady, [])
        }
        guard outputCount >= 0,
              inputs.count <= Int(UInt32.max),
              outputCount <= Int(UInt32.max) else {
            return (kIOReturnBadArgument, [])
        }

        var mutableInputs = inputs
        var outputs = [UInt64](repeating: 0, count: outputCount)
        var mutableOutputCount = UInt32(outputCount)
        let status = mutableInputs.withUnsafeMutableBufferPointer { inputBuffer in
            outputs.withUnsafeMutableBufferPointer { outputBuffer in
                IOConnectCallScalarMethod(
                    connection,
                    method.rawValue,
                    inputBuffer.baseAddress,
                    UInt32(inputBuffer.count),
                    outputBuffer.baseAddress,
                    &mutableOutputCount
                )
            }
        }

        guard status == KERN_SUCCESS else {
            return (status, [])
        }
        guard mutableOutputCount == UInt32(outputs.count) else {
            return (kIOReturnNoSpace, [])
        }
        return (status, outputs)
    }

    func performSBP2DataCall(_ call: SBP2DataCall) -> (kern_return_t, Data) {
        guard connection != 0 else {
            return (kIOReturnNotReady, Data())
        }
        guard call.outputCapacity >= 0,
              call.scalars.count <= Int(UInt32.max) else {
            return (kIOReturnBadArgument, Data())
        }

        var scalars = call.scalars
        var output = Data(count: call.outputCapacity)
        var outputSize = output.count
        let status = output.withUnsafeMutableBytes { outputBytes in
            call.input.withUnsafeBytes { inputBytes in
                scalars.withUnsafeMutableBufferPointer { scalarBuffer in
                    IOConnectCallMethod(
                        connection,
                        call.method.rawValue,
                        scalarBuffer.baseAddress,
                        UInt32(scalarBuffer.count),
                        inputBytes.baseAddress,
                        call.input.count,
                        nil,
                        nil,
                        outputBytes.baseAddress,
                        &outputSize
                    )
                }
            }
        }

        guard status == KERN_SUCCESS else {
            return (status, Data())
        }
        guard outputSize <= output.count else {
            return (kIOReturnNoSpace, Data())
        }
        output.count = outputSize
        return (status, output)
    }

    func reportSBP2Failure(_ action: String, status: kern_return_t) {
        let message = "\(action) failed: \(interpretIOReturn(status))"
        lastError = message
        log(message, level: .error)
    }
}
