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
//  Ring layout is ABI with ASFWDriver/Protocols/DV/DVCaptureSink.hpp:
//    +0   magic 'ASDV' (0x41534456)
//    +4   version (u16) = 3
//    +6   negotiated channel (u8), connection mode (u8)
//    +8   numRecords (u32)
//    +12  recordBytes (u32) = 480
//    +16  dataOffsetBytes (u32) = 192
//    +20  session state (u32, driver-owned atomic)
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
    var sessionState: UInt32 = 0
    var channel: UInt8 = 0xFF
    var connectionMode: UInt8 = 0
    var packetsSeen: UInt32 = 0
    var dvSourcePackets: UInt32 = 0
    var nonDvPackets: UInt32 = 0
    var overruns: UInt32 = 0
    var lastRejectLen: UInt32 = 0
    var lastRejectQ0: UInt32 = 0
    var lastRejectQ1: UInt32 = 0
    var lastXferStatus: UInt32 = 0
    var dbcDiscontinuities: UInt32 = 0
    var invalidDifBlocks: UInt32 = 0
}

enum DVCaptureSessionState: UInt32 {
    case initializing = 0
    case active = 1
    case stopped = 2
    case busReset = 3
    case failed = 4
}

// MARK: - Mapped Ring

/// Consumer-side view of the shared DV ring. Single consumer only.
final class DVCaptureRing {
    static let magic: UInt32 = 0x41534456 // 'ASDV'
    static let version: UInt16 = 3
    static let headerBytes = 192
    static let recordBytes = 480

    private let base: UnsafeMutableRawPointer
    private let mappedAddress: mach_vm_address_t
    private let connection: io_connect_t
    private let numRecords: UInt32
    private let dataOffset: Int
    private var mapped = true

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
        let version = pointer.load(fromByteOffset: 4, as: UInt16.self)
        let numRecords = pointer.load(fromByteOffset: 8, as: UInt32.self)
        let recBytes = pointer.load(fromByteOffset: 12, as: UInt32.self)
        let dataOffset = pointer.load(fromByteOffset: 16, as: UInt32.self)

        guard magic == DVCaptureRing.magic,
              version == DVCaptureRing.version,
              recBytes == UInt32(DVCaptureRing.recordBytes),
              numRecords > 0,
              dataOffset == UInt32(DVCaptureRing.headerBytes),
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
        guard mapped else { return }
        mapped = false
        IOConnectUnmapMemory64(connection, 1, mach_task_self_, mappedAddress)
    }

    deinit { unmap() }

    /// Drain all available records, invoking the handler with each 480-byte chunk.
    /// Returns the number of records consumed.
    ///
    @discardableResult
    func drain(_ handler: (UnsafeRawBufferPointer) -> Void) -> Int {
        let writePointer = (base + 64).assumingMemoryBound(to: UInt32.self)
        let readPointer = (base + 128).assumingMemoryBound(to: UInt32.self)
        let w = ASFWAtomicLoadU32Acquire(writePointer)
        var r = ASFWAtomicLoadU32Acquire(readPointer)
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
            ASFWAtomicStoreU32Release(readPointer, r)
        }
        return consumed
    }

    var stats: DVCaptureStats {
        func atomicLoad(_ offset: Int) -> UInt32 {
            ASFWAtomicLoadU32Acquire(
                (base + offset).assumingMemoryBound(to: UInt32.self))
        }
        return DVCaptureStats(
            sessionState: atomicLoad(20),
            channel: base.load(fromByteOffset: 6, as: UInt8.self),
            connectionMode: base.load(fromByteOffset: 7, as: UInt8.self),
            packetsSeen: atomicLoad(24),
            dvSourcePackets: atomicLoad(28),
            nonDvPackets: atomicLoad(32),
            overruns: atomicLoad(36),
            lastRejectLen: atomicLoad(40),
            lastRejectQ0: atomicLoad(44),
            lastRejectQ1: atomicLoad(48),
            lastXferStatus: atomicLoad(52),
            dbcDiscontinuities: atomicLoad(56),
            invalidDifBlocks: atomicLoad(60)
        )
    }
}

// MARK: - Driver Connector Extension

extension ASFWDriverConnector {

    /// Start DV capture for a discovered device. The driver resolves its
    /// generation/node and negotiates CMP/IRM resources (with broadcast fallback).
    func startDVCapture(deviceID: DeviceInstanceID) -> Bool {
        guard isConnected, connection != 0 else { return false }

        var input: [UInt64] = [deviceID.rawValue]
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
        log("Started DV capture for device instance \(deviceID.rawValue)",
            level: .info)
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
