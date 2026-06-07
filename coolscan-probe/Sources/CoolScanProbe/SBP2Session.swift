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
    /// Retries once: a target that still holds a prior exclusive login often
    /// accepts the second attempt after the first nudges it to drop/reconnect.
    static func login(_ conn: ASFWConnection, device: FWDevice, unit: FWUnit,
                      timeout: TimeInterval = 8.0, attempts: Int = 2) throws -> SBP2Session {
        var lastError: Error = ProbeError("login: ingen forsøk")
        for attempt in 1...max(1, attempts) {
            do { return try loginOnce(conn, device: device, unit: unit, timeout: timeout) }
            catch {
                lastError = error
                if attempt < attempts {
                    print("   ↻ login-forsøk \(attempt) feilet (\(error)); prøver igjen…")
                    usleep(1_500_000) // 1.5 s — la target rydde forrige login
                }
            }
        }
        throw lastError
    }

    private static func loginOnce(_ conn: ASFWConnection, device: FWDevice, unit: FWUnit,
                                  timeout: TimeInterval) throws -> SBP2Session {
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
                session.release() // free host-side session before bubbling up
                throw ProbeError("Login feilet (lastError=0x\(String(format: "%08x", state.lastError)))")
            default:
                usleep(100_000) // 100 ms
            }
        }
        let lastLabel = (try? session.state())?.login.label ?? "ukjent"
        session.release() // don't leak a half-open session on timeout
        throw ProbeError("Login timeout — siste state=\(lastLabel)")
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
            // Raw single call (no auto-retry) so we can see the exact kern_return
            // and the struct-output size the dext reports — needed to tell a
            // genuine BadArgument apart from a NoSpace→out-of-line marshalling issue.
            let (kr, reportedSize, data) = conn.callOnceStructOut(
                .getSBP2CommandResult, scalarIn: [handle], structOutCap: max(cap, 512))
            if kr == KERN_SUCCESS {
                return parseResult(data)
            }
            if kr == kIOReturnNotFound {
                usleep(50_000) // 50 ms — not ready yet
                continue
            }
            if kr == kIOReturnNoSpace {
                // Result bigger than our buffer — retry once with the reported size.
                let (kr2, size2, data2) = conn.callOnceStructOut(
                    .getSBP2CommandResult, scalarIn: [handle], structOutCap: max(reportedSize, cap))
                if kr2 == KERN_SUCCESS { return parseResult(data2) }
                throw ProbeError("getResult NoSpace→retry feilet: kr=0x\(String(format: "%08x", UInt32(bitPattern: kr2))) "
                    + "førsteReportedSize=\(reportedSize) retryReportedSize=\(size2) (struct for stor / out-of-line?)")
            }
            // Other error. Probe whether the dext is still alive.
            let alive: String
            do {
                let (out, _) = try conn.call(.getSBP2SessionState, scalarIn: [handle], scalarOutCount: 5)
                alive = "dext SVARER (state=\(out.first.map { ASFW.LoginState(raw: $0).label } ?? "?"))"
            } catch let live {
                alive = "dext SVARER IKKE (\(live))"
            }
            throw ProbeError("getResult kr=0x\(String(format: "%08x", UInt32(bitPattern: kr))) "
                + "reportedSize=\(reportedSize) (rått enkelt-kall, ingen retry) — \(alive)")
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

    /// Tear down the session. The dext issues an async LOGOUT ORB and retains the
    /// session until it completes/times out — but if our process exits the instant
    /// release() returns, the connection is torn down before the logout ORB is
    /// acked on the wire, leaving the target holding the exclusive login (which
    /// then forces a power-cycle before the next login). Hold the connection open
    /// briefly so the logout can land.
    func release() {
        _ = try? conn.call(.releaseSBP2Session, scalarIn: [handle])
        usleep(1_500_000) // 1.5 s — let the LOGOUT ORB complete before we exit
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
