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
    return d.count > max ? "\(hex) … (+\(d.count - max) byte)" : hex
}

func run() -> Int32 {
    print("=== CoolScanProbe — ASFireWire SBP-2 proof of life ===\n")

    // 1) Open the dext user client.
    let conn: ASFWConnection
    do {
        conn = try ASFWConnection()
        print("✅ Koblet til «\(ASFW.serviceName)» user client.")
    } catch {
        print("❌ \(error)")
        return 2
    }

    // 2) Enumerate the bus.
    let devices: [FWDevice]
    do {
        devices = try Discovery.enumerate(conn)
    } catch {
        print("❌ Enumerering feilet: \(error)")
        return 3
    }

    guard !devices.isEmpty else {
        print("""
        ⚠️  Ingen FireWire-enheter funnet.
            Sjekk: TB→FW-adapter tilkoblet, 9000 påslått, kabel i orden,
            og at bussen har kommet opp (Self-ID) i ASFireWire-loggen.
        """)
        return 4
    }

    print("\nFant \(devices.count) enhet(er):")
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
        print("\n⚠️  Ingen SBP-2-enhet (skanner/storage) på bussen. CoolScan logger inn som SBP-2 — sjekk at den er påslått.")
        return 5
    }
    print("\n→ Bruker SBP-2-target \(device.guidString) (LUN \(unit.lun)).")

    // 4) Log in.
    let session: SBP2Session
    do {
        session = try SBP2Session.login(conn, device: device, unit: unit)
        print("✅ SBP-2 login OK (handle=0x\(String(format: "%llx", session.handle))).")
    } catch {
        print("❌ Login feilet: \(error)")
        return 6
    }
    defer { session.release() }

    // 5) INQUIRY — the actual go/no-go signal.
    do {
        let r = try SCSI.inquiry(session)
        if r.ok {
            print("\n🎉 INQUIRY OK:\n   \(SCSI.describeInquiry(r.payload))")
            print("   raw: \(hexDump(r.payload))")
        } else {
            print("\n⚠️  INQUIRY kom gjennom, men status ikke OK (transport=\(r.transportStatus) sbp=\(r.sbpStatus)).")
            if !r.sense.isEmpty { print("   sense: \(hexDump(r.sense))") }
        }
    } catch {
        print("❌ INQUIRY feilet: \(error)")
        return 7
    }

    // 6) TEST UNIT READY — is the scanner spun up / ready?
    do {
        let r = try SCSI.testUnitReady(session)
        print("\nTEST UNIT READY: transport=\(r.transportStatus) sbp=\(r.sbpStatus) \(r.ok ? "(klar ✅)" : "(ikke klar — sjekk sense)")")
        if !r.ok && !r.sense.isEmpty { print("   sense: \(hexDump(r.sense))") }
    } catch {
        print("⚠️  TEST UNIT READY feilet: \(error)")
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
        print("\n=== CHAINTEST: doorbell-chain depth (cmd1=INQUIRY, cmd2=TUR allerede gjort) ===")
        print("Hver kommando under forlenger doorbell-kjeden med én. Vi vil vite HVOR den knekker.\n")
        func step(_ n: Int, _ label: String, _ body: () throws -> SCSIResult) -> Bool {
            do {
                let r = try body()
                let extra = r.payload.count > 0 ? " — \(r.payload.count)B mottatt" : ""
                print("  cmd\(n) [\(label)]: ✅ fullført (transport=\(r.transportStatus) sbp=\(r.sbpStatus))\(extra)")
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
        allOk = step(3, "TUR no-data (2. doorbell)")  { try SCSI.testUnitReady(session) }    && allOk
        allOk = step(4, "TUR no-data (3. doorbell)")  { try SCSI.testUnitReady(session) }    && allOk
        allOk = step(5, "INQUIRY 96B data-IN i kjede") { try SCSI.inquiry(session, allocation: 96) } && allOk
        allOk = step(6, "TUR no-data (5. doorbell)")  { try SCSI.testUnitReady(session) }    && allOk
        allOk = step(7, "INQUIRY 36B data-IN")        { try SCSI.inquiry(session, allocation: 36) } && allOk
        print("""

        TOLKNING:
          • cmd3 (TUR) timeout            ⇒ 2. doorbell knekt for ALLE kommandoer = ren kjede-dybde-bug.
          • cmd3/cmd4 (TUR) OK            ⇒ no-data-kjeding funker dypt; MODE SELECT-feilen er data-OUT-spesifikk.
          • cmd5/cmd7 (INQUIRY data-IN) OK ⇒ data-IN funker i kjede ⇒ feilen er spesifikt data-OUT i doorbell-kjede.
        """)
        return allOk ? 0 : 9
    }

    // SCANDIAG mode: the transport works and a scan completes, but every byte is
    // zero. Find WHERE in the scan sequence it goes wrong, using the scanner's own
    // SCSI state instead of relying on listening to the mechanism:
    //   • TUR polled right after SCAN — a BUSY/not-ready → ready transition PROVES
    //     the scanner is physically acquiring. Staying instantly ready = it never
    //     scanned (⇒ missing pre-scan setup: SEND LUT / SET FOCUS / LOAD).
    //   • REQUEST SENSE at each step — a sense key/ASC tells us what it wants.
    //   • the first READ's actual bytes — zero vs non-zero, decisively.
    if CommandLine.arguments.contains("scandiag") {
        print("\n=== SCANDIAG v18: grunngitt boundary → poll 30s (akseptert+BUSY vs wedget?) ===")
        // Retry transient reset-per-ORB timeouts (~1 command in ~10) so a single
        // hiccup doesn't abort the diagnostic.
        func attempt<T>(_ label: String, _ body: () throws -> T) throws -> T {
            var last: Error = ProbeError("\(label): ingen forsøk")
            for i in 1...4 {
                do { return try body() }
                catch { last = error; if i < 4 { usleep(250_000) } }
            }
            throw last
        }
        // One REQUEST SENSE → decoded (key,asc,ascq), or nil on empty/timeout.
        func senseOnce() -> (key: UInt8, asc: UInt8, ascq: UInt8)? {
            guard let r = try? CoolScan.requestSense(session) else { return nil }
            let b = [UInt8](r.payload)
            guard b.count >= 14 else { return nil }
            return (b[2] & 0x0f, b[12], b[13])
        }
        func show(_ label: String, _ s: (key: UInt8, asc: UInt8, ascq: UInt8)) {
            print("  [\(label)] key=0x\(String(format: "%x", s.key)) "
                + "ASC=0x\(String(format: "%02x", s.asc)) ASCQ=0x\(String(format: "%02x", s.ascq)) "
                + "(\(senseText(key: s.key, asc: s.asc, ascq: s.ascq)))")
        }
        // Drain queued UNIT ATTENTION (key 6) conditions until NO SENSE or a real
        // (non-UA) condition appears. A pending UA makes the scanner CHECK-CONDITION
        // the next command WITHOUT executing it, so the window never applies until
        // the queue is empty (this is what coolscan3's scanner_ready does).
        @discardableResult
        func drainUA(_ label: String) -> UInt8 {
            for _ in 0..<8 {
                guard let s = senseOnce() else { print("  [\(label)] (sense tom/timeout)"); return 0xff }
                show(label, s)
                if s.key != 6 { return s.key }   // NO SENSE (0) or a real condition — done
                usleep(120_000)
            }
            return 6
        }
        // Issue a command, then read its OWN sense. UNIT ATTENTION (key 6) means a
        // queued power-on/holder condition CHECK-CONDITIONed the command WITHOUT
        // executing it — so drain it and re-issue, until the command's sense is clean
        // (key 0 = executed) or a real reject (key 5 = bad field) appears. A single
        // drainUA at the start is not enough: the scanner keeps arming UAs as it
        // calibrates and detects the holder after power-on. This is exactly what
        // coolscan3's scanner_ready loops on between steps.
        func settle(_ label: String, maxTries: Int = 12, _ body: () throws -> SCSIResult) {
            for i in 1...maxTries {
                let cmd = try? attempt(label) { try body() }
                // Show the command's OWN SBP-2 result (transport/sbp) — settle used to
                // discard it, so a flaky follow-up sense hid whether the command itself
                // transported GOOD (accepted) vs timed out.
                let st = cmd.map { "transport=\($0.transportStatus) sbp=\($0.sbpStatus)\($0.ok ? " GOOD" : "")" } ?? "kastet/timeout"
                // Follow-up REQUEST SENSE is transiently flaky — retry so a single miss
                // doesn't hide the verdict.
                var s: (key: UInt8, asc: UInt8, ascq: UInt8)? = nil
                for _ in 0..<5 { if let x = senseOnce() { s = x; break }; usleep(200_000) }
                guard let sk = s else { print("  [\(label)] cmd[\(st)] — sense utilgjengelig (5×)"); return }
                if sk.key != 6 {
                    let v = sk.key == 5 ? "❌ AVVIST (bad felt)"
                          : sk.key == 0 ? "✅ AKSEPTERT (ren)"
                          : "? key=0x\(String(format: "%x", sk.key))"
                    show("\(label) cmd[\(st)] → \(v) (etter \(i))", sk)
                    return
                }
                show("\(label): UA — drener (\(i)/\(maxTries))", sk)
                usleep(150_000)
            }
            print("  [\(label)] ⚠️ kom aldri forbi UNIT ATTENTION (\(maxTries)×)")
        }
        // Poll until TRULY ready — TUR then REQUEST SENSE (the real status; the dext
        // masks the SCSI status byte, so TUR's own "ok" lies). key 0 = ready; key 6 =
        // UA (drain, re-poll now); not-ready / not-self-configured (key 2/0xb, ASC
        // 0x04/0x3e) = still booting → wait. coolscan3's cs3_scanner_ready.
        func scannerReady(_ label: String, timeout: TimeInterval = 120) -> Bool {
            let deadline = Date().addingTimeInterval(timeout)
            var polls = 0
            while Date() < deadline {
                _ = try? attempt(label) { try CoolScan.testUnitReady(session) }
                polls += 1
                guard let s = senseOnce() else { usleep(500_000); continue }
                if s.key == 0 { print("  [\(label)] ✅ klar (etter \(polls) poll)"); return true }
                if s.key == 6 { continue }   // UA — drain, re-poll straks
                if polls % 5 == 1 { show("\(label): venter", s) }
                usleep(1_000_000)
            }
            print("  [\(label)] ⚠️ ikke klar innen \(Int(timeout))s")
            return false
        }
        let caps = CoolScan.Capabilities.coolScan9000
        print("— drener UA-kø —"); _ = drainUA("drain")
        settle("RESERVE")     { try CoolScan.reserve(session) }
        // Vent ut power-on selv-konfigurering (0x3e) FØR de ekte kommandoene — ellers
        // aborterer alt med key 0xb/ASC 0x3e og boundary-testen blir meningsløs.
        guard scannerReady("scanner-ready") else {
            print("  ⚠️ scanneren ble ikke klar (film/holder isatt? nettopp slått på?).")
            return 0
        }
        settle("MODE SELECT") { try CoolScan.modeSelect(session, unitDpi: caps.resXMax) }

        // NO 0xC1 read — it wedges this firmware (v15: all post-read REQUEST SENSEs timed
        // out). We already have the live per-frame values from v14's dump (stable for this
        // FH 35mm holder): per-frame 4000×5904 du, resy_max 4000, frame_offset(cs3)=6001.
        // Hardcode them and test coolscan3's EXACT frame-0 geometry — never tested with the
        // right values (yEnd=fo-1=6000, xBoundary = per-frame boundaryX-1 = 3999, NOT 9999).
        // v17: cs3-grounded SET BOUNDARY (yEnd=6000, xB=3999) returned SBP-2 GOOD — UNLIKE
        // the wrong-geometry attempts (clean 0x26 reject) — then sense went unavailable and
        // the next commands failed transport (-1/255). Pattern = ACCEPTED + scanner BUSY (or
        // wedged). Issue once, then poll up to 30s (riding out BUSY + reset-per-ORB transient)
        // for the real verdict.
        let pfX: UInt32 = 4000, pfY: UInt32 = 5904, fo: UInt32 = 6001
        func runAndSettle(_ label: String, timeout: TimeInterval = 30, _ body: () throws -> SCSIResult) {
            let cmd = try? attempt(label) { try body() }
            let st = cmd.map { "transport=\($0.transportStatus) sbp=\($0.sbpStatus)\($0.ok ? " GOOD" : "")" } ?? "kastet"
            let dl = Date().addingTimeInterval(timeout)
            var polls = 0
            while Date() < dl {
                polls += 1
                if let s = senseOnce(), s.key != 6 {
                    let v = s.key == 5 ? "❌ AVVIST" : s.key == 0 ? "✅ AKSEPTERT/ren" : "key=0x\(String(format: "%x", s.key))"
                    show("\(label) cmd[\(st)] → \(v) (poll \(polls))", s); return
                }
                _ = try? CoolScan.testUnitReady(session)   // nudge through BUSY/transient
                usleep(500_000)
            }
            print("  [\(label)] cmd[\(st)] — ingen lesbar sense innen \(Int(timeout))s (BUSY/wedget?)")
        }
        runAndSettle("SET BOUNDARY cs3 yEnd=\(fo - 1) xB=\(pfX - 1)") {
            try CoolScan.setBoundary(session, frames: [(yOffset: 0, yEnd: fo - 1, xBoundary: pfX - 1)])
        }
        var liveCaps = CoolScan.Capabilities.coolScan9000
        liveCaps.boundaryX = pfX; liveCaps.boundaryY = pfY
        var gg = CoolScan.geometry(liveCaps, resolution: 1000, depth: 8, yOffsetDU: 0)
        gg.widthDU = 2000; gg.logicalWidth  = max(1, 2000 / gg.pitchX)
        gg.heightDU = 2000; gg.logicalHeight = max(1, 2000 / gg.pitchY)
        runAndSettle("SET WINDOW small") { try CoolScan.setWindow(session, CoolScan.window(gg, color: .red)) }
        return 0
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
            var last: Error = ProbeError("\(label): ingen forsøk")
            for i in 1...4 {
                do { return try body() }
                catch {
                    last = error
                    if i < 4 { print("   ↻ \(label) forsøk \(i) feilet (\(error)); prøver igjen…"); usleep(300_000) }
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
                print("  [xfer \(label)] (ingen dext-info — eldre dext? selector 62 mangler)")
            }
        }
        do {
            // Use the known fallback caps instead of reading 0xC1 — that read is
            // itself flaky and irrelevant to whether data-OUT lands. Fewer commands
            // before the critical SET WINDOW = fewer chances to derail the test.
            let caps = CoolScan.Capabilities.coolScan9000
            print("\nBruker fallback-caps: areal \(caps.boundaryX)×\(caps.boundaryY) du, \(caps.maxBits) bit.")
            _ = try attempt("MODE SELECT") { try CoolScan.modeSelect(session, unitDpi: caps.resXMax) }

            // Distinctive, non-zero, easy-to-spot geometry.
            let g = CoolScan.geometry(caps, resolution: 500, yOffsetDU: 2900)
            let w = CoolScan.window(g, color: .red)
            let sent = CoolScan.windowDescriptor(w)
            print("\n— SET WINDOW (R) sender \(sent.count) byte —")
            print("  hex: \(sent.map { String(format: "%02x", $0) }.joined(separator: " "))")
            print(String(format: "  fields: resX=%d resY=%d xOff=%d yOff=%d w=%d h=%d depth=%d",
                         w.resX, w.resY, w.xOffset, w.yOffset, w.width, w.height, w.depth))

            let sr = try attempt("SET WINDOW") { try CoolScan.setWindow(session, w) }
            print("  SET WINDOW status: transport=\(sr.transportStatus) sbp=\(sr.sbpStatus) \(sr.ok ? "✅" : "❌")")
            xfer("SET WINDOW")   // data-OUT → expect targetREAD≈58 if delivery works

            let gr = try attempt("GET WINDOW") { try CoolScan.getWindowRaw(session, color: .red) }
            xfer("GET WINDOW")   // data-IN → expect targetWROTE≈58 if delivery works
            let b = [UInt8](gr.payload)
            print("\n— GET WINDOW (R) leste \(b.count) byte —")
            print("  hex: \(b.map { String(format: "%02x", $0) }.joined(separator: " "))")
            func be16(_ o: Int) -> Int { b.count > o+1 ? (Int(b[o])<<8)|Int(b[o+1]) : -1 }
            func be32(_ o: Int) -> Int { b.count > o+3 ? (Int(b[o])<<24)|(Int(b[o+1])<<16)|(Int(b[o+2])<<8)|Int(b[o+3]) : -1 }
            // Same offsets we wrote (descriptor body starts after the 8-byte header).
            print(String(format: "  parsed @SET-offsets: resX=%d resY=%d xOff=%d yOff=%d w=%d h=%d",
                         be16(10), be16(12), be32(14), be32(18), be32(22), be32(26)))
            let match = be16(10) == Int(w.resX) && be32(18) == Int(w.yOffset) && be32(22) == Int(w.width)
            print(match
                ? "\n✅ Data-OUT VIRKER — SET WINDOW-verdiene overlevde round-trip. Skann-no-op ligger et annet sted (LOAD/fokus/ready-poll)."
                : "\n⚠️  GET WINDOW matcher ikke det vi skrev — enten landet payloaden ikke, ELLER scanneren forkastet innholdet.")

            // DISCRIMINATOR: send a SET WINDOW with an IMPOSSIBLE resolution
            // (0xFFFF dpi). If the scanner READ our bytes it must reject this with
            // CHECK CONDITION (sense). If it returns GOOD, it never saw our payload
            // (read zeros → empty descriptor) = data-OUT delivery is broken.
            var bad = sent
            bad[10] = 0xFF; bad[11] = 0xFF   // resX = 65535 dpi — impossible
            print("\n— SET WINDOW (R) med UGYLDIG resX=0xFFFF (skiller levering fra innhold) —")
            let badCdb: [UInt8] = [0x24, 0, 0, 0, 0, 0, 0, 0, 0x3a, 0x00]
            let br = try attempt("SET WINDOW(bad)") {
                try session.sendSCSI(cdb: badCdb, direction: .toTarget,
                                     transferLength: UInt32(bad.count), outgoing: bad,
                                     captureSense: true)
            }
            print("  status: transport=\(br.transportStatus) sbp=\(br.sbpStatus) \(br.ok ? "GOOD" : "CHECK/feil")")
            xfer("SET WINDOW(bad)")   // data-OUT → did the target read these bytes?
            if !br.sense.isEmpty { print("  sense: \(br.sense.map { String(format: "%02x", $0) }.joined(separator: " "))") }
            print(br.ok
                ? "→ GOOD på umulig verdi ⇒ scanneren leste ALDRI payloaden vår (null-buffer) = DATA-OUT-LEVERING ER ØDELAGT i dext-en."
                : "→ CHECK CONDITION ⇒ scanneren LESTE bytene våre = data-OUT leverer; SET WINDOW-no-op er et innholds-/protokollproblem.")
        } catch {
            print("❌ windowcheck feilet: \(error)")
            return 8
        }
        return 0
    }

    // SCAN mode: «CoolScanProbe scan [dpi]» runs the minimal scan path and dumps
    // the raw byte stream for offline inspection. Default 500 dpi (fast, ~13 MB).
    if CommandLine.arguments.contains("scan") {
        let dpi = scanDpiArg() ?? 500
        let rows = scanRowsArg()   // optional thin-strip cap
        let yOff = scanYOffsetArg() ?? 0   // optional: start strip N device units down
        do {
            let caps = try CoolScan.capabilities(session)
            print("\nKapabiliteter: optisk \(caps.resXOptical) dpi, areal "
                + "\(caps.boundaryX)×\(caps.boundaryY) du, \(caps.maxBits) bit.")
            var g = CoolScan.geometry(caps, resolution: dpi, yOffsetDU: yOff)
            if let rows = rows, rows > 0, UInt32(rows) < g.logicalHeight {
                g.logicalHeight = UInt32(rows); g.heightDU = UInt32(rows) * g.pitchY
            }
            print("Skann @ \(g.realResX)×\(g.realResY) dpi → \(g.logicalWidth)×\(g.logicalHeight) px, "
                + "\(g.nColors) kanaler, \(g.bytesPerPixel)B/px → \(g.totalBytes) byte forventet"
                + (rows != nil ? " (stripe: \(rows!) rader)" : "")
                + (yOff > 0 ? " @ y-offset \(yOff) du" : "") + ".")
            var lastPct = -1
            let result = try CoolScan.scanFrame(session, caps: caps, resolution: dpi,
                                                rows: rows, yOffsetDU: yOff) { got, total in
                let pct = total > 0 ? got * 100 / total : 0
                if pct != lastPct { lastPct = pct; FileHandle.standardError.write(Data("\r  les \(pct)% (\(got)/\(total) byte)".utf8)) }
            }
            FileHandle.standardError.write(Data("\n".utf8))
            try saveScan(result)
            print(result.complete ? "✅ Skann komplett." : "⚠️  Kort skann (\(result.raw.count)/\(result.expectedBytes) byte).")
        } catch {
            print("❌ Skann feilet: \(error)")
            return 8
        }
        return 0
    }

    // Diagnostics (REQUEST SENSE + EVPD 0xC1) are opt-in: they can wedge the
    // command pipeline on some firmware, which then blocks a clean LOGOUT. Run
    // `CoolScanProbe diag` to enable. Default stays a clean login→inquiry→logout.
    guard CommandLine.arguments.contains("diag") else {
        print("\nFerdig (ren kjøring). Kjør «CoolScanProbe diag» for kapabilitets-dump.")
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
            + "\(r.ok ? "OK ✅" : "status \(r.transportStatus)/\(r.sbpStatus)") — \(r.payload.count) byte mottatt")
    } catch {
        print("\n⚠️  INQUIRY #2 (96B) feilet: \(error)")
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
                + "raw=\(hexDump(hdr.payload, max: 8)) — 9000 støtter kanskje ikke siden.")
            if !hdr.sense.isEmpty { print("   sense: \(hexDump(hdr.sense, max: 32))") }
            throw ProbeError("0xC1 header ikke brukbar")
        }
        let pageLen = Int(hb[3])
        let total = UInt8(min(4 + pageLen, 255))
        print("\nEVPD 0xC1: page_len-felt=\(pageLen) → leser \(total) byte")
        let r = try session.sendSCSI(cdb: [0x12, 0x01, 0xC1, 0x00, total, 0x00],
                                     direction: .fromTarget, transferLength: UInt32(total), timeoutMs: 3000)
        print("EVPD 0xC1 (Nikon-kapabiliteter): \(r.payload.count) byte")
        print("   raw: \(hexDump(r.payload, max: 256))")
        if let caps = CoolScan.parseCapabilities(r.payload) {
            print("""
               → optisk: \(caps.resXOptical)×\(caps.resYOptical) dpi  \
            maks: \(caps.resXMax)×\(caps.resYMax)  min: \(caps.resXMin)×\(caps.resYMin)
               → maks skanneareal: \(caps.boundaryX)×\(caps.boundaryY) px @ optisk  \
            (\(String(format: "%.2f", Double(caps.boundaryX)/Double(caps.resXOptical)))″×\
            \(String(format: "%.2f", Double(caps.boundaryY)/Double(caps.resYOptical)))″)
               → fokus: \(caps.focusMin)…\(caps.focusMax)  bit-dybde: \(caps.maxBits)  \
            rammer: \(caps.nFrames)
            """)
        } else {
            print("   ⚠️  for kort til å parse kapabilitetsfeltene.")
        }
    } catch {
        print("⚠️  EVPD 0xC1 feilet: \(error)")
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
        print("⚠️  REQUEST SENSE feilet: \(error)")
    }

    // 9) Bonus: TARGET RESET via task management (function 0x0F). Tests whether
    //    this clears the target's SBP-2 state so the NEXT login succeeds without
    //    a physical power-cycle (logout alone does not free the login).
    do {
        try conn.call(.submitSBP2TaskManagement, scalarIn: [session.handle, 0x0F])
        print("\nTARGET RESET (task mgmt 0x0F) sendt OK — sjekk om neste login går uten strømsyklus.")
    } catch {
        print("\n⚠️  TARGET RESET feilet: \(error)")
    }

    print("\nFerdig. Hvis INQUIRY viser «Nikon … LS-9000» er transportlaget bevist — neste steg er CoolScan-kommandosettet.")
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
    channels      : \(g.nColors)  (line-sequential planes, order R,G,B[,IR])
    bytes/pixel   : \(g.bytesPerPixel)  (depth \(g.depth) bit)
    bytes/line    : \(g.bytesPerLine)  (incl. \(g.oddPadding)B odd-padding per plane)
    expected bytes: \(g.totalBytes)
    received bytes: \(r.raw.count)\(r.complete ? "" : "  ⚠️ SHORT")

    Layout is UNCONFIRMED on hardware. To view: interpret as \(g.nColors) planes of
    \(g.logicalWidth)x\(g.logicalHeight), \(g.bytesPerPixel == 2 ? "16-bit big-endian" : "8-bit"). If it looks
    wrong, the colour interleave or byte order differs — adjust before writing TIFF.
    """
    try sidecar.write(to: URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        .appendingPathComponent(base + ".txt"), atomically: true, encoding: .utf8)
    print("💾 Lagret \(binURL.lastPathComponent) (\(r.raw.count) byte) + sidecar \(base).txt")
}

/// Minimal SCSI sense key/ASC decode for the messages we expect from the 9000.
func senseText(key: UInt8, asc: UInt8, ascq: UInt8) -> String {
    switch (key, asc, ascq) {
    case (0x0, _, _):        return "NO SENSE — klar"
    case (0x2, 0x04, _):     return "NOT READY (LOGICAL UNIT)"
    case (0x2, 0x3a, _):     return "MEDIUM NOT PRESENT — ingen film/holder"
    case (0x6, 0x29, _):     return "POWER ON / RESET (UA)"
    case (0x6, 0x28, _):     return "NOT READY→READY, medium isatt (UA)"
    case (0x6, 0x3f, _):     return "TARGET OPERATING CONDITIONS CHANGED (UA)"
    case (0x6, _, _):        return "UNIT ATTENTION (annen)"
    case (0x5, 0x20, _):     return "INVALID COMMAND OPCODE"
    case (0x5, 0x24, _):     return "INVALID FIELD IN CDB"
    case (0x5, 0x26, _):     return "INVALID FIELD IN PARAMETER LIST"
    case (0x5, _, _):        return "ILLEGAL REQUEST (annen)"
    case (_, 0x3e, _):       return "LU IKKE SELV-KONFIGURERT ENNÅ (scanner booter — vent)"
    case (0xb, _, _):        return "ABORTED COMMAND"
    default:                 return "ukjent — slå opp i SPC sense-tabell"
    }
}

exit(run())
