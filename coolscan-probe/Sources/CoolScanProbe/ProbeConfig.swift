import Foundation

/// Run-time knobs for A/B-testing the SBP-2 transport-wedge workarounds against
/// main's session-scheduler deadlock fix (PR #33, "arm timer outside lock").
///
/// Hypothesis: the deadlock was the root cause of ORBs wedging (a stuck ORB held
/// its slot for tens of seconds), which forced two workarounds:
///   - long per-command ORB timeouts (`SCSI.kSlowCmdTimeoutMs` / `kPollCmdTimeoutMs`)
///     so a short timeout wouldn't fire AGENT_RESET against a settling scanner, and
///   - a LOGICAL UNIT RESET on the wedge-recovery path.
/// If the fix removed the wedge, both should become unnecessary. These flags let
/// one installed dext be exercised across the A/B matrix without a probe rebuild.
///
///   --orb-timeout-ms N   Cap every command's ORB timeout at N ms (A-test: re-introduce
///                        the aggressive short-timeout regime the workarounds replaced).
///                        Omitted ⇒ each command keeps its tuned per-command timeout.
///   --no-lur             Skip the best-effort LOGICAL UNIT RESET on transport-wedge
///                        recovery (B-test: does un-wedging still need it?).
enum ProbeConfig {
    static let orbTimeoutCapMs: UInt32? = parseUIntFlag("--orb-timeout-ms")
    static let disableLUR: Bool = CommandLine.arguments.contains("--no-lur")

    /// Apply the `--orb-timeout-ms` ceiling. A cap (min), not a replacement, so
    /// per-command semantics survive: a command tuned to 30 s and one tuned to
    /// 3 s both drop to the same aggressive ceiling under test.
    static func cap(_ ms: UInt32) -> UInt32 {
        guard let c = orbTimeoutCapMs else { return ms }
        return min(ms, c)
    }

    /// One-line banner so a run's log makes the active regime unambiguous.
    static func describe() -> String {
        let t = orbTimeoutCapMs.map { "\($0) ms cap" } ?? "per-command"
        return "ORB-timeout=\(t), LUR=\(disableLUR ? "off" : "on")"
    }

    private static func parseUIntFlag(_ name: String) -> UInt32? {
        let a = CommandLine.arguments
        guard let i = a.firstIndex(of: name), i + 1 < a.count else { return nil }
        return UInt32(a[i + 1])
    }
}
