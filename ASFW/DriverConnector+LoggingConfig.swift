import Foundation
import IOKit

extension ASFWDriverConnector {
    // MARK: - Logging Configuration

    func setAsyncVerbosity(_ level: UInt32) -> Bool {
        guard isConnected else {
            log("setAsyncVerbosity: Not connected", level: .warning)
            return false
        }

        var input: UInt64 = UInt64(level)
        let kr = IOConnectCallScalarMethod(
            connection,
            ASFWDriverConnector.Method.setAsyncVerbosity.rawValue,
            &input,
            1,
            nil,
            nil
        )

        guard kr == KERN_SUCCESS else {
            log("setAsyncVerbosity failed: \(interpretIOReturn(kr))", level: .error)
            return false
        }

        log("Async verbosity set to \(level)", level: .success)
        return true
    }

    func setHexDumps(enabled: Bool) -> Bool {
        guard isConnected else {
            log("setHexDumps: Not connected", level: .warning)
            return false
        }

        var input: UInt64 = enabled ? 1 : 0
        let kr = IOConnectCallScalarMethod(
            connection,
            ASFWDriverConnector.Method.setHexDumps.rawValue,
            &input,
            1,
            nil,
            nil
        )

        guard kr == KERN_SUCCESS else {
            log("setHexDumps failed: \(interpretIOReturn(kr))", level: .error)
            return false
        }

        log("Hex dumps \(enabled ? "enabled" : "disabled")", level: .success)
        return true
    }

    func getLogConfig() -> (asyncVerbosity: UInt32, hexDumpsEnabled: Bool)? {
        guard isConnected else {
            log("getLogConfig: Not connected", level: .warning)
            return nil
        }

        var output = [UInt64](repeating: 0, count: 2)
        var outputCount: UInt32 = 2

        let kr = IOConnectCallScalarMethod(
            connection,
            ASFWDriverConnector.Method.getLogConfig.rawValue,
            nil,
            0,
            &output,
            &outputCount
        )

        guard kr == KERN_SUCCESS else {
            log("getLogConfig failed: \(interpretIOReturn(kr))", level: .error)
            return nil
        }

        return (UInt32(output[0]), output[1] != 0)
    }
}
