//
//  DriverConnector+DVCapture.swift
//  ASFW
//
//  DV (IEC 61883-2) capture control and shared DIF ring access.
//
//  The driver fills a shared-memory SPSC ring with raw 480-byte DIF chunks
//  (one DV source packet each). This file maps the ring (memory type 1) and
//  consumes records; the view layer concatenates them into a .dv file.
//
//  Ring layout is ABI with ASFWDriver/Isoch/Receive/DVCaptureSink.hpp:
//    +0   magic 'ASDV' (0x41534456)
//    +4   version (u16) = 1
//    +8   numRecords (u32)
//    +12  recordBytes (u32) = 480
//    +16  dataOffsetBytes (u32) = 192
//    +24  packetsSeen (u32)
//    +28  dvSourcePackets (u32)
//    +32  nonDvPackets (u32)
//    +36  overruns (u32)
//    +64  writeIndex (u32, driver-owned, free-running records)
//    +128 readIndex (u32, app-owned, free-running records)
//    +192 record data (numRecords × 480 bytes)
//

import Foundation
import IOKit

// MARK: - DV Ring Stats

struct DVCaptureStats {
    var packetsSeen: UInt32 = 0
    var dvSourcePackets: UInt32 = 0
    var nonDvPackets: UInt32 = 0
    var overruns: UInt32 = 0
    var lastRejectLen: UInt32 = 0
    var lastRejectQ0: UInt32 = 0
    var lastRejectQ1: UInt32 = 0
    var lastXferStatus: UInt32 = 0
}

// MARK: - Mapped Ring

/// Consumer-side view of the shared DV ring. Single consumer only.
final class DVCaptureRing {
    static let magic: UInt32 = 0x41534456 // 'ASDV'
    static let recordBytes = 480

    private let base: UnsafeMutableRawPointer
    private let mappedAddress: mach_vm_address_t
    private let connection: io_connect_t
    private let numRecords: UInt32
    private let dataOffset: Int

    private init(base: UnsafeMutableRawPointer,
                 mappedAddress: mach_vm_address_t,
                 connection: io_connect_t,
                 numRecords: UInt32,
                 dataOffset: Int) {
        self.base = base
        self.mappedAddress = mappedAddress
        self.connection = connection
        self.numRecords = numRecords
        self.dataOffset = dataOffset
    }

    static func map(connection: io_connect_t) -> DVCaptureRing? {
        var address: mach_vm_address_t = 0
        var length: mach_vm_size_t = 0
        let options = UInt32(kIOMapAnywhere | kIOMapDefaultCache)
        let kr = IOConnectMapMemory64(connection, 1, mach_task_self_, &address, &length, options)
        guard kr == KERN_SUCCESS, let pointer = UnsafeMutableRawPointer(bitPattern: UInt(address)) else {
            return nil
        }

        let magic = pointer.load(fromByteOffset: 0, as: UInt32.self)
        let numRecords = pointer.load(fromByteOffset: 8, as: UInt32.self)
        let recBytes = pointer.load(fromByteOffset: 12, as: UInt32.self)
        let dataOffset = pointer.load(fromByteOffset: 16, as: UInt32.self)

        guard magic == DVCaptureRing.magic,
              recBytes == UInt32(DVCaptureRing.recordBytes),
              numRecords > 0,
              UInt64(dataOffset) + UInt64(numRecords) * UInt64(recBytes) <= UInt64(length) else {
            IOConnectUnmapMemory64(connection, 1, mach_task_self_, address)
            return nil
        }

        return DVCaptureRing(base: pointer,
                             mappedAddress: address,
                             connection: connection,
                             numRecords: numRecords,
                             dataOffset: Int(dataOffset))
    }

    func unmap() {
        IOConnectUnmapMemory64(connection, 1, mach_task_self_, mappedAddress)
    }

    /// Drain all available records, invoking the handler with each 480-byte chunk.
    /// Returns the number of records consumed.
    ///
    /// Note on ordering: the driver release-stores writeIndex after the memcpy.
    /// We poll at tens of milliseconds, so observed records were published long
    /// before we read them; explicit acquire barriers are intentionally omitted.
    @discardableResult
    func drain(_ handler: (UnsafeRawBufferPointer) -> Void) -> Int {
        let w = base.load(fromByteOffset: 64, as: UInt32.self)
        var r = base.load(fromByteOffset: 128, as: UInt32.self)
        var consumed = 0

        while r != w {
            let idx = Int(r % numRecords)
            let chunk = UnsafeRawBufferPointer(
                start: base + dataOffset + idx * DVCaptureRing.recordBytes,
                count: DVCaptureRing.recordBytes
            )
            handler(chunk)
            r &+= 1
            consumed += 1
        }

        if consumed > 0 {
            base.storeBytes(of: r, toByteOffset: 128, as: UInt32.self)
        }
        return consumed
    }

    var stats: DVCaptureStats {
        DVCaptureStats(
            packetsSeen: base.load(fromByteOffset: 24, as: UInt32.self),
            dvSourcePackets: base.load(fromByteOffset: 28, as: UInt32.self),
            nonDvPackets: base.load(fromByteOffset: 32, as: UInt32.self),
            overruns: base.load(fromByteOffset: 36, as: UInt32.self),
            lastRejectLen: base.load(fromByteOffset: 40, as: UInt32.self),
            lastRejectQ0: base.load(fromByteOffset: 44, as: UInt32.self),
            lastRejectQ1: base.load(fromByteOffset: 48, as: UInt32.self),
            lastXferStatus: base.load(fromByteOffset: 52, as: UInt32.self)
        )
    }
}

// MARK: - Driver Connector Extension

extension ASFWDriverConnector {

    /// Start DV capture on the given isoch channel (camcorders broadcast on 63).
    func startDVCapture(channel: UInt8) -> Bool {
        guard isConnected, connection != 0 else { return false }

        var input: [UInt64] = [UInt64(channel)]
        let kr = IOConnectCallScalarMethod(
            connection,
            Method.startDVCapture.rawValue,
            &input, 1,
            nil, nil
        )

        if kr != KERN_SUCCESS {
            log("startDVCapture failed: \(interpretIOReturn(kr))", level: .error)
            return false
        }
        log("Started DV capture on channel \(channel)", level: .info)
        return true
    }

    /// Stop DV capture.
    func stopDVCapture() -> Bool {
        guard isConnected, connection != 0 else { return false }

        let kr = IOConnectCallScalarMethod(
            connection,
            Method.stopDVCapture.rawValue,
            nil, 0,
            nil, nil
        )

        if kr != KERN_SUCCESS {
            log("stopDVCapture failed: \(interpretIOReturn(kr))", level: .error)
            return false
        }
        log("Stopped DV capture", level: .info)
        return true
    }

    /// Map the shared DV ring. Call after startDVCapture succeeds.
    func mapDVCaptureRing() -> DVCaptureRing? {
        guard isConnected, connection != 0 else { return nil }
        guard let ring = DVCaptureRing.map(connection: connection) else {
            log("mapDVCaptureRing: mapping failed", level: .error)
            return nil
        }
        log("Mapped DV capture ring", level: .info)
        return ring
    }
}
