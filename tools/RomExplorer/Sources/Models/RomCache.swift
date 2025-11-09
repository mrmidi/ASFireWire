import Foundation

// Absolute quadlet-addressed ROM cache (Apple-style)
struct RomCache {
    private let data: Data // big-endian normalized

    init(fileData: Data) throws {
        self.data = try RomCache.makeBigEndian(data: fileData)
        guard data.count >= 8 else { throw RomError.invalidData("ROM too short") }
    }

    var byteCount: Int { data.count }

    var quadletCount: Int { data.count / 4 }

    func quadlet(at q: Int) throws -> UInt32 {
        // Validate index in quadlet-space first to avoid potential Int overflow on q * 4
        guard q >= 0 && q < quadletCount else {
            DebugLog.error("quadlet OOB @\(q) (quadlets=\(quadletCount), off=\(q &* 4), bytes=\(data.count))")
            throw RomError.invalidData("quadlet OOB @\(q)")
        }
        let off = q &* 4
        return data.withUnsafeBytes { $0.load(fromByteOffset: off, as: UInt32.self).bigEndian }
    }

    func readBytes(quadletStart q: Int, quadletLength lq: Int) throws -> Data {
        // Validate in quadlet-space to avoid potential Int overflow on q * 4 or lq * 4
        guard q >= 0, lq >= 0 else {
            DebugLog.error("slice negative params q=\(q), lq=\(lq)")
            throw RomError.invalidData("slice negative params")
        }
        // Allow start at last quadlet when requesting zero length
        guard q <= quadletCount else {
            DebugLog.error("slice OOB start @\(q) (quadlets=\(quadletCount))")
            throw RomError.invalidData("slice OOB start @\(q)")
        }
        let off = q &* 4
        let len = lq &* 4
        // Clamp end to available bytes to mirror tolerant legacy behavior
        let end = min(off &+ len, data.count)
        if end < off &+ len { DebugLog.warn("slice clamped q=\(q) lq=\(lq) off=\(off) len=\(len) end=\(end) bytes=\(data.count)") }
        return data.subdata(in: off..<end)
    }

    // Header/meta in quadlet 0
    var busInfoLengthQ: Int {
        let meta = data.withUnsafeBytes { $0.load(as: UInt32.self).bigEndian }
        return Int((meta & 0xff00_0000) >> 24)
    }

    var rootDirectoryStartQ: Int { 1 + busInfoLengthQ }

    func busInfoRaw() throws -> Data {
        let off = 4
        let len = busInfoLengthQ * 4
        guard off + len <= data.count else { throw RomError.invalidData("BIB OOB") }
        return data.subdata(in: off..<(off + len))
    }

    private static func makeBigEndian(data: Data) throws -> Data {
        guard data.count >= 8 else { throw RomError.invalidData("ROM too short") }
        let marker = data.withUnsafeBytes { $0.load(fromByteOffset: 4, as: UInt32.self).bigEndian }
        if marker == 0x34393331 { // '4931' -> LE quadlets
            var out = Data(); out.reserveCapacity(data.count)
            var i = 0
            while i < data.count {
                let v: UInt32 = data.withUnsafeBytes { $0.load(fromByteOffset: i, as: UInt32.self).littleEndian }
                var be = v.bigEndian
                withUnsafeBytes(of: &be) { out.append(contentsOf: $0) }
                i += 4
            }
            return out
        }
        return data
    }
}

