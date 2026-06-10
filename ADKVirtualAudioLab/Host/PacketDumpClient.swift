import Foundation
import IOKit

// User-client bridge to the lab dext's diagnostic interface and the parser
// for its PacketDumpBlob. Layout constants mirror Lab/PacketDumpBlob.hpp —
// keep the two in sync (native little-endian, same-host only).

enum PacketDumpWire {
    static let magic: UInt32 = 0x4C44_4D50 // 'LDMP'
    static let version: UInt32 = 1
    static let headerSize = 128
    static let recordMetaSize = 64
    static let packetBytesSize = 512
    static let recordSize = recordMetaSize + packetBytesSize // 576
    static let maxRecords: UInt32 = 6

    static let userClientType: UInt32 = 0x4C44_4247 // 'LDBG'
    static let selectorDumpPackets: UInt32 = 0
    static let anchorLatest: UInt64 = .max
    static let anchorPayload: UInt64 = .max - 1

    static let flagIsData: UInt32 = 1 << 0
    static let flagPublished: UInt32 = 1 << 1
    static let flagTimelineLive: UInt32 = 1 << 2
    static let flagLiveBytes: UInt32 = 1 << 3
}

struct PacketDumpHeader {
    var recordCount: UInt32 = 0
    var hostTimeTicks: UInt64 = 0
    var periodIndex: UInt64 = 0
    var ztsPeriodFrames: UInt32 = 0
    var ioRunning: Bool = false
    var exposedFrames: UInt64 = 0
    var nextPacketIndex: UInt64 = 0
    var prepareFailures: UInt64 = 0
    var writeEndCount: UInt64 = 0
    var framesVisited: UInt64 = 0
    var framesWritten: UInt64 = 0
    var framesWithoutPacket: UInt64 = 0
    var framesOutsidePacket: UInt64 = 0
    var framesRacedReuse: UInt64 = 0
    var expectedNextSampleTime: UInt64 = 0
    var expectedSampleTimeValid: Bool = false

    var ztsSampleTime: UInt64 { periodIndex * UInt64(ztsPeriodFrames) }
}

struct CIPHeader {
    let q0: UInt32
    let q1: UInt32

    var sid: UInt32 { (q0 >> 24) & 0x3F }
    var dbs: UInt32 { (q0 >> 16) & 0xFF }
    var fn: UInt32 { (q0 >> 14) & 0x3 }
    var qpc: UInt32 { (q0 >> 11) & 0x7 }
    var sph: UInt32 { (q0 >> 10) & 0x1 }
    var dbc: UInt32 { q0 & 0xFF }
    var fmt: UInt32 { (q1 >> 24) & 0x3F }
    var fdf: UInt32 { (q1 >> 16) & 0xFF }
    var syt: UInt32 { q1 & 0xFFFF }
    var sytIsNoInfo: Bool { syt == 0xFFFF }
}

struct PacketDumpRecord: Identifiable {
    var packetIndex: UInt64 = 0
    var firstAudioFrame: UInt64 = 0
    var flags: UInt32 = 0
    var byteCount: UInt32 = 0
    var framesInPacket: UInt32 = 0
    var dbs: UInt32 = 0
    var slotState: UInt32 = 0xFFFF_FFFF
    var generation: UInt32 = 0
    var bytes = Data()

    var id: UInt64 { packetIndex }
    var isData: Bool { flags & PacketDumpWire.flagIsData != 0 }
    var isPublished: Bool { flags & PacketDumpWire.flagPublished != 0 }
    var isTimelineLive: Bool { flags & PacketDumpWire.flagTimelineLive != 0 }
    var hasLiveBytes: Bool { flags & PacketDumpWire.flagLiveBytes != 0 }

    var slotStateName: String {
        switch slotState {
        case 0: return "Empty"
        case 1: return "ExposedForAudio"
        case 2: return "Published"
        case 3: return "Completed"
        case 0xFFFF_FFFF: return "evicted"
        default: return "state(\(slotState))"
        }
    }

    /// Big-endian wire word at the given quadlet index.
    func word(_ index: Int) -> UInt32? {
        let offset = index * 4
        guard offset + 4 <= bytes.count, offset + 4 <= Int(byteCount) else {
            return nil
        }
        return (UInt32(bytes[offset]) << 24) | (UInt32(bytes[offset + 1]) << 16)
            | (UInt32(bytes[offset + 2]) << 8) | UInt32(bytes[offset + 3])
    }

    var cip: CIPHeader? {
        guard let q0 = word(0), let q1 = word(1) else { return nil }
        return CIPHeader(q0: q0, q1: q1)
    }

    /// Audio slot word for (frame, slot) in the payload after the CIP header.
    func slotWord(frame: Int, slot: Int) -> UInt32? {
        word(2 + frame * Int(dbs) + slot)
    }

    /// Decode a slot word as the lab's raw signed-24-in-32 PCM (Saffire TX
    /// quirk). AM824-labeled words decode the same low 24 bits.
    static func pcmValue(_ word: UInt32) -> Float {
        let raw = Int32(bitPattern: word << 8) >> 8 // sign-extend [23:0]
        return Float(raw) / 8_388_607.0
    }
}

struct PacketDump {
    var header = PacketDumpHeader()
    var records: [PacketDumpRecord] = []
}

enum PacketDumpClientError: LocalizedError {
    case serviceNotFound
    case openFailed(kern_return_t)
    case callFailed(kern_return_t)
    case malformedBlob(String)

    var errorDescription: String? {
        switch self {
        case .serviceNotFound:
            return "Dext service not found — is the extension activated?"
        case .openFailed(let kr):
            return String(format: "IOServiceOpen failed (0x%08x) — check userclient-access entitlement.", kr)
        case .callFailed(let kr):
            return String(format: "Dump call failed (0x%08x).", kr)
        case .malformedBlob(let why):
            return "Malformed dump blob: \(why)"
        }
    }
}

final class PacketDumpClient {
    private var connection: io_connect_t = IO_OBJECT_NULL

    deinit { close() }

    func close() {
        if connection != IO_OBJECT_NULL {
            IOServiceClose(connection)
            connection = IO_OBJECT_NULL
        }
    }

    private func findService() -> io_service_t {
        // DriverKit services register under their IOUserClass name.
        let byName = IOServiceGetMatchingService(
            kIOMainPortDefault, IOServiceNameMatching("VirtualAudioDriver"))
        if byName != IO_OBJECT_NULL { return byName }

        // Fallback: scan IOUserService instances for our server name.
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(
            kIOMainPortDefault, IOServiceMatching("IOUserService"),
            &iterator) == KERN_SUCCESS else { return IO_OBJECT_NULL }
        defer { IOObjectRelease(iterator) }

        while case let service = IOIteratorNext(iterator), service != IO_OBJECT_NULL {
            if let name = IORegistryEntryCreateCFProperty(
                service, "IOUserServerName" as CFString, kCFAllocatorDefault, 0)?
                .takeRetainedValue() as? String,
                name.contains("ADKVirtualAudioLab") {
                return service
            }
            IOObjectRelease(service)
        }
        return IO_OBJECT_NULL
    }

    private func ensureConnection() throws {
        guard connection == IO_OBJECT_NULL else { return }
        let service = findService()
        guard service != IO_OBJECT_NULL else {
            throw PacketDumpClientError.serviceNotFound
        }
        defer { IOObjectRelease(service) }
        let kr = IOServiceOpen(service, mach_task_self_,
                               PacketDumpWire.userClientType, &connection)
        guard kr == KERN_SUCCESS, connection != IO_OBJECT_NULL else {
            connection = IO_OBJECT_NULL
            throw PacketDumpClientError.openFailed(kr)
        }
    }

    func dump(count: UInt32 = 4,
              anchor: UInt64 = PacketDumpWire.anchorLatest) throws -> PacketDump {
        try ensureConnection()
        defer { close() }

        let clamped = min(max(count, 1), PacketDumpWire.maxRecords)
        var scalars: [UInt64] = [UInt64(clamped), anchor]
        let capacity = PacketDumpWire.headerSize
            + Int(clamped) * PacketDumpWire.recordSize
        var blob = Data(count: capacity)
        var blobSize = capacity

        let kr = blob.withUnsafeMutableBytes { raw -> kern_return_t in
            IOConnectCallMethod(connection,
                                PacketDumpWire.selectorDumpPackets,
                                &scalars, 2,
                                nil, 0,
                                nil, nil,
                                raw.baseAddress, &blobSize)
        }
        guard kr == KERN_SUCCESS else {
            // A dropped connection (dext replaced) needs a reopen next time.
            if kr == kIOReturnNotOpen || kr == kIOReturnNoDevice { close() }
            throw PacketDumpClientError.callFailed(kr)
        }
        blob.removeSubrange(blobSize..<blob.count)
        return try Self.parse(blob)
    }

    static func parse(_ blob: Data) throws -> PacketDump {
        guard blob.count >= PacketDumpWire.headerSize else {
            throw PacketDumpClientError.malformedBlob("short header (\(blob.count) B)")
        }
        var dump = PacketDump()
        try blob.withUnsafeBytes { raw in
            func u32(_ offset: Int) -> UInt32 { raw.loadUnaligned(fromByteOffset: offset, as: UInt32.self) }
            func u64(_ offset: Int) -> UInt64 { raw.loadUnaligned(fromByteOffset: offset, as: UInt64.self) }

            guard u32(0) == PacketDumpWire.magic else {
                throw PacketDumpClientError.malformedBlob("bad magic")
            }
            guard u32(4) == PacketDumpWire.version else {
                throw PacketDumpClientError.malformedBlob("version \(u32(4))")
            }
            let recordCount = Int(u32(8))
            let stride = Int(u32(12))
            guard stride == PacketDumpWire.recordSize else {
                throw PacketDumpClientError.malformedBlob("stride \(stride)")
            }
            guard blob.count >= PacketDumpWire.headerSize + recordCount * stride else {
                throw PacketDumpClientError.malformedBlob("truncated records")
            }

            dump.header.recordCount = u32(8)
            dump.header.hostTimeTicks = u64(16)
            dump.header.periodIndex = u64(24)
            dump.header.ztsPeriodFrames = u32(32)
            dump.header.ioRunning = u32(36) != 0
            dump.header.exposedFrames = u64(40)
            dump.header.nextPacketIndex = u64(48)
            dump.header.prepareFailures = u64(56)
            dump.header.writeEndCount = u64(64)
            dump.header.framesVisited = u64(72)
            dump.header.framesWritten = u64(80)
            dump.header.framesWithoutPacket = u64(88)
            dump.header.framesOutsidePacket = u64(96)
            dump.header.framesRacedReuse = u64(104)
            dump.header.expectedNextSampleTime = u64(112)
            dump.header.expectedSampleTimeValid = u32(120) != 0

            for i in 0..<recordCount {
                let base = PacketDumpWire.headerSize + i * stride
                var record = PacketDumpRecord()
                record.packetIndex = u64(base)
                record.firstAudioFrame = u64(base + 8)
                record.flags = u32(base + 16)
                record.byteCount = u32(base + 20)
                record.framesInPacket = u32(base + 24)
                record.dbs = u32(base + 28)
                record.slotState = u32(base + 32)
                record.generation = u32(base + 36)
                let payloadStart = base + PacketDumpWire.recordMetaSize
                record.bytes = blob.subdata(
                    in: payloadStart..<(payloadStart + PacketDumpWire.packetBytesSize))
                dump.records.append(record)
            }
        }
        return dump
    }
}

// IOKit doesn't surface these in Swift.
private let kIOReturnNotOpen: kern_return_t = -536870195 // 0xE00002CD
private let kIOReturnNoDevice: kern_return_t = -536870208 // 0xE00002C0
