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

    print("\nFerdig. Hvis INQUIRY viser «Nikon … LS-9000» er transportlaget bevist — neste steg er CoolScan-kommandosettet.")
    return 0
}

exit(run())
