import Foundation
import IOKit

extension ASFWDriverConnector {
    // MARK: - Config ROM Operations

    struct ConfigROMFetchResult {
        let data: Data
        let requestedGeneration: UInt16
        let resolvedGeneration: UInt16

        var isExactGenerationMatch: Bool { requestedGeneration == resolvedGeneration }
    }

    func getConfigROM(nodeId: UInt8, generation: UInt16) -> ConfigROMFetchResult? {
        guard isConnected else {
            log("getConfigROM: Not connected", level: .warning)
            return nil
        }

        let input: [UInt64] = [UInt64(nodeId), UInt64(generation)]
        let maxSize = 1024 * 4  // Max 1024 quadlets
        var outputStruct = Data(count: maxSize)
        var outputSize = outputStruct.count
        var resolvedGeneration: UInt64 = UInt64(generation)
        var scalarOutputCount: UInt32 = 1

        let kr = outputStruct.withUnsafeMutableBytes { bufferPtr in
            IOConnectCallMethod(
                connection,
                Method.exportConfigROM.rawValue,
                input,
                UInt32(input.count),
                nil,
                0,
                &resolvedGeneration,
                &scalarOutputCount,
                bufferPtr.baseAddress,
                &outputSize
            )
        }

        guard kr == KERN_SUCCESS else {
            let errorMsg = "getConfigROM failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return nil
        }

        guard outputSize > 0 else {
            log("getConfigROM: ROM not cached for node=\(nodeId) gen=\(generation)", level: .info)
            return nil  // ROM not cached
        }

        let romData = outputStruct.prefix(outputSize)
        let resolvedGen16 = UInt16(truncatingIfNeeded: resolvedGeneration)
        if resolvedGen16 != generation {
            log("getConfigROM: received stale cache for node=\(nodeId) requestedGen=\(generation) resolvedGen=\(resolvedGen16) bytes=\(outputSize)", level: .warning)
        } else {
            log("getConfigROM: received \(outputSize) bytes for node=\(nodeId) gen=\(generation)", level: .success)
        }
        return ConfigROMFetchResult(data: Data(romData),
                                    requestedGeneration: generation,
                                    resolvedGeneration: resolvedGen16)
    }

    enum ROMReadStatus: UInt32 {
        case initiated = 0
        case alreadyInProgress = 1
        case failed = 2
    }

    func triggerROMRead(nodeId: UInt8) -> ROMReadStatus {
        guard isConnected else {
            log("triggerROMRead: Not connected", level: .warning)
            print("[Connector] ❌ triggerROMRead: Not connected")
            return .failed
        }

        var input: [UInt64] = [UInt64(nodeId)]
        var output = [UInt64](repeating: 0, count: 1)  // Use array like getBusResetCount
        var outputCount: UInt32 = 1

        print("[Connector] 📞 triggerROMRead: nodeId=\(nodeId) (0x\(String(nodeId, radix: 16))) connection=0x\(String(connection, radix: 16))")
        print("[Connector]    input=\(input) inputCount=\(input.count) selector=\(Method.triggerROMRead.rawValue)")

        let kr = input.withUnsafeMutableBufferPointer { buffer -> kern_return_t in
            IOConnectCallScalarMethod(
                connection,
                Method.triggerROMRead.rawValue,
                buffer.baseAddress,
                UInt32(buffer.count),
                &output,
                &outputCount
            )
        }

        print("[Connector]    IOKit result: kr=\(kr) (0x\(String(UInt32(bitPattern: kr), radix: 16))) output=\(output[0]) outputCount=\(outputCount)")

        guard kr == KERN_SUCCESS else {
            let errorMsg = "triggerROMRead failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            print("[Connector] ❌ triggerROMRead failed: \(errorMsg)")
            return .failed
        }

        let status = ROMReadStatus(rawValue: UInt32(output[0])) ?? .failed
        let statusText = status == .initiated ? "initiated" :
                        status == .alreadyInProgress ? "already in progress" : "failed"
        log("triggerROMRead: node=\(nodeId) \(statusText)", level: status == .failed ? .error : .success)
        print("[Connector] ✅ triggerROMRead: node=\(nodeId) status=\(statusText) (rawStatus=\(output[0]))")
        return status
    }
}
