//
//  DriverConnector+NodeMemory.swift
//  ASFW
//
//  Generic read-only node memory access shared by the device report tabs.
//
//  This is deliberately protocol-agnostic: it knows how to get bytes out of a
//  remote node's address space and nothing else. DICE reads register spaces at
//  0xFFFFE0000000, BeBoB reads the BridgeCo BootROM block at 0xFFFFC8020000 --
//  same primitive, different addresses.
//
//  Every read here is an ordinary async read against a live generation. There
//  are no writes on this path.
//

import Foundation

extension ASFWDriverConnector {

    /// Async block reads are bounded by the device's max_rec, so long sections
    /// are fetched in chunks. 512 bytes is safely below every device's
    /// advertised maximum.
    static let nodeReadChunkBytes = 512
    static let nodeReadTimeout: TimeInterval = 2.0
    /// A diagnostic must never use a remote device's advertised length as an
    /// unbounded allocation request. The sections we decode fit well within
    /// this limit; vendor application data is intentionally not read.
    static let maxNodeDiagnosticReadBytes = 64 * 1024

    /// One chunked block read. Returns nil if any chunk fails, so a partially
    /// read section is never mistaken for a short one.
    ///
    /// Falls back to quadlet reads if the device refuses a block read of this
    /// size. An unknown device is exactly the case these reports exist to
    /// describe, so slow-but-works beats fast-but-empty.
    func readDeviceBlock(deviceID: DeviceInstanceID,
                         base: UInt64,
                         offset: Int,
                         length: Int) -> Data? {
        guard offset >= 0, length > 0,
              length <= Self.maxNodeDiagnosticReadBytes,
              offset <= Int.max - length else { return nil }
        var out = Data()
        out.reserveCapacity(length)
        var done = 0
        while done < length {
            let chunk = min(Self.nodeReadChunkBytes, length - done)
            let addr = base &+ UInt64(offset + done)
            guard let part = deviceSyncRead(deviceID: deviceID,
                                          addressHigh: UInt16((addr >> 32) & 0xFFFF),
                                          addressLow: UInt32(addr & 0xFFFF_FFFF),
                                          length: UInt32(chunk)),
                  part.count >= chunk else {
                return readDeviceQuadlets(deviceID: deviceID, base: base,
                                          offset: offset, length: length)
            }
            out.append(part.prefix(chunk))
            done += chunk
        }
        return out
    }

    private func readDeviceQuadlets(deviceID: DeviceInstanceID,
                                    base: UInt64,
                                    offset: Int,
                                    length: Int) -> Data? {
        guard length > 0, length <= Self.maxNodeDiagnosticReadBytes,
              length.isMultiple(of: 4) else { return nil }
        var out = Data()
        out.reserveCapacity(length)
        var done = 0
        while done < length {
            let addr = base &+ UInt64(offset + done)
            guard let part = deviceSyncRead(deviceID: deviceID,
                                          addressHigh: UInt16((addr >> 32) & 0xFFFF),
                                          addressLow: UInt32(addr & 0xFFFF_FFFF),
                                          length: 4),
                  part.count >= 4 else {
                return nil
            }
            out.append(part.prefix(4))
            done += 4
        }
        return out
    }

    private func deviceSyncRead(deviceID: DeviceInstanceID,
                                addressHigh: UInt16,
                                addressLow: UInt32,
                                length: UInt32) -> Data? {
        guard let handle = asyncRead(deviceID: deviceID,
                                     addressHigh: addressHigh,
                                     addressLow: addressLow,
                                     length: length) else { return nil }
        let deadline = Date().addingTimeInterval(Self.nodeReadTimeout)
        while Date() < deadline {
            if let result = getTransactionResult(handle: handle,
                                                 initialPayloadCapacity: Int(length) + 128) {
                guard result.status == 0, result.responseCode == 0 else { return nil }
                return result.payload
            }
            Thread.sleep(forTimeInterval: 0.01)
        }
        return nil
    }
}
