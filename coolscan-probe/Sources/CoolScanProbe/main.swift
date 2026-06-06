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

/// Minimal SCSI sense key/ASC decode for the messages we expect from the 9000.
func senseText(key: UInt8, asc: UInt8, ascq: UInt8) -> String {
    switch (key, asc, ascq) {
    case (0x0, _, _):        return "NO SENSE — klar"
    case (0x2, 0x04, _):     return "NOT READY (LOGICAL UNIT)"
    case (0x2, 0x3a, _):     return "MEDIUM NOT PRESENT — ingen film/holder"
    case (0x6, 0x29, _):     return "POWER ON / RESET"
    case (0x6, 0x28, _):     return "NOT READY TO READY CHANGE (medium isatt)"
    case (0x5, 0x20, _):     return "INVALID COMMAND OPCODE"
    case (0x5, 0x24, _):     return "INVALID FIELD IN CDB"
    default:                 return "ukjent — slå opp i SPC sense-tabell"
    }
}

exit(run())
