import Foundation
import IOKit

/// Thin wrapper around IOConnectCall* patterns with kIOReturn decoding and size retry.
final class DriverConnectorTransport {
    typealias ConnectionProvider = () -> io_connect_t
    typealias ErrorHandler = (String) -> Void
    typealias IOReturnInterpreter = (kern_return_t) -> String

    private let connectionProvider: ConnectionProvider
    private let errorHandler: ErrorHandler
    private let interpretIOReturn: IOReturnInterpreter

    init(connectionProvider: @escaping ConnectionProvider,
         interpretIOReturn: @escaping IOReturnInterpreter,
         errorHandler: @escaping ErrorHandler) {
        self.connectionProvider = connectionProvider
        self.errorHandler = errorHandler
        self.interpretIOReturn = interpretIOReturn
    }

    func callStruct(selector: UInt32, input: Data? = nil, initialCap: Int = 64 * 1024) -> Data? {
        let connection = connectionProvider()
        guard connection != 0 else {
            errorHandler("Not connected to driver")
            print("[Connector] ❌ callStruct: connection=0 (not connected)")
            return nil
        }

        print("[Connector] 📞 callStruct: selector=\(selector) connection=0x\(String(connection, radix: 16)) initialCap=\(initialCap)")

        var outSize = initialCap
        var out = Data(count: outSize)

        func doCall() -> kern_return_t {
            out.withUnsafeMutableBytes { outPtr in
                if let input = input {
                    return input.withUnsafeBytes { inPtr in
                        IOConnectCallStructMethod(connection, selector,
                                                  inPtr.baseAddress, input.count,
                                                  outPtr.baseAddress, &outSize)
                    }
                } else {
                    return IOConnectCallStructMethod(connection, selector,
                                                     nil, 0,
                                                     outPtr.baseAddress, &outSize)
                }
            }
        }

        var kr = doCall()
        if kr == kIOReturnNoSpace {
            print("[Connector] callStruct: got kIOReturnNoSpace, retrying with size=\(outSize)")
            out = Data(count: outSize)
            kr = doCall()
        }

        guard kr == KERN_SUCCESS else {
            let errMsg = "selector \(selector) failed: \(interpretIOReturn(kr))"
            print("[Connector] ❌ callStruct error: \(errMsg)")
            errorHandler(errMsg)
            return nil
        }

        print("[Connector] ✅ callStruct success: selector=\(selector) outSize=\(outSize)")
        out.count = outSize
        return out
    }

    func callStructWithScalar(selector: UInt32,
                              input: Data? = nil,
                              initialCap: Int = 64 * 1024,
                              scalarOutput: inout UInt64) -> Data? {
        let connection = connectionProvider()
        guard connection != 0 else {
            errorHandler("Not connected to driver")
            print("[Connector] ❌ callStructWithScalar: connection=0 (not connected)")
            return nil
        }

        print("[Connector] 📞 callStructWithScalar: selector=\(selector) connection=0x\(String(connection, radix: 16)) initialCap=\(initialCap)")

        var outSize = initialCap
        var out = Data(count: outSize)
        var scalarOutputCount: UInt32 = 1

        func doCall() -> kern_return_t {
            out.withUnsafeMutableBytes { outPtr in
                if let input = input {
                    return input.withUnsafeBytes { inPtr in
                        IOConnectCallMethod(connection, selector,
                                            nil, 0,  // No scalar input
                                            inPtr.baseAddress, input.count,
                                            &scalarOutput, &scalarOutputCount,
                                            outPtr.baseAddress, &outSize)
                    }
                } else {
                    return IOConnectCallMethod(connection, selector,
                                               nil, 0,  // No scalar input
                                               nil, 0,  // No struct input
                                               &scalarOutput, &scalarOutputCount,
                                               outPtr.baseAddress, &outSize)
                }
            }
        }

        var kr = doCall()
        if kr == kIOReturnNoSpace {
            print("[Connector] callStructWithScalar: got kIOReturnNoSpace, retrying with size=\(outSize)")
            out = Data(count: outSize)
            kr = doCall()
        }

        guard kr == KERN_SUCCESS else {
            let errMsg = "selector \(selector) failed: \(interpretIOReturn(kr))"
            print("[Connector] ❌ callStructWithScalar error: \(errMsg)")
            errorHandler(errMsg)
            return nil
        }

        print("[Connector] ✅ callStructWithScalar success: selector=\(selector) outSize=\(outSize) scalarOut=\(scalarOutput)")
        out.count = outSize
        return out
    }
}
