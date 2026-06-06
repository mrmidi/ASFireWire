import Foundation

// CoolScan SCSI command set — skeleton ported from SANE coolscan3 (GPL).
// Every operation is a raw CDB sent through SBP2Session.sendSCSI. All multi-byte
// fields are BIG-ENDIAN (SCSI convention). See docs/coolscan9000-command-set.md.
//
// Status: command builders are laid out per the coolscan3 reference but NOT yet
// validated against hardware. Items marked TODO need the real 9000 to confirm.

enum CoolScan {

    /// Device color ids (cs3_colors): R, G, B, IR.
    enum Color: UInt8 { case red = 1, green = 2, blue = 3, infrared = 9 }

    /// Scan "kind" byte in the SET WINDOW descriptor (offset 50).
    enum ScanKind: UInt8 { case normal = 0x01, autoExposure = 0x20, autoExposureWB = 0x40 }

    // MARK: - Big-endian packing (SCSI)

    static func be16(_ v: UInt16) -> [UInt8] { [UInt8(v >> 8), UInt8(v & 0xff)] }
    static func be24(_ v: UInt32) -> [UInt8] { [UInt8((v >> 16) & 0xff), UInt8((v >> 8) & 0xff), UInt8(v & 0xff)] }
    static func be32(_ v: UInt32) -> [UInt8] {
        [UInt8((v >> 24) & 0xff), UInt8((v >> 16) & 0xff), UInt8((v >> 8) & 0xff), UInt8(v & 0xff)]
    }

    // MARK: - Status & lifecycle

    static func testUnitReady(_ s: SBP2Session) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0x00, 0, 0, 0, 0, 0], direction: .none)
    }

    /// REQUEST SENSE — read status/error bits the scanner_ready loop polls on.
    static func requestSense(_ s: SBP2Session, length: UInt8 = 18) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0x03, 0, 0, 0, length, 0], direction: .fromTarget,
                       transferLength: UInt32(length))
    }

    /// Poll TEST UNIT READY until it succeeds (coolscan3 scanner_ready, simplified).
    /// TODO: decode REQUEST SENSE status bits (NO_DOCS / READY) for the 9000.
    @discardableResult
    static func waitReady(_ s: SBP2Session, timeout: TimeInterval = 120) throws -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if try testUnitReady(s).ok { return true }
            usleep(1_000_000) // 1 s, matching coolscan3
        }
        return false
    }

    static func reserve(_ s: SBP2Session) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0x16, 0, 0, 0, 0, 0], direction: .none)
    }
    static func release(_ s: SBP2Session) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0x17, 0, 0, 0, 0, 0], direction: .none)
    }

    /// EXECUTE (vendor trigger) — issued after parameter-setting commands.
    static func execute(_ s: SBP2Session) throws -> SCSIResult {
        try s.sendSCSI(cdb: [0xc1, 0, 0, 0, 0, 0], direction: .none)
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
                       direction: .fromTarget, transferLength: UInt32(length))
    }

    // MARK: - Focus

    static func setFocus(_ s: SBP2Session, focus: UInt32) throws -> SCSIResult {
        var data = be32(focus); data += [0, 0, 0, 0]   // 9-byte payload total
        return try s.sendSCSI(cdb: [0xe0, 0x00, 0xc1, 0, 0, 0, 0, 0, 0x09, 0x00],
                              direction: .toTarget, transferLength: UInt32(data.count), outgoing: data)
    }

    /// READ FOCUS → returns the current focus value (13-byte response).
    static func readFocus(_ s: SBP2Session) throws -> UInt32 {
        let r = try s.sendSCSI(cdb: [0xe1, 0x00, 0xc1, 0, 0, 0, 0, 0, 0x0d, 0x00],
                               direction: .fromTarget, transferLength: 13)
        let b = [UInt8](r.payload)
        guard b.count >= 5 else { return 0 }
        return (UInt32(b[1]) << 24) | (UInt32(b[2]) << 16) | (UInt32(b[3]) << 8) | UInt32(b[4])
    }

    static func autofocus(_ s: SBP2Session, x: UInt32, y: UInt32) throws -> SCSIResult {
        let data = be32(x) + be32(y)
        return try s.sendSCSI(cdb: [0xe0, 0x00, 0xa0, 0, 0, 0, 0, 0, 0x09, 0x00],
                              direction: .toTarget, transferLength: UInt32(data.count), outgoing: data)
    }

    // MARK: - SET WINDOW

    /// Geometry + acquisition parameters for one SET WINDOW call (device units).
    struct Window {
        var color: Color
        var resX: UInt16
        var resY: UInt16
        var xOffset: UInt32
        var yOffset: UInt32
        var width: UInt32
        var height: UInt32
        var depth: UInt8            // 8 / 14 / 16
        var samplesPerScan: UInt8   // multi-sampling (1 = single)
        var negative: Bool
        var kind: ScanKind
        var exposure: UInt32        // ×10 ns; ignored for IR
        /// LS40/LS4000/LS50/LS5000 set CDB byte9 = 0x80. 8000/9000 use 0x00.
        var ls40Family: Bool = false
    }

    /// Build the 58-byte SET WINDOW descriptor (see doc table for offsets).
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
        d += [0x00, 0x00, 0x00]                    // brightness/contrast
        d += [0x05]                                // image composition
        d += [w.depth]                             // pixel/bit depth
        d += [UInt8](repeating: 0, count: 13)
        d += [(w.samplesPerScan &- 1) << 4]        // multiread/ordering
        d += [w.negative ? UInt8(0x80) : UInt8(0x81)] // averaging + pos/neg
        d += [w.kind.rawValue]                     // scan kind
        d += [w.samplesPerScan == 1 ? 0x02 : 0x10] // single / multi
        d += [0x02]                                // color interleaving
        d += [0xff]                                // (AE)
        if w.color == .infrared { d += [0, 0, 0, 0] } else { d += be32(w.exposure) }
        return d // 58 bytes
    }

    static func setWindow(_ s: SBP2Session, _ w: Window) throws -> SCSIResult {
        let data = windowDescriptor(w)
        let cdb: [UInt8] = [0x24, 0, 0, 0, 0, 0, 0, 0, 0x3a, w.ls40Family ? 0x80 : 0x00]
        return try s.sendSCSI(cdb: cdb, direction: .toTarget,
                              transferLength: UInt32(data.count), outgoing: data)
    }

    /// GET WINDOW → read back the exposure for one color (bytes 54..57).
    static func getWindowExposure(_ s: SBP2Session, color: Color) throws -> UInt32 {
        let r = try s.sendSCSI(cdb: [0x25, 0x01, 0, 0, 0, color.rawValue, 0, 0, 0x3a, 0x00],
                               direction: .fromTarget, transferLength: 58)
        let b = [UInt8](r.payload)
        guard b.count >= 58 else { return 0 }
        return (UInt32(b[54]) << 24) | (UInt32(b[55]) << 16) | (UInt32(b[56]) << 8) | UInt32(b[57])
    }

    // MARK: - Scan & read

    /// SCAN — lists the colour channels to acquire (RGB or RGBI).
    static func scan(_ s: SBP2Session, infrared: Bool) throws -> SCSIResult {
        let cdb: [UInt8] = infrared
            ? [0x1b, 0, 0, 0, 0x04, 0, 0x01, 0x02, 0x03, 0x09]
            : [0x1b, 0, 0, 0, 0x03, 0, 0x01, 0x02, 0x03]
        return try s.sendSCSI(cdb: cdb, direction: .none)
    }

    /// READ(10) — fetch `length` bytes of image data. For multi-sampling the caller
    /// must pre-multiply length by samplesPerScan (coolscan3 does this).
    static func readData(_ s: SBP2Session, length: UInt32) throws -> SCSIResult {
        let cdb: [UInt8] = [0x28, 0, 0, 0, 0, 0] + be24(length) + [0x00]
        return try s.sendSCSI(cdb: cdb, direction: .fromTarget,
                              transferLength: length, timeoutMs: 30_000)
    }

    // TODO: sendLUT (0x2a 00 03 …), setBoundary (0x2a 00 88 …), modeSelect (0x15 …)
    //       — needed for full scans; deferred until the read path is proven.
}
