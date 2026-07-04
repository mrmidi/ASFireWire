import Foundation
import IOKit

/// Thin wrapper around the ASFireWire DriverKit user client.
///
/// All selector numbers, wire-format layouts, and the service name are taken
/// directly from the ASFireWire sources:
///   - ASFWDriver/UserClient/Core/ASFWDriverUserClient.iig  (selector enum)
///   - ASFWDriver/UserClient/WireFormats/*.hpp              (host-endian records)
///   - ASFW/ASFWDriverConnector.swift                       (service name + open)
///
/// The records exchanged with the dext are native-endian ("host") structs, NOT
/// the big-endian SBP-2 bus wire format — that conversion happens inside the dext.
enum ASFW {
    static let serviceName = "ASFWDriver"

    /// ExternalMethod selectors (subset we need for SBP-2 / SCSI passthrough).
    /// Values MUST match ASFWDriver/UserClient/Core/ASFWDriverUserClient.iig on
    /// the dext we talk to. Aligned to mrmidi/main's Session/ architecture (the
    /// SBP-2 block is 52–60 there, NOT the 53–61 of the old coolscan-9000 dext).
    enum Selector: UInt32 {
        case getDiscoveredDevices    = 16
        case createSBP2Session       = 52
        case startSBP2Login          = 53
        case getSBP2SessionState     = 54
        case submitSBP2Inquiry       = 55
        case getSBP2InquiryResult    = 56
        case submitSBP2Command       = 57
        case getSBP2CommandResult    = 58
        case submitSBP2TaskManagement = 59
        case releaseSBP2Session      = 60
        // Not present on main's dext (was a coolscan-9000-only diagnostic). Left
        // dormant: lastTransferInfo() calls it best-effort and degrades to nil.
        // 62 is unused on main, so no collision. Re-add to the dext to revive it.
        case getSBP2TransferInfo     = 62
    }

    /// SBP-2 login state (mirror of ASFW::Protocols::SBP2::LoginState).
    enum LoginState: UInt64 {
        case idle = 0, loggingIn = 1, loggedIn = 2, reconnecting = 3
        case loggingOut = 4, suspended = 5, failed = 6, unknown = 255

        init(raw: UInt64) { self = LoginState(rawValue: raw) ?? .unknown }
        var label: String {
            switch self {
            case .idle: return "Idle"; case .loggingIn: return "LoggingIn"
            case .loggedIn: return "LoggedIn"; case .reconnecting: return "Reconnecting"
            case .loggingOut: return "LoggingOut"; case .suspended: return "Suspended"
            case .failed: return "Failed"; case .unknown: return "Unknown"
            }
        }
    }

    /// Data-transfer direction for a SCSI command (mirror of SCSI::DataDirection).
    enum DataDirection: UInt8 {
        case none = 0, fromTarget = 1, toTarget = 2
    }

    /// SBP-2 unit signature in the Config ROM unit directory. Any IEEE-1394
    /// SBP-2 target (storage, scanner, ...) advertises these standard values.
    static let sbp2SpecId: UInt32 = 0x00609E
    static let sbp2SwVersion: UInt32 = 0x010483
}

/// Errors surfaced by the bridge. `kr` carries the raw kern_return for callers
/// that need to react to specific codes (e.g. retry on a busy dext slot).
struct ProbeError: Error, CustomStringConvertible {
    let description: String
    let kr: kern_return_t?
    init(_ m: String, kr: kern_return_t? = nil) { description = m; self.kr = kr }
}

/// Live connection to the ASFireWire dext user client.
final class ASFWConnection {
    let connection: io_connect_t

    init() throws {
        let matching = IOServiceNameMatching(ASFW.serviceName)
        let service = IOServiceGetMatchingService(kIOMainPortDefault, matching)
        guard service != 0 else {
            throw ProbeError("""
                IOService "\(ASFW.serviceName)" not found.
                Is the ASFireWire dext loaded? Check: systemextensionsctl list
                """)
        }
        defer { IOObjectRelease(service) }

        var conn: io_connect_t = 0
        let kr = IOServiceOpen(service, mach_task_self_, 0, &conn)
        guard kr == KERN_SUCCESS else {
            throw ProbeError("""
                IOServiceOpen failed: \(Self.decode(kr)).
                (0x\(String(format: "%08x", UInt32(bitPattern: kr))))
                KERN_PROTECTION_FAILURE here usually means the client lacks
                access to the user client — run with SIP off + developer mode,
                or fold the probe into the ASFW app, which already has access.
                """)
        }
        connection = conn
    }

    deinit { if connection != 0 { IOServiceClose(connection) } }

    // MARK: - Generic ExternalMethod call

    /// Calls an ExternalMethod with optional scalar/struct input and output.
    /// Retries struct output once on kIOReturnNoSpace (mirrors the GUI transport).
    @discardableResult
    func call(_ sel: ASFW.Selector,
              scalarIn: [UInt64] = [],
              structIn: Data? = nil,
              scalarOutCount: Int = 0,
              structOutCap: Int = 0) throws -> (scalarOut: [UInt64], structOut: Data) {

        var scalarOut = [UInt64](repeating: 0, count: scalarOutCount)
        var scalarOutCnt = UInt32(scalarOutCount)

        func invoke(_ cap: Int) -> (kern_return_t, Data) {
            var structOut = Data(count: cap)
            var structOutSize = cap
            let kr: kern_return_t = scalarIn.withUnsafeBufferPointer { sin in
                withStructIn(structIn) { (sinPtr, sinLen) in
                    structOut.withUnsafeMutableBytes { sout in
                        scalarOut.withUnsafeMutableBufferPointer { sout64 in
                            IOConnectCallMethod(
                                connection, sel.rawValue,
                                scalarIn.isEmpty ? nil : sin.baseAddress, UInt32(scalarIn.count),
                                sinPtr, sinLen,
                                scalarOutCount == 0 ? nil : sout64.baseAddress, &scalarOutCnt,
                                cap == 0 ? nil : sout.baseAddress, &structOutSize)
                        }
                    }
                }
            }
            if cap != 0 { structOut.count = min(structOutSize, cap) }
            return (kr, cap == 0 ? Data() : structOut)
        }

        var (kr, out) = invoke(structOutCap)
        if kr == kIOReturnNoSpace && structOutCap != 0 {
            // dext told us the real size via structOutSize; retry bigger.
            (kr, out) = invoke(max(structOutCap * 4, 256 * 1024))
        }
        guard kr == KERN_SUCCESS else {
            throw ProbeError("\(sel) (selector \(sel.rawValue)) failed: \(Self.decode(kr))", kr: kr)
        }
        return (Array(scalarOut.prefix(Int(scalarOutCnt))), out)
    }

    /// Single IOConnectCallMethod with NO NoSpace-retry. Returns the raw
    /// kern_return, the struct-output size the dext reported, and the data.
    /// Used to diagnose struct-output marshalling (e.g. NoSpace → out-of-line).
    func callOnceStructOut(_ sel: ASFW.Selector, scalarIn: [UInt64],
                           structOutCap: Int) -> (kr: kern_return_t, reportedSize: Int, data: Data) {
        var scalarOutCnt: UInt32 = 0
        var structOut = Data(count: structOutCap)
        var structOutSize = structOutCap
        let kr: kern_return_t = scalarIn.withUnsafeBufferPointer { sin in
            structOut.withUnsafeMutableBytes { sout in
                IOConnectCallMethod(
                    connection, sel.rawValue,
                    scalarIn.isEmpty ? nil : sin.baseAddress, UInt32(scalarIn.count),
                    nil, 0,
                    nil, &scalarOutCnt,
                    structOutCap == 0 ? nil : sout.baseAddress, &structOutSize)
            }
        }
        let n = min(structOutSize, structOutCap)
        return (kr, structOutSize, n > 0 ? structOut.prefix(n) : Data())
    }

    private func withStructIn<R>(_ data: Data?, _ body: (UnsafeRawPointer?, Int) -> R) -> R {
        guard let data, !data.isEmpty else { return body(nil, 0) }
        return data.withUnsafeBytes { body($0.baseAddress, data.count) }
    }

    // MARK: - IOReturn decoding (common cases)

    static func decode(_ kr: kern_return_t) -> String {
        switch kr {
        case KERN_SUCCESS: return "Success"
        case kIOReturnNoDevice: return "No Device (driver not loaded)"
        case kIOReturnNotPrivileged: return "Not Privileged (app sandbox / access)"
        case kIOReturnBadArgument: return "Bad Argument"
        case kIOReturnUnsupported: return "Unsupported"
        case kIOReturnNotOpen: return "Not Open"
        case kIOReturnBusy: return "Busy"
        case kIOReturnTimeout: return "Timeout"
        case kIOReturnNotFound: return "Not Found (not ready yet)"
        case kIOReturnNoSpace: return "No Space"
        default: return String(format: "0x%08x (%d)", UInt32(bitPattern: kr), kr)
        }
    }
}
