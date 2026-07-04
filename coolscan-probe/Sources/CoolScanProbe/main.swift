import Foundation

// CoolScanProbe — "proof of life" for driving a FireWire SBP-2 scanner (Nikon
// CoolScan 9000 ED) through the ASFireWire DriverKit stack on macOS Tahoe.
//
// Flow:  open dext → enumerate bus → find SBP-2 target → login → INQUIRY.
// A successful INQUIRY that reports "Nikon … LS-9000" is the go/no-go gate for
// the whole project: everything above it is then just porting the CoolScan SCSI
// command set (reference: SANE coolscan3) on top of `SBP2Session.sendSCSI`.

func hexDump(_ d: Data, max: Int = 64) -> String {
    let slice = d.prefix(max)
    let hex = slice.map { String(format: "%02x", $0) }.joined(separator: " ")
    return d.count > max ? "\(hex) … (+\(d.count - max) bytes)" : hex
}

func run() -> Int32 {
    print("=== CoolScanProbe — ASFireWire SBP-2 proof of life ===")
    print("    regime: \(ProbeConfig.describe())\n")

    // 1) Open the dext user client.
    let conn: ASFWConnection
    do {
        conn = try ASFWConnection()
        print("✅ Connected to «\(ASFW.serviceName)» user client.")
    } catch {
        print("❌ \(error)")
        return 2
    }

    // 2) Enumerate the bus.
    let devices: [FWDevice]
    do {
        devices = try Discovery.enumerate(conn)
    } catch {
        print("❌ Enumeration failed: \(error)")
        return 3
    }

    guard !devices.isEmpty else {
        print("""
        ⚠️  No FireWire devices found.
            Check: TB→FW adapter connected, 9000 powered on, cable OK,
            and that the bus has come up (Self-ID) in the ASFireWire log.
        """)
        return 4
    }

    print("\nFound \(devices.count) device(s):")
    for dev in devices {
        print("  • \(dev.guidString)  node=\(dev.nodeId)  «\(dev.vendorName)» / «\(dev.modelName)»  units=\(dev.units.count)")
        for u in dev.units {
            let tag = u.isSBP2 ? "SBP-2 ✅" : "spec=0x\(String(format: "%06x", u.specId))"
            print("      └─ \(tag)  romOffset=0x\(String(format: "%x", u.romOffset))  lun=\(u.lun)  «\(u.vendorName)» / «\(u.productName)»")
        }
    }

    // 3) Pick the first SBP-2 target (the CoolScan).
    guard let device = devices.first(where: { !$0.sbp2Units.isEmpty }),
          let unit = device.sbp2Units.first else {
        print("\n⚠️  No SBP-2 device (scanner/storage) on the bus. CoolScan logs in as SBP-2 — check that it is powered on.")
        return 5
    }
    print("\n→ Using SBP-2 target \(device.guidString) (LUN \(unit.lun)).")

    // 4) Log in.
    let session: SBP2Session
    do {
        session = try SBP2Session.login(conn, device: device, unit: unit)
        print("✅ SBP-2 login OK (handle=0x\(String(format: "%llx", session.handle))).")
    } catch {
        print("❌ Login failed: \(error)")
        return 6
    }
    defer { session.release() }

    // Submission model is no longer probe-selectable: main's FetchAgent always
    // submits the first ORB to the fetch-agent register and chains the rest via
    // their next-ORB pointer (the Linux firewire-sbp2 model). The old
    // --reset-per-orb A/B toggle drove a dext selector that no longer exists.

    // 5) INQUIRY — the actual go/no-go signal.
    do {
        let r = try SCSI.inquiry(session)
        if r.ok {
            print("\n🎉 INQUIRY OK:\n   \(SCSI.describeInquiry(r.payload))")
            print("   raw: \(hexDump(r.payload))")
        } else {
            print("\n⚠️  INQUIRY went through, but status not OK (transport=\(r.transportStatus) sbp=\(r.sbpStatus)).")
            if !r.sense.isEmpty { print("   sense: \(hexDump(r.sense))") }
        }
    } catch {
        print("❌ INQUIRY failed: \(error)")
        return 7
    }

    // 6) TEST UNIT READY — is the scanner spun up / ready?
    do {
        let r = try SCSI.testUnitReady(session)
        print("\nTEST UNIT READY: transport=\(r.transportStatus) sbp=\(r.sbpStatus) \(r.ok ? "(ready ✅)" : "(not ready — check sense)")")
        if !r.ok && !r.sense.isEmpty { print("   sense: \(hexDump(r.sense))") }
    } catch {
        print("⚠️  TEST UNIT READY failed: \(error)")
    }

    // CHAINTEST mode: isolate WHY command #3 (the 2nd doorbell) fails. Every run
    // already issued INQUIRY (cmd1, via ORB_POINTER) + TUR (cmd2, 1st doorbell)
    // above. In windowcheck, cmd3 is MODE SELECT — which is BOTH the 2nd doorbell
    // AND the first data-OUT command, so we can't tell which property breaks it.
    // Here we extend the chain with pure no-data (TUR) and data-IN (INQUIRY)
    // commands so the failure point is unambiguous:
    //   • cmd3 (TUR) times out      ⇒ the 2nd doorbell is broken for ANY command
    //                                  = a pure chain-depth bug (data dir is moot).
    //   • cmd3/cmd4 (TUR) complete  ⇒ no-data chaining works deep; MODE SELECT's
    //                                  failure is data-OUT-specific.
    //   • cmd5/cmd7 (INQUIRY) ok    ⇒ data-IN works in a chain ⇒ the bug is
    //                                  specifically data-OUT inside a doorbell chain.
    if CommandLine.arguments.contains("chaintest") {
        print("\n=== CHAINTEST: doorbell-chain depth (cmd1=INQUIRY, cmd2=TUR already done) ===")
        print("Each command below extends the doorbell chain by one. We want to know WHERE it breaks.\n")
        func step(_ n: Int, _ label: String, _ body: () throws -> SCSIResult) -> Bool {
            do {
                let r = try body()
                let extra = r.payload.count > 0 ? " — \(r.payload.count)B received" : ""
                print("  cmd\(n) [\(label)]: ✅ completed (transport=\(r.transportStatus) sbp=\(r.sbpStatus))\(extra)")
                if let t = session.lastTransferInfo() {
                    print("       [xfer] \(t.dirLabel) expected=\(t.expectedBytes)B "
                        + "targetWROTE=\(t.targetWroteBytes)B/\(t.targetWroteCalls)x "
                        + "targetREAD=\(t.targetReadBytes)B/\(t.targetReadCalls)x")
                }
                return true
            } catch {
                print("  cmd\(n) [\(label)]: ❌ \(error)")
                return false
            }
        }
        var allOk = true
        allOk = step(3, "TUR no-data (2nd doorbell)")  { try SCSI.testUnitReady(session) }    && allOk
        allOk = step(4, "TUR no-data (3rd doorbell)")  { try SCSI.testUnitReady(session) }    && allOk
        allOk = step(5, "INQUIRY 96B data-IN in chain") { try SCSI.inquiry(session, allocation: 96) } && allOk
        allOk = step(6, "TUR no-data (5th doorbell)")  { try SCSI.testUnitReady(session) }    && allOk
        allOk = step(7, "INQUIRY 36B data-IN")        { try SCSI.inquiry(session, allocation: 36) } && allOk
        print("""

        INTERPRETATION:
          • cmd3 (TUR) timeout            ⇒ 2nd doorbell broken for ALL commands = pure chain-depth bug.
          • cmd3/cmd4 (TUR) OK            ⇒ no-data chaining works deep; MODE SELECT failure is data-OUT-specific.
          • cmd5/cmd7 (INQUIRY data-IN) OK ⇒ data-IN works in chain ⇒ bug is specifically data-OUT in doorbell chain.
        """)
        return allOk ? 0 : 9
    }

    // WINDOWCHECK mode: prove whether data-OUT actually reaches the scanner.
    // SET WINDOW (data-out) with distinctive values, then GET WINDOW (small
    // data-in, a path we KNOW works via INQUIRY/0xC1) and compare. If the fields
    // survive the round-trip, data-out is fine and the scan no-op is elsewhere
    // (missing LOAD/focus/ready-poll). If they read back zero, our SET WINDOW
    // payload never landed = a data-OUT DMA bug.
    if CommandLine.arguments.contains("windowcheck") {
        // Retry transient timeouts (the reset-per-ORB path is flaky) so a single
        // hiccup doesn't waste a replug. Only retries on THROWN errors, never on a
        // !ok result — the invalid-window probe below MUST keep its CHECK CONDITION.
        func attempt<T>(_ label: String, _ body: () throws -> T) throws -> T {
            var last: Error = ProbeError("\(label): no attempts")
            for i in 1...4 {
                do { return try body() }
                catch {
                    last = error
                    if i < 4 { print("   ↻ \(label) attempt \(i) failed (\(error)); retrying…"); usleep(300_000) }
                }
            }
            throw last
        }
        // Print the dext's view of how many bytes the target actually moved against
        // the last command's data buffer — the whole point of this run.
        func xfer(_ label: String) {
            if let t = session.lastTransferInfo() {
                print("  [xfer \(label)] \(t.dirLabel) expected=\(t.expectedBytes)B  "
                    + "targetWROTE=\(t.targetWroteBytes)B/\(t.targetWroteCalls)x  "
                    + "targetREAD=\(t.targetReadBytes)B/\(t.targetReadCalls)x")
            } else {
                print("  [xfer \(label)] (no dext info — older dext? selector 62 missing)")
            }
        }
        do {
            // Use the known fallback caps instead of reading 0xC1 — that read is
            // itself flaky and irrelevant to whether data-OUT lands. Fewer commands
            // before the critical SET WINDOW = fewer chances to derail the test.
            let caps = CoolScan.Capabilities.coolScan9000
            print("\nUsing fallback caps: area \(caps.boundaryX)×\(caps.boundaryY) du, \(caps.maxBits) bit.")
            _ = try attempt("MODE SELECT") { try CoolScan.modeSelect(session, unitDpi: caps.resXMax) }

            // Distinctive, non-zero, easy-to-spot geometry.
            let g = CoolScan.geometry(caps, resolution: 500, yOffsetDU: 2900)
            let w = CoolScan.window(g, color: .red)
            let sent = CoolScan.windowDescriptor(w)
            print("\n— SET WINDOW (R) sending \(sent.count) bytes —")
            print("  hex: \(sent.map { String(format: "%02x", $0) }.joined(separator: " "))")
            print(String(format: "  fields: resX=%d resY=%d xOff=%d yOff=%d w=%d h=%d depth=%d",
                         w.resX, w.resY, w.xOffset, w.yOffset, w.width, w.height, w.depth))

            let sr = try attempt("SET WINDOW") { try CoolScan.setWindow(session, w) }
            print("  SET WINDOW status: transport=\(sr.transportStatus) sbp=\(sr.sbpStatus) \(sr.ok ? "✅" : "❌")")
            xfer("SET WINDOW")   // data-OUT → expect targetREAD≈58 if delivery works

            let gr = try attempt("GET WINDOW") { try CoolScan.getWindowRaw(session, color: .red) }
            xfer("GET WINDOW")   // data-IN → expect targetWROTE≈58 if delivery works
            let b = [UInt8](gr.payload)
            print("\n— GET WINDOW (R) read \(b.count) bytes —")
            print("  hex: \(b.map { String(format: "%02x", $0) }.joined(separator: " "))")
            func be16(_ o: Int) -> Int { b.count > o+1 ? (Int(b[o])<<8)|Int(b[o+1]) : -1 }
            func be32(_ o: Int) -> Int { b.count > o+3 ? (Int(b[o])<<24)|(Int(b[o+1])<<16)|(Int(b[o+2])<<8)|Int(b[o+3]) : -1 }
            // Same offsets we wrote (descriptor body starts after the 8-byte header).
            print(String(format: "  parsed @SET-offsets: resX=%d resY=%d xOff=%d yOff=%d w=%d h=%d",
                         be16(10), be16(12), be32(14), be32(18), be32(22), be32(26)))
            let match = be16(10) == Int(w.resX) && be32(18) == Int(w.yOffset) && be32(22) == Int(w.width)
            print(match
                ? "\n✅ Data-OUT WORKS — SET WINDOW values survived the round-trip. Scan no-op lies elsewhere (LOAD/focus/ready-poll)."
                : "\n⚠️  GET WINDOW does not match what we wrote — either the payload never landed, OR the scanner discarded the contents.")

            // DISCRIMINATOR: send a SET WINDOW with an IMPOSSIBLE resolution
            // (0xFFFF dpi). If the scanner READ our bytes it must reject this with
            // CHECK CONDITION (sense). If it returns GOOD, it never saw our payload
            // (read zeros → empty descriptor) = data-OUT delivery is broken.
            var bad = sent
            bad[10] = 0xFF; bad[11] = 0xFF   // resX = 65535 dpi — impossible
            print("\n— SET WINDOW (R) with INVALID resX=0xFFFF (separates delivery from content) —")
            let badCdb: [UInt8] = [0x24, 0, 0, 0, 0, 0, 0, 0, 0x3a, 0x00]
            let br = try attempt("SET WINDOW(bad)") {
                try session.sendSCSI(cdb: badCdb, direction: .toTarget,
                                     transferLength: UInt32(bad.count), outgoing: bad,
                                     captureSense: true)
            }
            print("  status: transport=\(br.transportStatus) sbp=\(br.sbpStatus) \(br.ok ? "GOOD" : "CHECK/error")")
            xfer("SET WINDOW(bad)")   // data-OUT → did the target read these bytes?
            if !br.sense.isEmpty { print("  sense: \(br.sense.map { String(format: "%02x", $0) }.joined(separator: " "))") }
            print(br.ok
                ? "→ GOOD on impossible value ⇒ scanner NEVER read our payload (zero buffer) = DATA-OUT DELIVERY IS BROKEN in the dext."
                : "→ CHECK CONDITION ⇒ scanner READ our bytes = data-OUT delivers; SET WINDOW no-op is a content/protocol problem.")
        } catch {
            print("❌ windowcheck failed: \(error)")
            return 8
        }
        return 0
    }

    // SCAN mode: «CoolScanProbe scan [dpi]» replicates VueScan's captured scan
    // sequence (capture/DECODED.md) and dumps the raw RGBI byte stream. Default
    // 500 dpi, frame box = the captured 35mm frame 1.
    if CommandLine.arguments.contains("scan") {
        let dpi = scanDpiArg() ?? 500
        let rows = scanRowsArg()   // optional thin-strip cap
        let yOff = scanYOffsetArg() ?? CoolScan.captureFrame.y   // optional override
        do {
            // No frame-info read here: this early in the run the scanner is
            // still booting and returns zeros — scanFrame dumps it post-ready.
            // Every pre-ready command is also one more pull of the transport
            // roulette before the diagnostics get a chance to run.
            let caps = try CoolScan.capabilities(session)
            print("Capabilities: optical \(caps.resXOptical) dpi, area "
                + "\(caps.boundaryX)×\(caps.boundaryY) du, \(caps.maxBits) bit.")
            var g = CoolScan.geometry(caps, resolution: dpi, depth: nil,
                                      xOffsetDU: CoolScan.captureFrame.x, yOffsetDU: yOff,
                                      widthDU: CoolScan.captureFrame.w,
                                      heightDU: CoolScan.captureFrame.h)
            if let rows = rows, rows > 0, UInt32(rows) < g.logicalHeight {
                g.logicalHeight = UInt32(rows); g.heightDU = UInt32(rows) * g.pitchY
            }
            print("Scan @ \(g.realResX)×\(g.realResY) dpi → \(g.logicalWidth)×\(g.logicalHeight) px, "
                + "\(g.nColors) channels, \(g.bytesPerPixel)B/px → \(g.totalBytes) bytes expected"
                + (rows != nil ? " (strip: \(rows!) rows)" : "")
                + " @ offset (\(CoolScan.captureFrame.x),\(yOff)) du.")
            var lastPct = -1
            let skipFocus = CommandLine.arguments.contains("nofocus")
            let result = try CoolScan.scanFrame(session, caps: caps, resolution: dpi,
                                                rows: rows, skipFocus: skipFocus,
                                                yOffsetDU: yOff) { got, total in
                let pct = total > 0 ? got * 100 / total : 0
                if pct != lastPct { lastPct = pct; FileHandle.standardError.write(Data("\r  read \(pct)% (\(got)/\(total) bytes)".utf8)) }
            }
            FileHandle.standardError.write(Data("\n".utf8))
            try saveScan(result)
            print(result.complete ? "✅ Scan complete." : "⚠️  Short scan (\(result.raw.count)/\(result.expectedBytes) bytes).")
        } catch {
            print("❌ Scan failed: \(error)")
            return 8
        }
        return 0
    }

    // Diagnostics (REQUEST SENSE + EVPD 0xC1) are opt-in: they can wedge the
    // command pipeline on some firmware, which then blocks a clean LOGOUT. Run
    // `CoolScanProbe diag` to enable. Default stays a clean login→inquiry→logout.
    guard CommandLine.arguments.contains("diag") else {
        print("\nDone (clean run). Run «CoolScanProbe diag» for a capability dump.")
        return 0
    }

    // 6b) DISCRIMINATOR: a second full-size INQUIRY (96-byte data-in, non-first).
    //     INQUIRY #1 (data-in) and TUR (no-data) both succeeded. If this 96-byte
    //     repeat works but the small reads below hang → the bug is transfer-SIZE
    //     sensitive. If this hangs too → it's "any non-first data-in" (ordinality).
    do {
        let r = try session.sendSCSI(cdb: [0x12, 0x00, 0x00, 0x00, 96, 0x00],
                                     direction: .fromTarget, transferLength: 96, timeoutMs: 3000)
        print("\nINQUIRY #2 (96B, non-first data-in): "
            + "\(r.ok ? "OK ✅" : "status \(r.transportStatus)/\(r.sbpStatus)") — \(r.payload.count) bytes received")
    } catch {
        print("\n⚠️  INQUIRY #2 (96B) failed: \(error)")
    }

    // 7) Nikon capability page 0xC1 FIRST — this is the data we actually want
    //    (max res, boundaries, focus range) and its layout is unknown on the 9000.
    //    Header-first: read 5 bytes to learn the page length, then read exactly
    //    that. Short timeout so a misbehaving page can't wedge us for long.
    do {
        let hdr = try session.sendSCSI(cdb: [0x12, 0x01, 0xC1, 0x00, 0x05, 0x00],
                                       direction: .fromTarget, transferLength: 5, timeoutMs: 3000)
        let hb = [UInt8](hdr.payload)
        guard hdr.ok, hb.count >= 4 else {
            print("\nEVPD 0xC1 header: status=\(hdr.transportStatus)/\(hdr.sbpStatus) "
                + "raw=\(hexDump(hdr.payload, max: 8)) — 9000 may not support the page.")
            if !hdr.sense.isEmpty { print("   sense: \(hexDump(hdr.sense, max: 32))") }
            throw ProbeError("0xC1 header not usable")
        }
        let pageLen = Int(hb[3])
        let total = UInt8(min(4 + pageLen, 255))
        print("\nEVPD 0xC1: page_len field=\(pageLen) → reading \(total) bytes")
        let r = try session.sendSCSI(cdb: [0x12, 0x01, 0xC1, 0x00, total, 0x00],
                                     direction: .fromTarget, transferLength: UInt32(total), timeoutMs: 3000)
        print("EVPD 0xC1 (Nikon capabilities): \(r.payload.count) bytes")
        print("   raw: \(hexDump(r.payload, max: 256))")
        if let caps = CoolScan.parseCapabilities(r.payload) {
            print("""
               → optical: \(caps.resXOptical)×\(caps.resYOptical) dpi  \
            max: \(caps.resXMax)×\(caps.resYMax)  min: \(caps.resXMin)×\(caps.resYMin)
               → max scan area: \(caps.boundaryX)×\(caps.boundaryY) px @ optical  \
            (\(String(format: "%.2f", Double(caps.boundaryX)/Double(caps.resXOptical)))″×\
            \(String(format: "%.2f", Double(caps.boundaryY)/Double(caps.resYOptical)))″)
               → focus: \(caps.focusMin)…\(caps.focusMax)  bit depth: \(caps.maxBits)  \
            frames: \(caps.nFrames)
            """)
        } else {
            print("   ⚠️  too short to parse the capability fields.")
        }
    } catch {
        print("⚠️  EVPD 0xC1 failed: \(error)")
    }

    // 8) REQUEST SENSE last — known to hang on this firmware; do it after the
    //    capability dump so a hang can't cost us the data above.
    do {
        let r = try session.sendSCSI(cdb: [0x03, 0, 0, 0, 18, 0],
                                     direction: .fromTarget, transferLength: 18, timeoutMs: 3000)
        let b = [UInt8](r.payload)
        if b.count >= 14 {
            let key = b[2] & 0x0f, asc = b[12], ascq = b[13]
            print("\nREQUEST SENSE: key=0x\(String(format: "%x", key)) "
                + "ASC=0x\(String(format: "%02x", asc)) ASCQ=0x\(String(format: "%02x", ascq)) "
                + "(\(senseText(key: key, asc: asc, ascq: ascq)))")
        }
        print("   raw: \(hexDump(r.payload, max: 32))")
    } catch {
        print("⚠️  REQUEST SENSE failed: \(error)")
    }

    // 9) Bonus: TARGET RESET via task management (function 0x0F). Tests whether
    //    this clears the target's SBP-2 state so the NEXT login succeeds without
    //    a physical power-cycle (logout alone does not free the login).
    do {
        try conn.call(.submitSBP2TaskManagement, scalarIn: [session.handle, 0x0F])
        print("\nTARGET RESET (task mgmt 0x0F) sent OK — check whether the next login works without a power cycle.")
    } catch {
        print("\n⚠️  TARGET RESET failed: \(error)")
    }

    print("\nDone. If INQUIRY shows «Nikon … LS-9000» the transport layer is proven — next step is the CoolScan command set.")
    return 0
}

/// Parse an optional dpi argument following «scan» (e.g. «scan 1000»).
func scanDpiArg() -> UInt16? {
    let args = CommandLine.arguments
    guard let i = args.firstIndex(of: "scan"), i + 1 < args.count,
          let v = UInt16(args[i + 1]) else { return nil }
    return v
}

/// Parse an optional row-count following the dpi (e.g. «scan 500 8» → 8 rows).
/// Caps the scan height for a fast, reliable thin-strip proof.
func scanRowsArg() -> Int? {
    let args = CommandLine.arguments
    guard let i = args.firstIndex(of: "scan"), i + 2 < args.count,
          let v = Int(args[i + 2]) else { return nil }
    return v
}

/// Parse an optional y-offset (device units) following the row count
/// (e.g. «scan 500 2 2900» → 2-row strip starting 2900 du down). Lets us scan
/// from the middle of the frame to skip the opaque holder-mask top edge.
func scanYOffsetArg() -> UInt32? {
    let args = CommandLine.arguments
    guard let i = args.firstIndex(of: "scan"), i + 3 < args.count,
          let v = UInt32(args[i + 3]) else { return nil }
    return v
}

/// Save the raw scan stream + a sidecar describing the geometry, so the byte/colour
/// layout can be confirmed offline before we commit to a TIFF writer.
func saveScan(_ r: CoolScan.ScanResult) throws {
    let g = r.geometry
    let yTag = g.yOffsetDU > 0 ? "-y\(g.yOffsetDU)" : ""
    let base = "coolscan-\(g.realResX)dpi-\(g.logicalWidth)x\(g.logicalHeight)-\(g.nColors)ch-\(g.depth)bit\(yTag)"
    let binURL = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        .appendingPathComponent(base + ".raw")
    try r.raw.write(to: binURL)
    let sidecar = """
    CoolScan 9000 raw scan
    resolution    : \(g.realResX) x \(g.realResY) dpi (pitch \(g.pitchX)/\(g.pitchY))
    logical pixels: \(g.logicalWidth) x \(g.logicalHeight)
    channels      : \(g.nColors)  (R,G,B,IR — window ids 01,02,03,09)
    bytes/pixel   : \(g.bytesPerPixel)  (depth \(g.depth) bit)
    bytes/line    : \(g.bytesPerLine)  (all channels per line, per capture READ totals)
    expected bytes: \(g.totalBytes)
    received bytes: \(r.raw.count)\(r.complete ? "" : "  ⚠️ SHORT")

    Channel interleave WITHIN a line (line-sequential vs pixel-interleaved) is
    unconfirmed — try \(g.logicalWidth)x\(g.logicalHeight), \(g.bytesPerPixel == 2 ? "16-bit big-endian" : "8-bit"), \(g.nColors) ch
    both ways and keep the one that looks right before writing TIFF.
    """
    try sidecar.write(to: URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        .appendingPathComponent(base + ".txt"), atomically: true, encoding: .utf8)
    print("💾 Saved \(binURL.lastPathComponent) (\(r.raw.count) bytes) + sidecar \(base).txt")
}

/// Minimal SCSI sense key/ASC decode for the messages we expect from the 9000.
func senseText(key: UInt8, asc: UInt8, ascq: UInt8) -> String {
    switch (key, asc, ascq) {
    case (0x0, _, _):        return "NO SENSE — ready"
    case (0x2, 0x04, _):     return "NOT READY (LOGICAL UNIT)"
    case (0x2, 0x3a, _):     return "MEDIUM NOT PRESENT — no film/holder"
    case (0x6, 0x29, _):     return "POWER ON / RESET (UA)"
    case (0x6, 0x28, _):     return "NOT READY→READY, medium inserted (UA)"
    case (0x6, 0x3f, _):     return "TARGET OPERATING CONDITIONS CHANGED (UA)"
    case (0x6, _, _):        return "UNIT ATTENTION (other)"
    case (0x5, 0x20, _):     return "INVALID COMMAND OPCODE"
    case (0x5, 0x24, _):     return "INVALID FIELD IN CDB"
    case (0x5, 0x26, _):     return "INVALID FIELD IN PARAMETER LIST"
    case (0x5, _, _):        return "ILLEGAL REQUEST (other)"
    case (_, 0x3e, _):       return "LU NOT SELF-CONFIGURED YET (scanner booting — wait)"
    case (0xb, _, _):        return "ABORTED COMMAND"
    default:                 return "unknown — look up in the SPC sense table"
    }
}

exit(run())
