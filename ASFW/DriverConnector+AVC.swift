import Foundation
import IOKit

extension ASFWDriverConnector {
    // MARK: - AVC Queries

    func getAVCUnits() -> [AVCUnitInfo]? {
        guard isConnected else {
            log("getAVCUnits: Not connected", level: .warning)
            return nil
        }

        // Initial capacity 4KB
        guard let data = callStruct(.getAVCUnits, initialCap: 4096) else {
            log("getAVCUnits: callStruct failed", level: .error)
            return nil
        }

        guard data.count >= 4 else {
            log("getAVCUnits: data too short", level: .error)
            return nil
        }

        // Helper to read UInt64 from unaligned offset
        func readUInt64(_ data: Data, at offset: Int) -> UInt64 {
            var value: UInt64 = 0
            for i in 0..<8 {
                value |= UInt64(data[offset + i]) << (i * 8)
            }
            return value
        }

        // Helper to read UInt32 from unaligned offset
        func readUInt32(_ data: Data, at offset: Int) -> UInt32 {
            var value: UInt32 = 0
            for i in 0..<4 {
                value |= UInt32(data[offset + i]) << (i * 8)
            }
            return value
        }

        // Helper to read UInt16 from unaligned offset
        func readUInt16(_ data: Data, at offset: Int) -> UInt16 {
            return UInt16(data[offset]) | (UInt16(data[offset + 1]) << 8)
        }

        let unitCount = readUInt32(data, at: 0)
        var offset = 4
        var units: [AVCUnitInfo] = []

        for _ in 0..<unitCount {
            // Check if we have enough data for AVCUnitInfoWire (20 bytes)
            if offset + 20 > data.count { break }

            let guid = readUInt64(data, at: offset)
            let nodeID = readUInt16(data, at: offset + 8)
            let vendorID = readUInt16(data, at: offset + 10)
            let modelID = readUInt16(data, at: offset + 12)
            let subunitCount = data[offset + 14]
            
            // Unit-level plug counts (from AVCUnitPlugInfoCommand)
            let isoInputPlugs = data[offset + 15]
            let isoOutputPlugs = data[offset + 16]
            let extInputPlugs = data[offset + 17]
            let extOutputPlugs = data[offset + 18]
            // _reserved at offset + 19

            offset += 20

            var subunits: [AVCSubunitInfo] = []
            for _ in 0..<subunitCount {
                if offset + 4 > data.count { break }

                let type = data[offset]
                let subID = data[offset + 1]
                let numSrc = data[offset + 2]
                let numDest = data[offset + 3]

                subunits.append(AVCSubunitInfo(type: type, subunitID: subID, numSrcPlugs: numSrc, numDestPlugs: numDest))
                offset += 4
            }

            units.append(AVCUnitInfo(
                guid: guid, 
                nodeID: nodeID, 
                vendorID: vendorID, 
                modelID: modelID, 
                subunits: subunits,
                isoInputPlugs: isoInputPlugs,
                isoOutputPlugs: isoOutputPlugs,
                extInputPlugs: extInputPlugs,
                extOutputPlugs: extOutputPlugs
            ))
        }

        return units
    }

    func getDriverVersion() -> DriverVersionInfo? {
        guard isConnected else {
            log("getDriverVersion: Not connected", level: .warning)
            return nil
        }

        // DriverVersionInfo is 280 bytes
        guard let data = callStruct(.getDriverVersion, initialCap: 280) else {
            log("getDriverVersion: callStruct failed", level: .error)
            return nil
        }

        guard let info = DriverVersionInfo(data: data) else {
            log("getDriverVersion: failed to decode data", level: .error)
            return nil
        }

        return info
    }

    func getSubunitCapabilities(guid: UInt64, type: UInt8, id: UInt8) -> AVCMusicCapabilities? {
        guard isConnected else { return nil }
        guard connection != 0 else { return nil }

        // Use scalar inputs (kernel expects 4 × UInt64 via scalarInput, not structureInput)
        let scalarInputs: [UInt64] = [
            guid >> 32,              // GUID high 32 bits
            guid & 0xFFFFFFFF,       // GUID low 32 bits
            UInt64(type),            // Subunit type
            UInt64(id)               // Subunit ID
        ]

        var outSize = 1024  // Initial capacity for output
        var out = Data(count: outSize)
        let scalarInputCount: UInt32 = 4

        let kr = out.withUnsafeMutableBytes { outPtr in
            scalarInputs.withUnsafeBufferPointer { scalarPtr in
                IOConnectCallMethod(
                    connection,
                    Method.getSubunitCapabilities.rawValue,
                    scalarPtr.baseAddress, scalarInputCount,  // Scalar inputs ✅
                    nil, 0,                                   // No struct input
                    nil, nil,                                 // No scalar output
                    outPtr.baseAddress?.assumingMemoryBound(to: UInt8.self), &outSize  // Struct output
                )
            }
        }

        guard kr == KERN_SUCCESS else {
            print("[Connector] ❌ callStruct error: getSubunitCapabilities failed: \(interpretIOReturn(kr))")
            return nil
        }

        out.count = outSize
        return AVCMusicCapabilities(data: out)
    }

    func getSubunitDescriptor(guid: UInt64, type: UInt8, id: UInt8) -> Data? {
        guard isConnected else { return nil }
        guard connection != 0 else { return nil }

        // Use scalar inputs (kernel expects 4 × UInt64 via scalarInput, not structureInput)
        let scalarInputs: [UInt64] = [
            guid >> 32,              // GUID high 32 bits
            guid & 0xFFFFFFFF,       // GUID low 32 bits
            UInt64(type),            // Subunit type
            UInt64(id)               // Subunit ID
        ]

        // DriverKit structure outputs over ~4KB get rejected; cap to match kMaxWireSize on the driver
        let maxWireSize = 4 * 1024
        var outSize = maxWireSize
        var out = Data(count: outSize)
        let scalarInputCount: UInt32 = 4

        let kr = out.withUnsafeMutableBytes { outPtr in
            scalarInputs.withUnsafeBufferPointer { scalarPtr in
                IOConnectCallMethod(
                    connection,
                    Method.getSubunitDescriptor.rawValue,
                    scalarPtr.baseAddress, scalarInputCount,  // Scalar inputs
                    nil, 0,                                   // No struct input
                    nil, nil,                                 // No scalar output
                    outPtr.baseAddress?.assumingMemoryBound(to: UInt8.self), &outSize  // Struct output
                )
            }
        }

        guard kr == KERN_SUCCESS else {
            print("[Connector] ❌ callStruct error: getSubunitDescriptor failed: \(interpretIOReturn(kr))")
            return nil
        }

        out.count = outSize
        return out
    }

    func reScanAVCUnits() -> Bool {
        guard isConnected else { return false }
        guard connection != 0 else { return false }

        let kr = IOConnectCallScalarMethod(
            connection,
            Method.reScanAVCUnits.rawValue,
            nil, 0,  // No inputs
            nil, nil // No outputs
        )

        if kr != KERN_SUCCESS {
            print("[Connector] ❌ reScanAVCUnits failed: \(interpretIOReturn(kr))")
            return false
        }

        print("[Connector] ✅ reScanAVCUnits triggered successfully")
        return true
    }
}
