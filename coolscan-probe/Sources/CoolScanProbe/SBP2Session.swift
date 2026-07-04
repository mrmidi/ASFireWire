import Foundation

/// Result of a SCSI command sent through the SBP-2 passthrough channel.
struct SCSIResult {
    let transportStatus: Int32   // 0 == transport OK
    let sbpStatus: UInt8         // 0 == kNoAdditionalInfo
    /// SAM status from the SBP-2 status block (dext v14+). nil = target sent no
    /// command-set-dependent status (normal on GOOD) or old dext. 0x02 = CHECK
    /// CONDITION — before v14 this was masked and looked like GOOD.
    let scsiStatus: UInt8?
    let payload: Data            // data read from target
    let sense: Data              // decoded autosense (CHECK CONDITION) or captureSense copy

    var ok: Bool { transportStatus == 0 && sbpStatus == 0 && (scsiStatus ?? 0) == 0 }

    /// Human-readable status for diagnostics: SCSI status + sense key/ASC/ASCQ.
    var statusText: String {
        var parts = ["transport=\(transportStatus)", "sbp=\(sbpStatus)"]
        if let s = scsiStatus {
            parts.append("scsi=0x" + String(format: "%02x", s))
            if s == 0x02, sense.count >= 14 {
                let key = sense[sense.startIndex + 2] & 0x0F
                let asc = sense[sense.startIndex + 12]
                let ascq = sense[sense.startIndex + 13]
                parts.append(String(format: "sense=%x/%02x/%02x", key, asc, ascq))
            }
        }
        return parts.joined(separator: " ")
    }
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
        var lastError: Error = ProbeError("login: no attempts")
        for attempt in 1...max(1, attempts) {
            do { return try loginOnce(conn, device: device, unit: unit, timeout: timeout) }
            catch {
                lastError = error
                if attempt < attempts {
                    print("   ↻ login attempt \(attempt) failed (\(error)); retrying…")
                    usleep(1_500_000) // 1.5 s — let target clean up previous login
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
        guard let handle = out.first else { throw ProbeError("CreateSBP2Session returned no handle") }
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
                throw ProbeError("Login failed (lastError=0x\(String(format: "%08x", state.lastError)))")
            default:
                usleep(100_000) // 100 ms
            }
        }
        let lastLabel = (try? session.state())?.login.label ?? "unknown"
        session.release() // don't leak a half-open session on timeout
        throw ProbeError("Login timeout — last state=\(lastLabel)")
    }

    struct State { let login: ASFW.LoginState; let loginID: UInt64; let generation: UInt64; let lastError: UInt32; let reconnectPending: Bool }

    func state() throws -> State {
        let (out, _) = try conn.call(.getSBP2SessionState, scalarIn: [handle], scalarOutCount: 5)
        guard out.count >= 5 else { throw ProbeError("GetSBP2SessionState returned too few values") }
        return State(login: ASFW.LoginState(raw: out[0]), loginID: out[1],
                     generation: out[2], lastError: UInt32(truncatingIfNeeded: out[3]),
                     reconnectPending: out[4] != 0)
    }

    /// Send an arbitrary SCSI CDB and wait for the result.
    func sendSCSI(cdb: [UInt8],
                  direction: ASFW.DataDirection,
                  transferLength: UInt32 = 0,
                  outgoing: [UInt8] = [],
                  // 15 s: a 5 s default fired the dext ORB-timeout (→ AGENT_RESET)
                  // against a mechanically-settling scanner and wedged the session.
                  timeoutMs: UInt32 = 15_000,
                  captureSense: Bool = true) throws -> SCSIResult {
        precondition(cdb.count >= 1 && cdb.count <= 16, "CDB must be 1–16 bytes")

        // Apply the --orb-timeout-ms A-test ceiling here so it covers every
        // caller (tuned constants and one-off hardcoded timeouts alike).
        let timeoutMs = ProbeConfig.cap(timeoutMs)

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

        // The dext refuses new submissions (kIOReturnError) while a previous ORB
        // is still in flight. A wedged ORB releases its slot when the dext-side
        // timeout/settle-poll failure path fires — that can take tens of seconds
        // (each AGENT_STATE poll read has its own async timeout). Wait it out
        // instead of dying: one lost ORB should cost a stall, not the whole run.
        var submitTry = 0
        while true {
            do {
                try conn.call(.submitSBP2Command, scalarIn: [handle], structIn: req)
                break
            } catch let e as ProbeError where e.kr == kIOReturnError {
                submitTry += 1
                if let st = try? state(), st.login != .loggedIn {
                    throw ProbeError("submit rejected and session is no longer logged in "
                        + "(state=\(st.login.label)) — giving up")
                }
                guard submitTry < 45 else { throw e }
                if submitTry == 1 {
                    print("   ⏳ submit rejected (previous ORB stuck in the dext) — waiting for freed slot…")
                } else if submitTry % 10 == 0 {
                    print("   ⏳ still waiting (\(submitTry)s)…")
                }
                usleep(1_000_000)
            }
        }

        // Poll for the result (GetSBP2CommandResult returns NotFound until ready).
        // The dext only returns payload bytes for data-IN (registry fills
        // result.payload solely for FromTarget); data-OUT/no-data results are
        // header+sense only, and captureSense duplicates a data-IN payload into
        // the sense field (doubling the result). The requested cap rides the
        // user-client marshalling, which has three zones: ≤ ~2304 B inline OK;
        // 2305–4095 overflows the IIG RPC reply (BadArgument); > 4096 the kernel
        // hands the dext a structureOutputDescriptor, which dext v13+ fills.
        // Round dead-zone requests up into the descriptor zone.
        let payloadCap = direction == .fromTarget
            ? Int(transferLength) * (captureSense ? 2 : 1) : 0
        var cap = payloadCap + 256
        if cap > 2304 { cap = max(cap, 4352) }
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
                throw ProbeError("getResult NoSpace→retry failed: kr=0x\(String(format: "%08x", UInt32(bitPattern: kr2))) "
                    + "firstReportedSize=\(reportedSize) retryReportedSize=\(size2) (struct too large / out-of-line?)")
            }
            // Other error. Probe whether the dext is still alive.
            let alive: String
            do {
                let (out, _) = try conn.call(.getSBP2SessionState, scalarIn: [handle], scalarOutCount: 5)
                alive = "dext RESPONDS (state=\(out.first.map { ASFW.LoginState(raw: $0).label } ?? "?"))"
            } catch let live {
                alive = "dext NOT RESPONDING (\(live))"
            }
            throw ProbeError("getResult kr=0x\(String(format: "%08x", UInt32(bitPattern: kr))) "
                + "reportedSize=\(reportedSize) (raw single call, no retry) — \(alive)")
        }
        throw ProbeError("SCSI command timeout (no result)")
    }

    /// Diagnostics: how much the target actually transferred against the last
    /// completed command's data buffer. `targetWroteBytes` is data-IN (target →
    /// host); `targetReadBytes` is data-OUT (host → target). Zero on a direction
    /// that should be non-zero means the target never touched our buffer.
    struct TransferInfo {
        let opcode: UInt8
        let direction: UInt8      // 0=none, 1=fromTarget(data-in), 2=toTarget(data-out)
        let expectedBytes: UInt32
        let targetWroteBytes: UInt64; let targetWroteCalls: UInt32
        let targetReadBytes: UInt64;  let targetReadCalls: UInt32
        var dirLabel: String { direction == 1 ? "data-IN" : direction == 2 ? "data-OUT" : "no-data" }
    }

    func lastTransferInfo() -> TransferInfo? {
        guard let (out, _) = try? conn.call(.getSBP2TransferInfo, scalarIn: [handle], scalarOutCount: 7),
              out.count >= 7 else { return nil }
        return TransferInfo(
            opcode: UInt8(truncatingIfNeeded: out[0]),
            direction: UInt8(truncatingIfNeeded: out[1]),
            expectedBytes: UInt32(truncatingIfNeeded: out[2]),
            targetWroteBytes: out[3], targetWroteCalls: UInt32(truncatingIfNeeded: out[4]),
            targetReadBytes: out[5], targetReadCalls: UInt32(truncatingIfNeeded: out[6]))
    }

    private func parseResult(_ data: Data) -> SCSIResult {
        guard data.count >= 16 else {
            return SCSIResult(transportStatus: -1, sbpStatus: 0xFF, scsiStatus: nil,
                              payload: Data(), sense: Data())
        }
        let transportStatus = data.i32(0)
        let sbpStatus = data.u8(4)
        let scsiStatus: UInt8? = data.u8(5) != 0 ? data.u8(6) : nil
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
                          scsiStatus: scsiStatus, payload: payload, sense: sense)
    }

    /// SBP-2 LOGICAL UNIT RESET (task management function 0x0E) via the separate
    /// management agent. Resets the LU's task state to un-wedge a command fetch
    /// agent that has stopped servicing ORBs — the dext's slot-recovery frees the
    /// host side, but only this resets the scanner side. Best-effort.
    func resetLogicalUnit() {
        _ = try? conn.call(.submitSBP2TaskManagement, scalarIn: [handle, 0x0E])
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
        guard d.count >= 36 else { return "INQUIRY data too short (\(d.count) bytes)" }
        let pdt = d.u8(0) & 0x1F
        let vendor = d.cString(8, 8).trimmingCharacters(in: .whitespaces)
        let product = d.cString(16, 16).trimmingCharacters(in: .whitespaces)
        let revision = d.cString(32, 4).trimmingCharacters(in: .whitespaces)
        return "PDT=0x\(String(format: "%02x", pdt))  vendor=«\(vendor)»  product=«\(product)»  rev=«\(revision)»"
    }
}
