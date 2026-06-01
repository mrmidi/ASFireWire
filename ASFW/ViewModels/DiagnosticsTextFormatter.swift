//
//  DiagnosticsTextFormatter.swift
//  ASFW
//
//  Created by ASFireWire Project on 29.05.2026.
//

import Foundation

/// Renders an `ASFWDiagnosticsSnapshot` into the human-readable text report.
///
/// The snapshot is a multi-KB value type and this runs on a libdispatch
/// `.userInitiated` worker whose stack is only ~512 KB (see
/// DiagnosticsStore.refresh). The work is split into per-section functions so
/// each section's temporaries — notably the `withUnsafeBytes` copies of the
/// ~17 KB `topology.nodes` array — live in a small frame that is reclaimed
/// before the next section runs. The snapshot is passed as a normal parameter:
/// large structs are passed indirectly (`@in_guaranteed`, by address) when the
/// callee does not consume them, so this does not copy the snapshot per call.
///
/// The previous implementation was a single ~1300-line function: every local,
/// every nested closure, and every multi-KB `withUnsafeBytes(of:)` temporary
/// shared one stack frame that the debug compiler does not coalesce. That frame
/// exceeded the worker's stack and faulted in `___chkstk_darwin` (EXC_BAD_ACCESS,
/// code=2) on entry — before any code ran, which is why the backtrace showed the
/// arguments as null/unavailable. Splitting into sections bounds the peak frame.
///
/// Each `append*` section lives in a `DiagnosticsTextFormatter+*.swift` extension.
struct DiagnosticsTextFormatter {

    static func format(snapshot: ASFWDiagnosticsSnapshot,
                       version: DriverVersionInfo? = nil) -> String {
        let r = DiagnosticsReport()

        appendHeader(r, snapshot, version)
        appendBusContract(r, snapshot)
        appendTopology(r, snapshot)
        appendRoleCoordinator(r, snapshot)
        appendBusManagerRuntime(r, snapshot)
        appendBMElection(r, snapshot)
        appendIRMFallback(r, snapshot)
        appendCycleMaster(r, snapshot)
        appendRootSelection(r, snapshot)
        appendGapCount(r, snapshot)
        appendPower(r, snapshot)
        appendCSRCompliance(r, snapshot)
        appendPostResetTiming(r, snapshot)
        appendOHCI(r, snapshot)
        appendPHY(r, snapshot)
        appendInboundCSRStats(r, snapshot)
        appendCSRContract(r, snapshot)
        appendAsyncTrace(r, snapshot)

        r.raw("\n" + String(repeating: "═", count: 60) + "\n")
        r.raw("END OF REPORT\n")

        return r.text
    }
}
