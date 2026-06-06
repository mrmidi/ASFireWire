import Foundation

/// Result of a SCSI command sent through the SBP-2 passthrough channel.
struct SCSIResult {
    let transportStatus: Int32   // 0 == transport OK
    let sbpStatus: UInt8         // 0 == kNoAdditionalInfo
    let payload: Data            // data read from target
    let sense: Data              // sense bytes (if CHECK CONDITION + captureSense)

    var ok: Bool { transportStatus == 0 && sbpStatus == 0 }
}

/// A logged-in SBP-2 session against one target, exposing raw SCSI passthrough.
/// This is the reusable transport the eventual CoolScan driver builds on:
/// every scanner operation is just a vendor SCSI CDB sent through `sendSCSI`.
final class SBP2Session {
    private let conn: ASFWConnection
    let handle: UInt64
    let device: FWDevice

    private init(conn: ASFWConnection, handle: UInt64, device: FWDevice) {
        self.conn = conn; self.handle = handle; self.device = device
    }

    /// Create a session and log in, polling state until LoggedIn / Failed / timeout.
    static func login(_ conn: ASFWConnection, device: FWDevice, unit: FWUnit,
                      timeout: TimeInterval = 8.0) throws -> SBP2Session {
        let guidHi = UInt64(device.guid >> 32)
        let guidLo = UInt64(device.guid & 0xFFFF_FFFF)
        let (out, _) = try conn.call(.createSBP2Session,
                                     scalarIn: [guidHi, guidLo, UInt64(unit.romOffset)],
                                     scalarOutCount: 1)
        guard let handle = out.first else { throw ProbeError("CreateSBP2Session ga ingen handle") }
        let session = SBP2Session(conn: conn, handle: handle, device: device)

        try conn.call(.startSBP2Login, scalarIn: [handle])

        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            let state = try session.state()
            switch state.login {
            case .loggedIn:
                return session
            case .failed:
                throw ProbeError("Login feilet (lastError=0x\(String(format: "%08x", state.lastError)))")
            default:
                usleep(100_000) // 100 ms
            }
        }
        let s = try session.state()
        throw ProbeError("Login timeout — siste state=\(s.login.label)")
    }

    struct State { let login: ASFW.LoginState; let loginID: UInt64; let generation: UInt64; let lastError: UInt32; let reconnectPending: Bool }

    func state() throws -> State {
        let (out, _) = try conn.call(.getSBP2SessionState, scalarIn: [handle], scalarOutCount: 5)
        guard out.count >= 5 else { throw ProbeError("GetSBP2SessionState ga for få verdier") }
        return State(login: ASFW.LoginState(raw: out[0]), loginID: out[1],
                     generation: out[2], lastError: UInt32(truncatingIfNeeded: out[3]),
                     reconnectPending: out[4] != 0)
    }

    /// Send an arbitrary SCSI CDB and wait for the result.
    func sendSCSI(cdb: [UInt8],
                  direction: ASFW.DataDirection,
                  transferLength: UInt32 = 0,
                  outgoing: [UInt8] = [],
                  timeoutMs: UInt32 = 5000,
                  captureSense: Bool = true) throws -> SCSIResult {
        precondition(cdb.count >= 1 && cdb.count <= 16, "CDB må være 1–16 byte")

        // Build host-endian SBP2CommandRequestWire (20 bytes) + CDB + outgoing payload.
        var req = Data()
        req.appendLE(UInt32(cdb.count))        // cdbLength
        req.appendLE(transferLength)           // transferLength
        req.appendLE(UInt32(outgoing.count))   // outgoingLength
        req.appendLE(timeoutMs)                // timeoutMs
        req.append(direction.rawValue)         // direction
        req.append(captureSense ? 1 : 0)       // captureSenseData
        req.append(0); req.append(0)           // _reserved[2]
        req.append(contentsOf: cdb)
        req.append(contentsOf: outgoing)

        try conn.call(.submitSBP2Command, scalarIn: [handle], structIn: req)

        // Poll for the result (GetSBP2CommandResult returns NotFound until ready).
        let cap = Int(transferLength) + 256 // payload + result header + sense headroom
        let deadline = Date().addingTimeInterval(Double(timeoutMs) / 1000.0 + 2.0)
        while Date() < deadline {
            do {
                let (_, data) = try conn.call(.getSBP2CommandResult, scalarIn: [handle],
                                              structOutCap: max(cap, 512))
                return parseResult(data)
            } catch let e as ProbeError where e.description.contains("Not Found") {
                usleep(50_000) // 50 ms — not ready yet
            }
        }
        throw ProbeError("SCSI-kommando timeout (ingen resultat)")
    }

    private func parseResult(_ data: Data) -> SCSIResult {
        guard data.count >= 16 else {
            return SCSIResult(transportStatus: -1, sbpStatus: 0xFF, payload: Data(), sense: Data())
        }
        let transportStatus = data.i32(0)
        let sbpStatus = data.u8(4)
        let payloadLen = Int(data.u32(8))
        let senseLen = Int(data.u32(12))
        var off = 16
        let payloadEnd = min(off + payloadLen, data.count)
        let payload = data.subdata(in: data.startIndex + off ..< data.startIndex + payloadEnd)
        off = payloadEnd
        let sense: Data = (off + senseLen <= data.count)
            ? data.subdata(in: data.startIndex + off ..< data.startIndex + off + senseLen)
            : Data()
        return SCSIResult(transportStatus: transportStatus, sbpStatus: sbpStatus,
                          payload: payload, sense: sense)
    }

    func release() {
        _ = try? conn.call(.releaseSBP2Session, scalarIn: [handle])
    }
}

// MARK: - Standard SCSI helpers (the start of the CoolScan command set)

enum SCSI {
    /// INQUIRY (0x12) — standard inquiry data, identifies vendor/product.
    static func inquiry(_ s: SBP2Session, allocation: UInt8 = 96) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0x12, 0x00, 0x00, 0x00, allocation, 0x00],
                       direction: .fromTarget, transferLength: UInt32(allocation))
    }

    /// TEST UNIT READY (0x00) — is the device ready / spun up?
    static func testUnitReady(_ s: SBP2Session) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0x00, 0x00, 0x00, 0x00, 0x00, 0x00], direction: .none)
    }

    /// Decode the vendor / product / revision fields from standard INQUIRY data.
    static func describeInquiry(_ d: Data) -> String {
        guard d.count >= 36 else { return "INQUIRY-data for kort (\(d.count) byte)" }
        let pdt = d.u8(0) & 0x1F
        let vendor = d.cString(8, 8).trimmingCharacters(in: .whitespaces)
        let product = d.cString(16, 16).trimmingCharacters(in: .whitespaces)
        let revision = d.cString(32, 4).trimmingCharacters(in: .whitespaces)
        return "PDT=0x\(String(format: "%02x", pdt))  vendor=«\(vendor)»  product=«\(product)»  rev=«\(revision)»"
    }
}
