import Foundation

// CoolScan SCSI command set. Originally laid out from SANE coolscan3 (GPL);
// the scan path is now byte-exact against a DTrace capture of VueScan driving a
// real 9000 on Sequoia — see capture/DECODED.md. Where coolscan3 and the capture
// disagree, the capture wins. All multi-byte fields are BIG-ENDIAN (SCSI).

enum CoolScan {

    /// Device color ids: R, G, B, IR. One SET WINDOW + SEND LUT per id; one SCAN
    /// delivers all four channels (window list 01 02 03 09).
    enum Color: UInt8, CaseIterable { case red = 1, green = 2, blue = 3, infrared = 9 }

    // MARK: - Big-endian packing (SCSI)

    static func be16(_ v: UInt16) -> [UInt8] { [UInt8(v >> 8), UInt8(v & 0xff)] }
    static func be24(_ v: UInt32) -> [UInt8] { [UInt8((v >> 16) & 0xff), UInt8((v >> 8) & 0xff), UInt8(v & 0xff)] }
    static func be32(_ v: UInt32) -> [UInt8] {
        [UInt8((v >> 24) & 0xff), UInt8((v >> 16) & 0xff), UInt8((v >> 8) & 0xff), UInt8(v & 0xff)]
    }

    static func hexLine(_ b: [UInt8]) -> String {
        b.map { String(format: "%02x", $0) }.joined(separator: " ")
    }

    // MARK: - Status & lifecycle

    // Per-command ORB timeouts (probe-controlled; the dext uses timeoutMs as the
    // ORB timer). The default 5 s declared slow-but-alive commands dead: when the
    // scanner is mechanically busy (boot, AF, calibration) it simply doesn't
    // fetch/complete ORBs for many seconds, and timing out fires AGENT_RESET mid
    // operation — observed as the "wedge" (HW-run 12: agent recovered ~44 s
    // later both times). VueScan's stack just waits. So: wait.
    static let kSlowCmdTimeoutMs: UInt32 = 30_000
    static let kPollCmdTimeoutMs: UInt32 = 15_000

    static func testUnitReady(_ s: SBP2Session) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0x00, 0, 0, 0, 0, 0], direction: .none,
                       timeoutMs: kPollCmdTimeoutMs)
    }

    /// REQUEST SENSE — read status/error bits the scanner_ready loop polls on.
    static func requestSense(_ s: SBP2Session, length: UInt8 = 18) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0x03, 0, 0, 0, length, 0], direction: .fromTarget,
                       transferLength: UInt32(length), timeoutMs: kPollCmdTimeoutMs)
    }

    /// Decoded (key, ASC, ASCQ) from one REQUEST SENSE, or nil on empty/timeout.
    static func senseTriple(_ s: SBP2Session) -> (key: UInt8, asc: UInt8, ascq: UInt8)? {
        guard let r = try? requestSense(s) else { return nil }
        let b = [UInt8](r.payload)
        guard b.count >= 14 else { return nil }
        return (b[2] & 0x0f, b[12], b[13])
    }

    /// Poll until the scanner is TRULY ready (coolscan3's cs3_scanner_ready).
    /// TUR alone is not enough: the dext masks the SCSI status byte, so TUR's ok
    /// lies — REQUEST SENSE is the real verdict. key 0 = ready; key 6 = queued
    /// UNIT ATTENTION (power-on/holder events): drain and re-poll immediately,
    /// since a pending UA CHECK-CONDITIONs the next command without executing it.
    @discardableResult
    static func waitReady(_ s: SBP2Session, timeout: TimeInterval = 120) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            _ = try? testUnitReady(s)
            guard let t = senseTriple(s) else { usleep(500_000); continue }
            if t.key == 0 { return true }
            if t.key == 6 { usleep(120_000); continue }
            usleep(1_000_000)
        }
        return false
    }

    /// MODE SELECT(6) — set the base measurement unit (1/unitDpi inch).
    /// Rob Sims' LS-9000 backend (`cs9k_mode_select`) sends this once in
    /// `sane_open`, right after INQUIRY and before RESERVE. SET WINDOW
    /// offsets/extents are expressed and validated against the unit it
    /// establishes. Our DTrace capture never showed it ONLY because the trace
    /// attached AFTER VueScan had already opened the device — one-time open-time
    /// commands are invisible to a mid-session capture, so "VueScan never sends
    /// MODE SELECT" was a wrong inference. Leading suspect for the SET WINDOW
    /// 5/26 wall: without it the scanner validates our (otherwise byte-identical)
    /// field values against its power-on-default unit → INVALID FIELD IN PARAM LIST.
    static func modeSelect(_ s: SBP2Session, unitDpi: UInt16) throws -> SCSIResult {
        // 20-byte MODE SELECT(6) parameter list, byte-exact from coolscan3
        // cs3_mode_select. It is a well-formed SCSI parameter list:
        //   [0..3]   mode parameter header — byte 3 = block-descriptor length = 0x08
        //   [4..11]  8-byte block descriptor (ends in 0x01)
        //   [12..19] mode page: code 0x03, length 0x06, with unit_dpi at [16..17]
        // CRITICAL: the 0x08 is the block-descriptor-length and MUST sit at offset 3.
        // A prior version put it at offset 4 (an extra leading 0x00 shifted the whole
        // list one byte) → the header read BDL=0 and a page code 0x08 / length 0, which
        // the 9000 rejects with CHECK CONDITION 0x5/0x26 (INVALID FIELD IN PARAM LIST).
        var p: [UInt8] = [0x00, 0x00, 0x00, 0x08,                        // header (BDL = 8)
                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, // block descriptor
                          0x03, 0x06, 0x00, 0x00]                        // mode page 0x03, len 0x06
        p += be16(unitDpi)   // page data offset 2..3 → list offset 16..17
        p += [0x00, 0x00]    // list offset 18..19 → 20 bytes total
        return try s.sendSCSI(cdb: [0x15, 0x10, 0x00, 0x00, UInt8(p.count), 0x00],
                              direction: .toTarget, transferLength: UInt32(p.count), outgoing: p)
    }

    static func reserve(_ s: SBP2Session) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0x16, 0, 0, 0, 0, 0], direction: .none)
    }
    static func release(_ s: SBP2Session) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0x17, 0, 0, 0, 0, 0], direction: .none)
    }

    /// Replicate Rob Sims' LS-9000 `sane_open` device setup: MODE SELECT (set the
    /// measurement unit to 1/`unitDpi`″) then RESERVE UNIT. We dropped both earlier
    /// on the (mistaken) grounds that the VueScan capture never showed them — but
    /// they are open-time commands the mid-session capture could not see. This is
    /// the primary fix attempt for the SET WINDOW 5/26 wall: it establishes the
    /// unit context SET WINDOW's offsets/extents are validated against.
    /// Returns the MODE SELECT result (RESERVE is best-effort).
    @discardableResult
    static func establishUnit(_ s: SBP2Session, unitDpi: UInt16) throws -> SCSIResult {
        var r = try modeSelect(s, unitDpi: unitDpi)
        // coolscan9k reissues MODE SELECT once on sense 0x05/0x2c (COMMAND
        // SEQUENCE ERROR — "scanner found in middle of sequence"): the first
        // non-info command after a prior aborted run can land in that state.
        if !r.ok, r.scsiStatus == 0x02, r.sense.count >= 13,
           (r.sense[r.sense.startIndex + 2] & 0x0F) == 0x05,
           r.sense[r.sense.startIndex + 12] == 0x2c {
            r = try modeSelect(s, unitDpi: unitDpi)
        }
        _ = try? reserve(s)   // RESERVE(6) — coolscan9k reserves the unit at open
        return r
    }

    /// EXECUTE (vendor trigger) — issued after parameter-setting commands
    /// (set AF point / set focus). AF moves the carriage and can hold the ORB
    /// for a long time — give it minutes, not seconds.
    static func execute(_ s: SBP2Session) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0xc1, 0, 0, 0, 0, 0], direction: .none,
                       timeoutMs: 120_000)
    }

    /// Vendor 0xC0 — abort/idle. VueScan sends it after autofocus and at
    /// end-of-pass (after the last READ).
    static func abortIdle(_ s: SBP2Session) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0xc0, 0, 0, 0, 0, 0], direction: .none,
                       timeoutMs: kSlowCmdTimeoutMs)
    }

    private static func vendorWrite13(_ s: SBP2Session, sub: UInt8) throws -> SCSIResult {
        // e0 00 <sub> 00 00 00 00 00 0d 00  + 13 zero bytes
        try s.sendSCSI(cdb: [0xe0, 0x00, sub, 0, 0, 0, 0, 0, 0x0d, 0x00],
                       direction: .toTarget, transferLength: 13, outgoing: [UInt8](repeating: 0, count: 13))
    }
    static func load(_ s: SBP2Session) throws -> SCSIResult { try vendorWrite13(s, sub: 0xd1) }
    static func eject(_ s: SBP2Session) throws -> SCSIResult { try vendorWrite13(s, sub: 0xd0) }
    static func reset(_ s: SBP2Session) throws -> SCSIResult { try vendorWrite13(s, sub: 0x80) }

    // MARK: - Identification

    static func inquiry(_ s: SBP2Session, length: UInt8 = 96) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0x12, 0x00, 0x00, 0x00, length, 0x00],
                       direction: .fromTarget, transferLength: UInt32(length))
    }

    /// EVPD page inquiry. Page 0xC1 = Nikon capabilities (max res, boundaries, …).
    static func pageInquiry(_ s: SBP2Session, page: UInt8, length: UInt8) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0x12, 0x01, page, 0x00, length, 0x00],
                       direction: .fromTarget, transferLength: UInt32(length),
                       timeoutMs: kPollCmdTimeoutMs)
    }

    // MARK: - Capabilities (EVPD page 0xC1)

    /// Device capabilities decoded from the Nikon EVPD page 0xC1.
    ///
    /// Offsets are absolute into the full INQUIRY response (byte 0 = PDT), matching
    /// coolscan3's `cs3_full_inquiry`. All multi-byte fields are big-endian. The
    /// field map was verified byte-for-byte against a real CoolScan 9000 ED capture
    /// — see docs/coolscan9000-command-set.md.
    struct Capabilities {
        var resXOptical: UInt16   // [18..19]  — optical X res (dpi)
        var resXMax: UInt16       // [20..21]
        var resXMin: UInt16       // [22..23]
        var boundaryX: UInt32     // [36..39]  — max scan width  (device px @ optical)
        var resYOptical: UInt16   // [40..41]  — optical Y res (dpi)
        var resYMax: UInt16       // [42..43]
        var resYMin: UInt16       // [44..45]
        var boundaryY: UInt32     // [58..61]  — max scan height (device px @ optical)
        var nFrames: UInt8        // [75]      — 0 when no holder is inserted
        var focusMin: UInt16      // [76..77]
        var focusMax: UInt16      // [78..79]
        var maxBits: UInt8        // [82]      — max bit depth per channel

        /// Values reported by the CoolScan 9000 ED (rev 1.02) — the fallback used
        /// when the page can't be read, so geometry is never left undefined.
        static let coolScan9000 = Capabilities(
            resXOptical: 4000, resXMax: 4000, resXMin: 666,
            boundaryX: 10000, resYOptical: 4000, resYMax: 4000, resYMin: 333,
            boundaryY: 13860, nFrames: 0, focusMin: 0, focusMax: 450, maxBits: 16)
    }

    /// Parse the raw bytes returned by INQUIRY EVPD page 0xC1.
    /// Returns nil if the buffer is too short to hold every field.
    static func parseCapabilities(_ raw: Data) -> Capabilities? {
        let b = [UInt8](raw)
        guard b.count >= 83 else { return nil }
        func u16(_ i: Int) -> UInt16 { (UInt16(b[i]) << 8) | UInt16(b[i + 1]) }
        func u32(_ i: Int) -> UInt32 {
            (UInt32(b[i]) << 24) | (UInt32(b[i + 1]) << 16)
            | (UInt32(b[i + 2]) << 8) | UInt32(b[i + 3])
        }
        return Capabilities(
            resXOptical: u16(18), resXMax: u16(20), resXMin: u16(22),
            boundaryX: u32(36), resYOptical: u16(40), resYMax: u16(42),
            resYMin: u16(44), boundaryY: u32(58), nFrames: b[75],
            focusMin: u16(76), focusMax: u16(78), maxBits: b[82])
    }

    /// Read + parse the 0xC1 capability page in one call (header-first for length).
    static func capabilities(_ s: SBP2Session) throws -> Capabilities {
        let hdr = try pageInquiry(s, page: 0xC1, length: 5)
        let hb = [UInt8](hdr.payload)
        guard hdr.ok, hb.count >= 4 else { return .coolScan9000 }
        let total = UInt8(min(4 + Int(hb[3]), 255))
        let r = try pageInquiry(s, page: 0xC1, length: total)
        return parseCapabilities(r.payload) ?? .coolScan9000
    }

    // MARK: - Focus

    /// Set focus position, then `execute`. Captured payload puts the value in
    /// bytes 1..4 (leading zero byte) — `00 00 00 00 eb …` for focus 235.
    static func setFocus(_ s: SBP2Session, focus: UInt32) throws -> SCSIResult {
        let data: [UInt8] = [0x00] + be32(focus) + [0, 0, 0, 0]   // 9 bytes
        return try s.sendSCSI(cdb: [0xe0, 0x00, 0xc1, 0, 0, 0, 0, 0, 0x09, 0x00],
                              direction: .toTarget, transferLength: UInt32(data.count),
                              outgoing: data, timeoutMs: kSlowCmdTimeoutMs)
    }

    /// Vendor read e1/c1, 13 bytes — frame/holder info; VueScan reads it before
    /// every pass and it is the likely source of the SET WINDOW offsets. Dump raw
    /// until the layout is understood. Bytes 1..4 hold the focus position.
    static func readFrameInfo(_ s: SBP2Session) throws -> SCSIResult {
        // 30 s: these reads run right after mechanical ops; the 5 s default
        // ORB-timeout fired mid-settle and AGENT_RESET wedged the session
        // (run 17: AF-result, run 18: post-focus frame-info).
        try s.sendSCSI(cdb: [0xe1, 0x00, 0xc1, 0, 0, 0, 0, 0, 0x0d, 0x00],
                       direction: .fromTarget, transferLength: 13,
                       timeoutMs: kSlowCmdTimeoutMs)
    }

    /// Vendor read e1/c1, 9 bytes — AF result; VueScan reads it right after the
    /// autofocus EXECUTE completes. Focus position in bytes 1..4 (BE).
    static func readAFResult(_ s: SBP2Session) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0xe1, 0x00, 0xc1, 0, 0, 0, 0, 0, 0x09, 0x00],
                       direction: .fromTarget, transferLength: 9,
                       timeoutMs: kSlowCmdTimeoutMs)
    }

    /// EVPD page 0xC8 (197 B) — undecoded; VueScan polls it constantly (~200×
    /// in the capture), so it is probably unit/holder status. Control byte 0x80
    /// to match the captured CDB `12 01 c8 00 c5 80` exactly.
    static func readPageC8(_ s: SBP2Session) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0x12, 0x01, 0xc8, 0x00, 0xc5, 0x80],
                       direction: .fromTarget, transferLength: 0xc5,
                       timeoutMs: kPollCmdTimeoutMs)
    }

    /// READ FOCUS → current focus value (bytes 1..4 of the 13-byte response).
    static func readFocus(_ s: SBP2Session) throws -> UInt32 {
        let b = [UInt8](try readFrameInfo(s).payload)
        guard b.count >= 5 else { return 0 }
        return (UInt32(b[1]) << 24) | (UInt32(b[2]) << 16) | (UInt32(b[3]) << 8) | UInt32(b[4])
    }

    /// Set the autofocus point, then `execute`. Same 9-byte framing as setFocus:
    /// `00` + x + y (capture: `00 00 00 0b 68 00 00 0f 60`).
    static func autofocus(_ s: SBP2Session, x: UInt32, y: UInt32) throws -> SCSIResult {
        let data: [UInt8] = [0x00] + be32(x) + be32(y)
        return try s.sendSCSI(cdb: [0xe0, 0x00, 0xa0, 0, 0, 0, 0, 0, 0x09, 0x00],
                              direction: .toTarget, transferLength: UInt32(data.count),
                              outgoing: data, timeoutMs: kSlowCmdTimeoutMs)
    }

    // MARK: - SET WINDOW

    /// Geometry + acquisition parameters for one SET WINDOW call (device units,
    /// 1/4000″). One window per colour channel, same geometry in all four.
    struct Window {
        var color: Color
        var resX: UInt16
        var resY: UInt16
        var xOffset: UInt32         // ULX
        var yOffset: UInt32         // ULY
        var width: UInt32
        var height: UInt32
        var depth: UInt8            // bits per pixel (capture: 16)
        var negative: Bool
        var exposure: UInt32        // per channel; 0 for IR
    }

    /// Per-channel exposure from the capture's preview pass (R:G:B ≈ 1:2.5:3.5;
    /// the real-scan pass used exactly 6× these). IR is always 0.
    static func defaultExposure(_ c: Color) -> UInt32 {
        switch c {
        case .red: return 40_000
        case .green: return 100_000
        case .blue: return 140_000
        case .infrared: return 0
        }
    }

    /// Resolved scan geometry, following coolscan3's `cs3_convert_options`.
    /// SET WINDOW expresses offsets/extents in **device units** (1/`resXMax`″) and
    /// resolution as **absolute dpi** (which snaps to `resXMax / integer pitch`).
    struct ScanGeometry {
        var realResX: UInt16, realResY: UInt16   // snapped dpi sent in SET WINDOW
        var pitchX: UInt32, pitchY: UInt32        // resMax / realRes
        var xOffsetDU: UInt32, yOffsetDU: UInt32   // device units
        var widthDU: UInt32, heightDU: UInt32      // device units (window field)
        var logicalWidth: UInt32, logicalHeight: UInt32  // output pixels
        var depth: UInt8
        var nColors: Int

        var bytesPerPixel: Int { depth > 8 ? 2 : 1 }
        /// Each scanline carries all four channels: bytes_per_line = width_px × 8
        /// at 16 bit (validated by the capture's READ totals). Whether channels
        /// are line-sequential or pixel-interleaved within the line is unconfirmed.
        var oddPadding: Int { (Int(logicalWidth) * bytesPerPixel) % 2 == 1 ? 1 : 0 }
        var bytesPerLine: Int { nColors * (Int(logicalWidth) * bytesPerPixel + oddPadding) }
        var totalBytes: Int { bytesPerLine * Int(logicalHeight) }
    }

    /// Frame box VueScan used for the FH-835S 35mm holder (frame 1 in the
    /// capture): the 0xC1 caps frame size at a holder-specific offset. The offset
    /// likely comes from `readFrameInfo` — dump it and refine.
    static let captureFrame = (x: UInt32(165), y: UInt32(203), w: UInt32(4000), h: UInt32(5904))

    /// Resolve a scan area (in device units, 1/`resXMax`″) at a requested resolution
    /// into a `ScanGeometry`. Defaults to the full reported boundary. Always four
    /// channels (R, G, B, IR) — that is what the captured scans acquire.
    static func geometry(_ caps: Capabilities, resolution: UInt16,
                         depth: UInt8? = nil,
                         xOffsetDU: UInt32 = 0, yOffsetDU: UInt32 = 0,
                         widthDU: UInt32? = nil, heightDU: UInt32? = nil) -> ScanGeometry {
        let resMaxX = UInt32(max(caps.resXMax, 1)), resMaxY = UInt32(max(caps.resYMax, 1))
        // The caps "min" fields are real device limits: SET WINDOW below them is
        // rejected with CHECK CONDITION 5/26 (INVALID FIELD IN PARAMETER LIST) —
        // proven on HW with 500 dpi vs resXMin=666. VueScan's low-dpi pass used
        // exactly 666. Clamp up before snapping.
        let wanted = max(resolution, max(caps.resXMin, caps.resYMin))
        let pitchX = max(1, resMaxX / UInt32(max(wanted, 1)))
        let pitchY = max(1, resMaxY / UInt32(max(wanted, 1)))
        let realResX = UInt16(resMaxX / pitchX)        // snapped to a divisor of resMax
        let realResY = UInt16(resMaxY / pitchY)
        let extentX = widthDU ?? caps.boundaryX
        let extentY = heightDU ?? caps.boundaryY
        // Extents go on the wire unsnapped: the capture sends width=4000 du at
        // pitch 6 (not a multiple), and our pitch-aligned 3996 was the only
        // byte-level diff when SET WINDOW came back 5/26. Pixel counts follow
        // the scanner's own rounding, ceil(du × dpi / resMax) — validated
        // against both captured passes' READ totals.
        let logW = max(1, (extentX * UInt32(realResX) + resMaxX - 1) / resMaxX)
        let logH = max(1, (extentY * UInt32(realResY) + resMaxY - 1) / resMaxY)
        return ScanGeometry(
            realResX: realResX, realResY: realResY,
            pitchX: pitchX, pitchY: pitchY,
            xOffsetDU: xOffsetDU, yOffsetDU: yOffsetDU,
            widthDU: extentX, heightDU: extentY,
            logicalWidth: logW, logicalHeight: logH,
            depth: depth ?? caps.maxBits, nColors: 4)
    }

    /// Build the per-colour `Window` for a resolved geometry.
    static func window(_ g: ScanGeometry, color: Color, negative: Bool = false) -> Window {
        Window(color: color, resX: g.realResX, resY: g.realResY,
               xOffset: g.xOffsetDU, yOffset: g.yOffsetDU,
               width: g.widthDU, height: g.heightDU,
               depth: g.depth, negative: negative,
               exposure: defaultExposure(color))
    }

    /// Build the 58-byte SET WINDOW payload, byte-exact per the capture:
    /// 8-byte header + 50-byte descriptor (offset table in capture/DECODED.md).
    static func windowDescriptor(_ w: Window) -> [UInt8] {
        var d = [UInt8]()
        d += [0, 0, 0, 0, 0, 0, 0, 0x32]          // header, 0x32 = descriptor length
        d += [w.color.rawValue, 0x00]
        d += be16(w.resX)
        d += be16(w.resY)
        d += be32(w.xOffset)
        d += be32(w.yOffset)
        d += be32(w.width)
        d += be32(w.height)
        d += [0x00, 0x00, 0x00]                    // brightness/threshold/contrast
        d += [0x05]                                // image composition (multi-channel)
        d += [w.depth]                             // bits per pixel
        d += [UInt8](repeating: 0, count: 14)      // halftone/RIF/bit-order/compression + byte 40
        // Vendor block at descriptor bytes 41–45. The raw capture tail is
        // `00 81 01 02 02 ff` + be32 exposure in all 11 windows — byte 40 is
        // ZERO. DECODED.md's first table had the block one byte early (40–44);
        // 0x81 at byte 40 is what the scanner rejected with 5/26.
        // 0x81→0x80 for negatives per coolscan3 (unverified — capture was one film).
        d += [w.negative ? 0x80 : 0x81, 0x01, 0x02, 0x02, 0xff]
        d += be32(w.color == .infrared ? 0 : w.exposure)
        return d // 58 bytes
    }

    /// SET WINDOW — control byte (CDB[9]) is 0x80 on the 9000, per the capture.
    static func setWindow(_ s: SBP2Session, _ w: Window) throws -> SCSIResult {
        let data = windowDescriptor(w)
        let cdb: [UInt8] = [0x24, 0, 0, 0, 0, 0, 0, 0, 0x3a, 0x80]
        return try s.sendSCSI(cdb: cdb, direction: .toTarget,
                              transferLength: UInt32(data.count), outgoing: data,
                              timeoutMs: kSlowCmdTimeoutMs)
    }

    /// SET WINDOW with caller-supplied raw payload (8-byte header + descriptor).
    static func setWindowRaw(_ s: SBP2Session, _ data: [UInt8]) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0x24, 0, 0, 0, 0, 0, 0, 0, UInt8(data.count), 0x80],
                       direction: .toTarget, transferLength: UInt32(data.count),
                       outgoing: data, timeoutMs: kSlowCmdTimeoutMs)
    }

    /// Field-level fault isolation for SET WINDOW 5/26: echo the scanner's own
    /// GET WINDOW block back unchanged, then mutate one field group at a time
    /// toward the capture values. The first rejected rung names the culprit.
    /// Safe to run back-to-back — key-5 rejections are deterministic and leave
    /// the transport clean (HW-run 14).
    static func bisectSetWindow(_ s: SBP2Session, _ g: ScanGeometry) {
        guard let gw = try? getWindowRaw(s, color: .red), gw.payload.count == 58 else {
            print("  bisect: GET WINDOW utilgjengelig — hopper over")
            return
        }
        let defaultDesc = [UInt8](gw.payload.suffix(50))
        let header: [UInt8] = [0, 0, 0, 0, 0, 0, 0, 0x32]

        func patch16(_ d: inout [UInt8], _ i: Int, _ v: UInt16) {
            d[i] = UInt8(v >> 8); d[i + 1] = UInt8(v & 0xff)
        }
        func patch32(_ d: inout [UInt8], _ i: Int, _ v: UInt32) {
            d[i] = UInt8(v >> 24); d[i + 1] = UInt8((v >> 16) & 0xff)
            d[i + 2] = UInt8((v >> 8) & 0xff); d[i + 3] = UInt8(v & 0xff)
        }
        func verdict(_ label: String, _ desc: [UInt8]) {
            do {
                let r = try setWindowRaw(s, header + desc)
                print("  bisect \(label): \(r.ok ? "✅ AKSEPTERT" : "❌ \(r.statusText)")")
            } catch {
                print("  bisect \(label): ❌ \(error)")
            }
        }

        // Tail-delivery matrix (run 18). Bytes 56–57 of the 58-byte payload —
        // the exposure low half — are the ONLY meaningful bytes in a partial
        // quadlet anywhere in the command vocabulary (ORBs are 32 B, LUT 32768,
        // SCAN 4; setFocus' 9th byte is zero padding). If our read response
        // garbles the non-quadlet tail, the device sees a wrong exposure in
        // EVERY variant — invariant 5/26 — while a window whose exposure is
        // 0x00000000 (the IR window, VueScan-accepted) has nothing to corrupt.
        var full = defaultDesc
        patch16(&full, 2, g.realResX); patch16(&full, 4, g.realResY)
        patch32(&full, 6, g.xOffsetDU); patch32(&full, 10, g.yOffsetDU)
        patch32(&full, 14, g.widthDU); patch32(&full, 18, g.heightDU)
        full[25] = 0x05; full[41] = 0x81
        var dRed = full; patch32(&dRed, 46, defaultExposure(.red))
        var dRedZero = full; patch32(&dRedZero, 46, 0)
        var dIR = full; dIR[0] = Color.infrared.rawValue; patch32(&dIR, 46, 0)

        verdict("A IR-vindu (id 09, exp=0, null-hale)", dIR)
        verdict("B rød m/eksponering=0 (null-hale)   ", dRedZero)
        verdict("C ekko av default (referanse)       ", defaultDesc)
        verdict("D vår fulle røde (referanse)        ", dRed)
        print("  bisect-tolkning: A/B ✅ + C/D ❌ → ikke-quadlet-halen korrumperes i transporten;"
            + " alle ❌ → halen frikjent, gå til dext-logg")
    }

    /// GET WINDOW → read back the exposure for one color (bytes 54..57).
    static func getWindowExposure(_ s: SBP2Session, color: Color) throws -> UInt32 {
        let r = try s.sendSCSI(cdb: [0x25, 0x01, 0, 0, 0, color.rawValue, 0, 0, 0x3a, 0x00],
                               direction: .fromTarget, transferLength: 58)
        let b = [UInt8](r.payload)
        guard b.count >= 58 else { return 0 }
        return (UInt32(b[54]) << 24) | (UInt32(b[55]) << 16) | (UInt32(b[56]) << 8) | UInt32(b[57])
    }

    /// GET WINDOW → read back the full 58-byte window parameter block for one color.
    /// Used to verify that a preceding SET WINDOW (data-OUT) actually reached the
    /// scanner: the descriptor fields are at the same offsets we wrote them.
    static func getWindowRaw(_ s: SBP2Session, color: Color) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0x25, 0x01, 0, 0, 0, color.rawValue, 0, 0, 0x3a, 0x00],
                       direction: .fromTarget, transferLength: 58,
                       timeoutMs: kPollCmdTimeoutMs)
    }

    // MARK: - Scan & read

    /// SEND LUT (0x2a / byte2 0x03) — per-window gamma table, sent after SET
    /// WINDOW and before SCAN for every window id. VueScan sends a 14-bit-deep
    /// identity ramp: 16384 × 16-bit BE values 0x0000…0x3fff (32768 bytes).
    static func sendLUT(_ s: SBP2Session, color: Color) throws -> SCSIResult {
        var lut = [UInt8](); lut.reserveCapacity(2 * 16384)
        for v in 0..<16384 { lut += be16(UInt16(v)) }
        return try s.sendSCSI(cdb: [0x2a, 0x00, 0x03, 0x00, color.rawValue, 0x01, 0x00, 0x80, 0x00, 0x00],
                              direction: .toTarget, transferLength: UInt32(lut.count), outgoing: lut,
                              timeoutMs: kSlowCmdTimeoutMs)
    }

    /// SCAN — the window-id list goes as a small data-OUT payload (capture),
    /// NOT appended to the CDB as coolscan3 suggests.
    static func scan(_ s: SBP2Session, colors: [Color] = Color.allCases) throws -> SCSIResult {
        let ids = colors.map(\.rawValue)
        return try s.sendSCSI(cdb: [0x1b, 0, 0, 0, UInt8(ids.count), 0],
                              direction: .toTarget, transferLength: UInt32(ids.count),
                              outgoing: ids, timeoutMs: kSlowCmdTimeoutMs)
    }

    /// READ(10) — fetch `length` bytes of image data. Control byte 0x80, like
    /// SET WINDOW. The stream is plain bytes; chunks need not be line-aligned.
    static func readData(_ s: SBP2Session, length: UInt32,
                         timeoutMs: UInt32 = 5_000) throws -> SCSIResult {
        let cdb: [UInt8] = [0x28, 0, 0, 0, 0, 0] + be24(length) + [0x80]
        // captureSense:false — the dext duplicates payload into senseData when
        // captureSenseData is set, doubling the result size (and it isn't real
        // SCSI sense, just a copy of the payload).
        return try s.sendSCSI(cdb: cdb, direction: .fromTarget,
                              transferLength: length, timeoutMs: timeoutMs, captureSense: false)
    }

    // MARK: - Scan orchestration (cs3_scan, minimal proof-of-read path)

    /// Retry transient reset-per-ORB hiccups (~1 command in 10) on BOTH thrown
    /// errors and non-ok results — transport=-1/sbp=0xFF is the transient
    /// submit/resubmit failure, not a device verdict. A real CHECK CONDITION is
    /// masked by the dext anyway (shows as GOOD + sense), so nothing real is
    /// swallowed by retrying !ok. Same policy as the READ(10) loop.
    /// `diag` is appended to thrown errors — used to attach transfer-activity
    /// evidence (did the target actually fetch our data-OUT buffer?). `recover`
    /// runs between tries instead of a blind sleep — scanFrame passes a TUR
    /// ride-out so a retry waits until the device answers again (the agent has
    /// been observed to come back ~44 s after a transport drop).
    private static func attempt(_ label: String, tries: Int = 8,
                                diag: (() -> String)? = nil,
                                recover: (() -> Void)? = nil,
                                _ body: () throws -> SCSIResult) throws -> SCSIResult {
        var lastInfo = "ingen forsøk"
        for i in 1...tries {
            var rejected: String? = nil
            do {
                let r = try body()
                if r.ok { return r }
                lastInfo = r.statusText
                // ILLEGAL REQUEST (key 5) is a deterministic rejection of the
                // command's bytes — retrying the identical command just poisons
                // the pipe (observed: 4× 5/26 then timeout/submit-fail).
                if r.scsiStatus == 0x02, r.sense.count >= 14,
                   (r.sense[r.sense.startIndex + 2] & 0x0F) == 0x05 {
                    rejected = r.statusText
                }
            } catch { lastInfo = "\(error)" }
            if let rejected {
                throw ProbeError("\(label) avvist av skanneren: \(rejected)\(diag?() ?? "") — "
                    + "deterministisk (ILLEGAL REQUEST), ingen retry")
            }
            if i < tries {
                print("   ↻ \(label) forsøk \(i)/\(tries): \(lastInfo)")
                if let recover {
                    recover()
                } else {
                    usleep(UInt32(min(200_000 * i, 1_000_000)))
                }
            }
        }
        throw ProbeError("\(label) ga opp etter \(tries) forsøk: \(lastInfo)\(diag?() ?? "")")
    }

    /// Raw result of a scan: the geometry used plus the unprocessed byte stream
    /// exactly as the scanner delivered it.
    struct ScanResult {
        let geometry: ScanGeometry
        let raw: Data
        var expectedBytes: Int { geometry.totalBytes }
        var complete: Bool { raw.count >= expectedBytes }
    }

    /// Run the captured VueScan scan sequence: wait-ready → set focus → SET
    /// WINDOW ×4 → SEND LUT ×4 → SCAN → READ(10) loop → abort/idle. Returns the
    /// raw byte stream for offline inspection (channel interleave within a line
    /// is still unconfirmed — see ScanGeometry).
    ///
    /// `progress` is called as bytes arrive so the caller can show a bar.
    /// v1 skips autofocus and sets the captured focus position directly, exactly
    /// like VueScan's final scan pass did.
    static func scanFrame(_ s: SBP2Session, caps: Capabilities,
                          resolution: UInt16, depth: UInt8? = nil,
                          negative: Bool = false,
                          rows: Int? = nil,   // cap output height (thin-strip proof)
                          // Skip the AF preamble entirely and keep the fixed focus
                          // value. HW-run 13: every TUR right after EXECUTE (AF) hit
                          // the mechanically busy device → ORB timeout → AGENT_RESET
                          // mid-cycle → terminal wedge. nofocus avoids that path.
                          skipFocus: Bool = false,
                          focus: UInt32 = 235,   // captured value for the test slide
                          // Frame box in device units (1/resXMax″). Defaults to the
                          // captured 35mm frame-1 box until readFrameInfo is decoded.
                          xOffsetDU: UInt32 = captureFrame.x,
                          yOffsetDU: UInt32 = captureFrame.y,
                          widthDU: UInt32 = captureFrame.w,
                          heightDU: UInt32 = captureFrame.h,
                          // READ(10) results return via the user client's
                          // structureOutputDescriptor path (dext v13+): caps over
                          // 4096 B arrive as a memory descriptor the dext fills,
                          // so chunks can be VueScan-sized (~510 KB → a handful of
                          // READs per frame instead of ~1440 fragile
                          // AGENT_RESET→resubmit round-trips). The loop floors the
                          // chunk to whole lines. Inline fallback (≤2048) still
                          // works against an old dext.
                          maxChunkBytes: Int = 480_000,
                          readTimeoutMs: UInt32 = 30_000,
                          bootSettleSeconds: Int = 25,
                          progress: ((Int, Int) -> Void)? = nil) throws -> ScanResult {
        var g = geometry(caps, resolution: resolution, depth: depth,
                         xOffsetDU: xOffsetDU, yOffsetDU: yOffsetDU,
                         widthDU: widthDU, heightDU: heightDU)
        if let rows = rows, rows > 0, UInt32(rows) < g.logicalHeight {
            g.logicalHeight = UInt32(rows)
            g.heightDU = UInt32(rows) * g.pitchY
        }

        let t0 = Date()
        func step(_ msg: String) { print(String(format: "  [%5.1fs] %@", Date().timeIntervalSince(t0), msg)) }

        // 0) MODE SELECT + RESERVE now happen in step 1a (after the scanner reports
        //    ready) — they are open-time commands the capture could not see, and
        //    re-adding them is the primary fix attempt for the SET WINDOW 5/26 wall.

        // 0b) Let the scanner finish its post-power-cycle boot before the first
        //     AGENT_RESET-routed command. The ORBs in main (INQUIRY/TUR/caps)
        //     ride the agent's post-login RESET state and answer reliably even
        //     mid-boot, but waitReady's TUR goes through AGENT_RESET→AGENT_STATE
        //     poll, which wedges if the target is still mechanically busy
        //     (HW-run 21: early waitReady wedge right after a power cycle, UA
        //     0x29 power-on still pending → ORB hangs ~44 s, commandInFlight locked).
        if bootSettleSeconds > 0 {
            step("venter \(bootSettleSeconds)s på at skanneren fullfører boot (unngår tidlig AGENT_RESET-wedge)")
            Thread.sleep(forTimeInterval: TimeInterval(bootSettleSeconds))
        }

        // 1) Wait until the scanner reports ready (film loaded, lamp warm).
        guard waitReady(s, timeout: 120) else { throw ProbeError("scanner ikke klar (sense-timeout)") }
        step("scanner klar")

        // 1a) Establish the measurement unit + reservation BEFORE any window/read
        //     ops — exactly as Rob Sims' LS-9000 backend does at sane_open
        //     (INQUIRY → MODE SELECT → RESERVE). The DTrace capture missed these
        //     (open-time, before the trace attached), so we wrongly dropped them.
        //     Without MODE SELECT the scanner validates SET WINDOW's fields against
        //     its power-on-default unit → 5/26. This is the primary fix attempt for
        //     that wall: if SET WINDOW now succeeds, the missing unit was the cause.
        //     Done after waitReady so a still-booting device can't wedge it.
        do {
            let mr = try establishUnit(s, unitDpi: max(caps.resXMax, 1))
            step("MODE SELECT (enhet 1/\(caps.resXMax)″) + RESERVE: \(mr.ok ? "ok ✅" : "❌ \(mr.statusText)")")
        } catch {
            print("  ⚠️ MODE SELECT/RESERVE feilet: \(error) — fortsetter (SET WINDOW gir uansett sitt eget verdikt)")
        }

        // 1b) Post-ready state dumps. The boot-time reads in main happen while
        //     the scanner still reports UA/not-ready and return zeros — these
        //     are the trustworthy ones.
        if let c = try? capabilities(s) {
            step("caps etter klar: nFrames=\(c.nFrames) fokus=\(c.focusMin)–\(c.focusMax)")
            if c.nFrames == 0 {
                print("  ⚠️ nFrames=0 — skanneren melder INGEN holder/rammer (caps byte 75);"
                    + " SET WINDOW kan avvises på det")
            }
        }
        if let fi = try? readFrameInfo(s) {
            print("  Frame-info etter klar: \(hexLine([UInt8](fi.payload))) (\(fi.statusText))")
        }
        if let c8 = try? readPageC8(s) {
            print("  EVPD C8 (\(c8.payload.count)B, \(c8.statusText)): \(hexLine([UInt8](c8.payload)))")
        }
        // GET WINDOW is our own addition (0x25 never appears in the capture) —
        // keep it out of the AF→SET WINDOW stretch so that stretch is pure
        // VueScan vocabulary.
        if let gw = try? getWindowRaw(s, color: .red) {
            print("  GET WINDOW(red) før preamble (\(gw.statusText)): \(hexLine([UInt8](gw.payload)))")
        }

        // 2) Capture preamble, replicated verbatim from the preview pass: set AF
        //    point (center) → EXECUTE → frame-info → AF result → c0 → set focus
        //    → EXECUTE. Autofocus has run before the FIRST SET WINDOW of every
        //    captured session, so it may be required state — payload bytes and
        //    focus-only state are already eliminated (HW-runs 8/9). Best-effort:
        //    AF failures are printed, not fatal, so SET WINDOW below always
        //    reports its own verdict. `skipFocus` keeps the captured focus value
        //    instead of adopting the AF result (like VueScan's final scan pass).
        // Between tries: ride TUR until the device answers again instead of a
        // blind sleep — a dropped transport has been observed to need ~44 s.
        let ride: () -> Void = { _ = waitReady(s, timeout: 60) }
        // Any command issued while the mechanics move risks ORB timeout →
        // AGENT_RESET mid-cycle → terminal wedge (HW-run 13). VueScan survived
        // this window via doorbell-chaining; with reset-per-ORB the only safe
        // wait is a command-free sleep before the first TUR.
        let settle: (TimeInterval, String) -> Void = { secs, what in
            step("venter passivt \(Int(secs))s etter \(what) (ingen kommandoer mens mekanikken jobber)")
            Thread.sleep(forTimeInterval: secs)
        }
        var appliedFocus = focus
        // Every captured pass runs a full AF cycle in its prolog (AF at line
        // 4673 before the preview windows, again at 12873 before the final
        // pass), and VueScan waits a long poll stretch after the c0 before
        // touching focus. Replicate unconditionally — skipFocus only keeps the
        // fixed focus value instead of adopting the AF result (capture sent
        // 235 here too).
        if skipFocus {
            step("nofocus: hopper over AF-syklusen (AF frikjent for 5/26 i HW-run 17, EXECUTE(AF) wedger reset-per-ORB) — fast fokus \(appliedFocus)")
        } else { do {
            _ = try attempt("SET AF-PUNKT", tries: 3, recover: ride) { try autofocus(s, x: 2920, y: 3936) }
            _ = try attempt("EXECUTE (AF)", tries: 3, recover: ride) { try execute(s) }
            settle(30, "EXECUTE (AF)")
            let afReady = waitReady(s, timeout: 120)
            step(afReady ? "autofokus ferdig" : "⚠️ ikke klar etter AF — fortsetter")
            if let fi = try? readFrameInfo(s) {
                print("  Frame-info etter AF: \(hexLine([UInt8](fi.payload)))")
            }
            if let af = try? readAFResult(s) {
                let b = [UInt8](af.payload)
                print("  AF-resultat (9B): \(hexLine(b)) (\(af.statusText))")
                if !skipFocus, b.count >= 5 {
                    let v = (UInt32(b[1]) << 24) | (UInt32(b[2]) << 16)
                          | (UInt32(b[3]) << 8) | UInt32(b[4])
                    if v > 0 && v <= 450 { appliedFocus = v }
                }
            }
            _ = try? abortIdle(s)
            settle(10, "c0 (abort/idle)")
            _ = waitReady(s, timeout: 60)
        } catch {
            print("  ⚠️ AF-preamble feilet: \(error) — fortsetter med fast fokus")
        } }
        if let fi = try? readFrameInfo(s) {
            print("  Frame-info før setFocus: \(hexLine([UInt8](fi.payload)))")
        }
        _ = try attempt("SET FOCUS", recover: ride) { try setFocus(s, focus: appliedFocus) }
        _ = try attempt("EXECUTE (fokus)", recover: ride) { try execute(s) }
        settle(15, "EXECUTE (fokus)")
        let fReady = waitReady(s, timeout: 60)
        step(fReady ? "fokus satt (\(appliedFocus))" : "⚠️ ikke klar etter fokus — fortsetter")
        // (Post-focus frame-info readback removed: data-OUT integrity is proven
        // twice — byte4 read back 0xeb in runs 16/17 — and the extra read sat as
        // one more wedge-roulette draw right before SET WINDOW; run 18 died on it.)

        // 3) One window per colour channel, then a LUT per channel — VueScan's
        //    order: all four SET WINDOW first, then all four SEND LUT.
        let colors: [Color] = Color.allCases
        let xferDiag: () -> String = {
            guard let t = s.lastTransferInfo() else { return " [xfer utilgjengelig]" }
            return " [target leste \(t.targetReadBytes)B/\(t.targetReadCalls)x av \(t.expectedBytes)B forventet]"
        }
        print("  SET WINDOW payload (red): "
            + hexLine(windowDescriptor(window(g, color: .red, negative: negative))))
        do {
            for c in colors {
                _ = try attempt("SET WINDOW (\(c))", diag: xferDiag, recover: ride) {
                    try setWindow(s, window(g, color: c, negative: negative))
                }
                step("SET WINDOW \(c) ok")
            }
        } catch {
            print("  → SET WINDOW avvist — bisekterer felt mot GET WINDOW-defaulten:")
            bisectSetWindow(s, g)
            throw error
        }
        for c in colors {
            _ = try attempt("SEND LUT (\(c))", recover: ride) { try sendLUT(s, color: c) }
            step("SEND LUT \(c) ok (32768B)")
        }

        // 4) Trigger the scan (window-id list as data-OUT payload).
        let scanResult = try attempt("SCAN", recover: ride) { try scan(s, colors: colors) }
        step("SCAN akseptert (\(scanResult.statusText)) — leser data")

        // 5) Pull the image as a byte stream. Whole-line chunks like VueScan
        //    (~510 KB ≈ 100+ lines per READ); layout is reconstructed offline
        //    from the total byte count.
        let lineBytes = max(1, g.bytesPerLine)
        var alignedChunk = max(1, maxChunkBytes)
        if alignedChunk >= lineBytes {
            alignedChunk -= alignedChunk % lineBytes
        }
        let chunkBytes = UInt32(alignedChunk)
        var raw = Data(capacity: g.totalBytes)
        while raw.count < g.totalBytes {
            let remaining = g.totalBytes - raw.count
            let want = UInt32(min(Int(chunkBytes), remaining))
            // Each non-first ORB resets the fetch agent before ORB_POINTER; under
            // back-to-back reads the submit/resubmit transiently fails (selector-59
            // kIOReturnError, transport=-1/sbp=0xFF, or a hang→timeout). Retry the
            // whole command — on a thrown error OR a non-ok result — with an
            // escalating settle delay, so the scan limps through a flaky path.
            var ok: SCSIResult? = nil
            var attempt = 0
            var lastInfo = ""
            // Mid-scan the device goes quiet while the carriage moves/buffers
            // (stops fetching ORBs well past our old 3 s window) — be patient.
            while attempt < 30 {
                do {
                    let r = try readData(s, length: want, timeoutMs: readTimeoutMs)
                    if r.ok { ok = r; break }
                    lastInfo = r.statusText
                } catch {
                    lastInfo = "\(error)"
                }
                attempt += 1
                usleep(UInt32(min(100_000 * attempt, 1_000_000))) // 100ms … 1s cap
            }
            guard let r = ok else {
                // Keep what we got — even a few lines is enough to validate the
                // pixel layout offline. The caller sees complete=false.
                print("\n  ⚠️ READ(10) ga opp ved \(raw.count)/\(g.totalBytes) byte etter \(attempt) forsøk: \(lastInfo) — lagrer partial")
                break
            }
            if r.payload.isEmpty { break } // short read — scanner says done
            // Discriminator for the zero-pixel mystery: targetWROTE == payload
            // size means the scanner really sent these bytes (zeros = scanner-
            // side); targetWROTE == 0 means the data phase never happened and
            // we are reading back our own zeroed buffer (transport-side).
            let nonZero = r.payload.contains { $0 != 0 }
            var xferNote = ""
            if let t = s.lastTransferInfo() {
                xferNote = " targetWROTE=\(t.targetWroteBytes)B/\(t.targetWroteCalls)x"
            }
            print("  READ \(r.payload.count)B \(r.statusText)\(xferNote) "
                + (nonZero ? "✅ ekte data" : "⚠️ kun nuller"))
            raw.append(r.payload)
            progress?(raw.count, g.totalBytes)
        }

        // 6) End-of-pass abort/idle, like VueScan.
        _ = try? abortIdle(s)
        return ScanResult(geometry: g, raw: raw)
    }
}
