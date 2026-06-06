import Foundation

/// Little-endian readers over a Data slice (the dext sends host-endian records).
extension Data {
    func u8(_ off: Int) -> UInt8 { self[startIndex + off] }
    func u32(_ off: Int) -> UInt32 {
        var v: UInt32 = 0
        for i in 0..<4 { v |= UInt32(self[startIndex + off + i]) << (i * 8) }
        return v
    }
    func u64(_ off: Int) -> UInt64 {
        var v: UInt64 = 0
        for i in 0..<8 { v |= UInt64(self[startIndex + off + i]) << (i * 8) }
        return v
    }
    func i32(_ off: Int) -> Int32 { Int32(bitPattern: u32(off)) }
    func cString(_ off: Int, _ len: Int) -> String {
        let bytes = self[startIndex + off ..< startIndex + off + len].prefix { $0 != 0 }
        return String(decoding: bytes, as: UTF8.self)
    }

    mutating func appendLE(_ v: UInt32) {
        for i in 0..<4 { append(UInt8((v >> (i * 8)) & 0xFF)) }
    }
}

/// A FireWire unit (one logical function of a device).
struct FWUnit {
    let specId: UInt32
    let swVersion: UInt32
    let romOffset: UInt32
    let lun: UInt32
    let vendorName: String
    let productName: String

    var isSBP2: Bool { specId == ASFW.sbp2SpecId && swVersion == ASFW.sbp2SwVersion }
}

/// A FireWire device (a node on the bus) and its units.
struct FWDevice {
    let guid: UInt64
    let vendorId: UInt32
    let modelId: UInt32
    let nodeId: UInt8
    let vendorName: String
    let modelName: String
    let units: [FWUnit]

    var sbp2Units: [FWUnit] { units.filter(\.isSBP2) }
    var guidString: String { String(format: "0x%016llx", guid) }
}

enum Discovery {
    private static let deviceWireSize = 152
    private static let unitWireSize = 160

    /// Calls selector 16 and decodes the device/unit tree.
    static func enumerate(_ conn: ASFWConnection) throws -> [FWDevice] {
        let (_, data) = try conn.call(.getDiscoveredDevices, structOutCap: 4096)
        guard data.count >= 8 else { return [] }

        let deviceCount = Int(data.u32(0))
        var off = 8 // deviceCount(4) + padding(4)
        var devices: [FWDevice] = []

        for _ in 0..<deviceCount {
            guard off + deviceWireSize <= data.count else {
                throw ProbeError("Avkortet device-data fra dext")
            }
            let guid = data.u64(off)
            let vendorId = data.u32(off + 8)
            let modelId = data.u32(off + 12)
            let nodeId = data.u8(off + 20)
            let unitCount = Int(data.u8(off + 22))
            let vendorName = data.cString(off + 24, 64)
            let modelName = data.cString(off + 88, 64)
            off += deviceWireSize

            var units: [FWUnit] = []
            for _ in 0..<unitCount {
                guard off + unitWireSize <= data.count else {
                    throw ProbeError("Avkortet unit-data fra dext")
                }
                units.append(FWUnit(
                    specId: data.u32(off),
                    swVersion: data.u32(off + 4),
                    romOffset: data.u32(off + 8),
                    lun: data.u32(off + 20),
                    vendorName: data.cString(off + 32, 64),
                    productName: data.cString(off + 96, 64)))
                off += unitWireSize
            }

            devices.append(FWDevice(
                guid: guid, vendorId: vendorId, modelId: modelId, nodeId: nodeId,
                vendorName: vendorName, modelName: modelName, units: units))
        }
        return devices
    }
}
