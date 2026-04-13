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

    func setIsochVerbosity(_ level: UInt32) -> Bool {
        guard isConnected else {
            log("setIsochVerbosity: Not connected", level: .warning)
            return false
        }

        var input: UInt64 = UInt64(level)
        let kr = IOConnectCallScalarMethod(
            connection,
            ASFWDriverConnector.Method.setIsochVerbosity.rawValue,
            &input,
            1,
            nil,
            nil
        )

        guard kr == KERN_SUCCESS else {
            log("setIsochVerbosity failed: \(interpretIOReturn(kr))", level: .error)
            return false
        }

        log("Isoch verbosity set to \(level)", level: .success)
        return true
    }

    func setIsochTelemetryLogging(enabled: Bool) -> Bool {
        // Telemetry logs are gated at verbosity >= 3.
        setIsochVerbosity(enabled ? 3 : 1)
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

    func setIsochTxVerifier(enabled: Bool) -> Bool {
        guard isConnected else {
            log("setIsochTxVerifier: Not connected", level: .warning)
            return false
        }

        var input: UInt64 = enabled ? 1 : 0
        let kr = IOConnectCallScalarMethod(
            connection,
            ASFWDriverConnector.Method.setIsochTxVerifier.rawValue,
            &input,
            1,
            nil,
            nil
        )

        guard kr == KERN_SUCCESS else {
            log("setIsochTxVerifier failed: \(interpretIOReturn(kr))", level: .error)
            return false
        }

        log("Isoch TX verifier \(enabled ? "enabled" : "disabled")", level: .success)
        return true
    }

    func getLogConfig() -> (asyncVerbosity: UInt32, hexDumpsEnabled: Bool, isochVerbosity: UInt32, isochTxVerifierEnabled: Bool)? {
        guard isConnected else {
            log("getLogConfig: Not connected", level: .warning)
            return nil
        }

        var output = [UInt64](repeating: 0, count: 4)
        var outputCount: UInt32 = 4

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

        let isochVerbosity = outputCount >= 3 ? UInt32(output[2]) : 1
        let isochTxVerifierEnabled = outputCount >= 4 ? (output[3] != 0) : false
        return (UInt32(output[0]), output[1] != 0, isochVerbosity, isochTxVerifierEnabled)
    }
}
