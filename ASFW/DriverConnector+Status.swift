import Foundation
import IOKit

extension ASFWDriverConnector {
    // MARK: - Status & Metrics

    func getBusResetCount() -> (count: UInt64, generation: UInt8, timestamp: UInt64)? {
        guard isConnected else {
            log("getBusResetCount: Not connected", level: .warning)
            return nil
        }

        var output = [UInt64](repeating: 0, count: 3)
        var outputCount: UInt32 = 3

        let kr = IOConnectCallScalarMethod(
            connection,
            Method.getBusResetCount.rawValue,
            nil,
            0,
            &output,
            &outputCount
        )

        guard kr == KERN_SUCCESS else {
            let errorMsg = "getBusResetCount failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return nil
        }

        return (output[0], UInt8(output[1]), output[2])
    }

    func getControllerStatus() -> ControllerStatus? {
        guard isConnected else { return nil }

        var outputStruct = Data(count: MemoryLayout<ControllerStatusWire>.size)
        var outputSize = outputStruct.count

        let kr = outputStruct.withUnsafeMutableBytes { bufferPtr in
            IOConnectCallStructMethod(
                connection,
                Method.getControllerStatus.rawValue,
                nil,
                0,
                bufferPtr.baseAddress,
                &outputSize
            )
        }

        guard kr == KERN_SUCCESS else {
            lastError = "getControllerStatus failed: \(interpretIOReturn(kr))"
            return nil
        }

        return outputStruct.withUnsafeBytes { ptr in
            let wire = ptr.load(as: ControllerStatusWire.self)
            return ControllerStatus(wire: wire)
        }
    }

    func getBusResetHistory(startIndex: UInt64 = 0, count: UInt64 = 10) -> [BusResetPacketSnapshot]? {
        guard connection != 0 else { return nil }

        var input = Data()
        withUnsafeBytes(of: startIndex.littleEndian) { input.append(contentsOf: $0) }
        withUnsafeBytes(of: count.littleEndian) { input.append(contentsOf: $0) }

        guard let bytes = callStruct(.getBusResetHistory, input: input, initialCap: 4096) else { return nil }

        let packetSize = MemoryLayout<BusResetPacketWire>.size
        guard !bytes.isEmpty, bytes.count % packetSize == 0 else { return [] }

        return bytes.withUnsafeBytes { ptr in
            let n = bytes.count / packetSize
            return (0..<n).map { i in
                BusResetPacketSnapshot(wire: ptr.load(fromByteOffset: i * packetSize, as: BusResetPacketWire.self))
            }
        }
    }

    func clearHistory() -> Bool {
        guard connection != 0 else { return false }

        let kr = IOConnectCallScalarMethod(
            connection,
            Method.clearHistory.rawValue,
            nil,
            0,
            nil,
            nil
        )

        guard kr == KERN_SUCCESS else {
            lastError = "clearHistory failed: \(interpretIOReturn(kr))"
            return false
        }

        return true
    }

    func getSelfIDCapture(generation: UInt32? = nil) -> SelfIDCapture? {
        guard connection != 0 else {
            print("[Connector] ❌ getSelfIDCapture: not connected")
            return nil
        }

        print("[Connector] 📞 Calling getSelfIDCapture via IOConnectCallStructMethod")
        guard let bytes = callStruct(.getSelfIDCapture, initialCap: 1024) else {
            print("[Connector] ❌ getSelfIDCapture: callStruct failed")
            return nil
        }

        print("[Connector] 📦 getSelfIDCapture: received \(bytes.count) bytes")
        guard !bytes.isEmpty else {
            print("[Connector] ⚠️  getSelfIDCapture: 0 bytes returned (no data from driver)")
            return nil
        }

        print("[Connector] 🔍 Decoding \(bytes.count) bytes...")
        let result = SelfIDCapture.decode(from: bytes)
        print("[Connector] Decode result: \(result != nil ? "✅ SUCCESS" : "❌ FAILED")")
        return result
    }

    func getTopologySnapshot(generation: UInt32? = nil) -> TopologySnapshot? {
        guard connection != 0 else {
            log("❌ getTopologySnapshot: not connected", level: .error)
            return nil
        }

        guard let bytes = callStruct(.getTopologySnapshot, initialCap: 4096) else {
            log("❌ getTopologySnapshot: callStruct failed", level: .error)
            return nil
        }

        guard !bytes.isEmpty else {
            log("⚠️  getTopologySnapshot: driver returned 0 bytes (no topology)", level: .warning)
            return nil
        }

        log("✅ getTopologySnapshot: received \(bytes.count) bytes from driver", level: .success)
        let result = TopologySnapshot.decode(from: bytes)
        if result == nil {
            log("❌ getTopologySnapshot: decode returned nil!", level: .error)
        }
        return result
    }

    func ping() -> String? {
        guard isConnected else {
            log("ping: Not connected", level: .warning)
            return nil
        }

        var output = Data(count: 128)
        var outputSize = output.count

        let kr = output.withUnsafeMutableBytes { bufferPtr in
            IOConnectCallStructMethod(
                connection,
                Method.ping.rawValue,
                nil,
                0,
                bufferPtr.baseAddress,
                &outputSize
            )
        }

        guard kr == KERN_SUCCESS else {
            let errorMsg = "ping failed: \(interpretIOReturn(kr))"
            log(errorMsg, level: .error)
            lastError = errorMsg
            return nil
        }

        guard outputSize > 0 else {
            log("ping: driver returned empty payload", level: .warning)
            return nil
        }

        let messageData = output.prefix(outputSize)
        let message: String? = messageData.withUnsafeBytes { buffer in
            let ptr = buffer.bindMemory(to: CChar.self).baseAddress
            if let cString = ptr {
                return String(cString: cString)
            }
            return nil
        }

        if let message = message {
            log("ping response: \(message)", level: .success)
        }
        return message
    }
}
