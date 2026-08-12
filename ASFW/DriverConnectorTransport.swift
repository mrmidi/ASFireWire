import Foundation
import IOKit

/// Thin wrapper around IOConnectCall* patterns with kIOReturn decoding and size retry.
final class DriverConnectorTransport {
    /// IOKit copies a struct-output reply inline only up to this size. Ask for
    /// more and the reply comes back over the descriptor path instead, which the
    /// driver does not populate — the call "succeeds" while `outSize` is never
    /// narrowed to the real length, so any parser validating a declared size
    /// against `data.count` rejects the whole payload and reports nothing.
    ///
    /// Measured 2026-08-12: every request at or below this size worked (log ring
    /// 4096, diagnostics, self-ID 1024, driver version 280) and every request
    /// above it returned empty while the driver logged correct data — discovery
    /// 16384 (driver sent 394 bytes for one device), AV/C units 16384 (84 bytes),
    /// audio telemetry 8192 (no endpoints during live playback).
    static let maxInlineStructOutputBytes = 4096

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

    func callStruct(selector: UInt32,
                    input: Data? = nil,
                    initialCap: Int = maxInlineStructOutputBytes,
                    traceCalls: Bool = true) -> Data? {
        let connection = connectionProvider()
        guard connection != 0 else {
            errorHandler("Not connected to driver")
            if traceCalls {
                print("[Connector] ❌ callStruct: connection=0 (not connected)")
            }
            return nil
        }

        if traceCalls {
            print("[Connector] 📞 callStruct: selector=\(selector) connection=0x\(String(connection, radix: 16)) initialCap=\(initialCap)")
        }

        // Clamping here rather than at each call site: a caller asking for more
        // than IOKit will return inline gets silence, not an error.
        var outSize = min(initialCap, Self.maxInlineStructOutputBytes)
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
            // The driver needs more room. Growing past the inline limit would
            // put us back on the silent descriptor path, so fail loudly instead:
            // the payload has outgrown this transport and needs paginating the
            // way the log ring already does.
            guard outSize <= Self.maxInlineStructOutputBytes else {
                let errMsg = "selector \(selector) needs \(outSize) bytes, above the "
                    + "\(Self.maxInlineStructOutputBytes)-byte inline struct-output limit; "
                    + "this reply must be paginated"
                if traceCalls {
                    print("[Connector] callStruct: \(errMsg)")
                }
                errorHandler(errMsg)
                return nil
            }
            if traceCalls {
                print("[Connector] callStruct: got kIOReturnNoSpace, retrying with size=\(outSize)")
            }
            out = Data(count: outSize)
            kr = doCall()
        }

        guard kr == KERN_SUCCESS else {
            let errMsg = "selector \(selector) failed: \(interpretIOReturn(kr))"
            if traceCalls {
                print("[Connector] ❌ callStruct error: \(errMsg)")
            }
            errorHandler(errMsg)
            return nil
        }

        if traceCalls {
            print("[Connector] ✅ callStruct success: selector=\(selector) outSize=\(outSize)")
        }
        out.count = outSize
        return out
    }

    func callStructWithScalar(selector: UInt32,
                              input: Data? = nil,
                              initialCap: Int = maxInlineStructOutputBytes,
                              scalarOutput: inout UInt64) -> Data? {
        let connection = connectionProvider()
        guard connection != 0 else {
            errorHandler("Not connected to driver")
            print("[Connector] ❌ callStructWithScalar: connection=0 (not connected)")
            return nil
        }

        print("[Connector] 📞 callStructWithScalar: selector=\(selector) connection=0x\(String(connection, radix: 16)) initialCap=\(initialCap)")

        // Same inline limit as `callStruct` — see `maxInlineStructOutputBytes`.
        var outSize = min(initialCap, Self.maxInlineStructOutputBytes)
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
            guard outSize <= Self.maxInlineStructOutputBytes else {
                let errMsg = "selector \(selector) needs \(outSize) bytes, above the "
                    + "\(Self.maxInlineStructOutputBytes)-byte inline struct-output limit; "
                    + "this reply must be paginated"
                print("[Connector] callStructWithScalar: \(errMsg)")
                errorHandler(errMsg)
                return nil
            }
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
