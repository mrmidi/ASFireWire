#!/usr/bin/env python3
"""FW-184 tree guard: retired timing/HAL geometry must not come back.

Epic FW-177 replaced several parallel derivations of the timing and HAL
buffer geometry with one resolver (documentation/TIMING_GEOMETRY_OWNERSHIP.md).
This scan fails when a retired name, a second profile lookup or a hand-written
geometry literal reappears in driver code. Comments are stripped first, so
history may still be discussed in prose.

Run from ctest (tests/tools/CMakeLists.txt) or by hand:

    python3 tools/timing_geometry_guard.py [--root REPO]
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".iig"}

# Identifiers deleted by FW-181/182/183 and the V3 change (FW-183c). Each maps
# to what replaced it, so a failure says where to go instead.
RETIRED_IDENTIFIERS = {
    "TimingCursorPolicy": "ivars.device.timing (ResolvedTimingGeometry)",
    "AudioGeometryPolicy": "ResolveTimingGeometry / TimingLadder",
    "RequiredInputSafetyFrames": "ResolveInputSafetyFrames",
    "InputSafetyPolicy": "ResolveInputSafetyFrames",
    "TxBufferProfile": "IAudioDeviceProfile declarations via the resolver",
    "RxBufferProfile": "IAudioDeviceProfile declarations via the resolver",
    "kReportedDeviceLatencyFrames": "profile latency via the resolver",
    "kReportedSafetyOffsetFrames": "profile safety via the resolver",
    "kTransferDelayTicks": "Encoding::AmdtpTransferDelayTicks",
    "kTransferDelayNanos": "Encoding::AmdtpTransferDelayTicks",
    "kShippedTransferDelayTicks": "AppliedTransferDelayTicks (D1)",
    "kHalZeroTimestampPeriodFrames": "HalBufferProfileForRate(rate) / ivars.device.timing",
    "kFrameRingFrames": "HalBufferProfileForRate(rate) (active) / kAllocatedFrameRingFrames",
    "kActiveAudioHalBufferProfile": "HalBufferProfileForRate(rate)",
    "SelectAudioHalBufferProfile": "HalBufferProfileForRate(rate)",
    "AudioHalBufferProfileId": "HalBufferProfileForRate(rate)",
    "ASFW_AUDIO_HAL_BUFFER_PROFILE": "HalBufferProfileForRate(rate)",
    "IsLiveCompatible": "ProfileFitsAllocation + the configuration-change window",
    "pendingExternalRateHz": "pendingSampleRateHz",
    "kMinNominalFramesPerInterrupt": "CompletionBatchFrames(rate)",
    "kMaxNominalFramesPerInterrupt": "CompletionBatchFrames(rate)",
    "kInputSafetyFloorFrames": "CompletionBatchFrames(rate)",
    "kOutputConsumerLeadFrames": "AudioTimingGeometry TX budgets",
    "kOutputCursorResyncDeadbandFrames": "AudioTimingGeometry TX budgets",
}

# Transfer delay is derived from the wire geometry, never passed as a double.
RETIRED_PATTERNS = {
    r"\bTransferDelayTicks\s*\(\s*double\b": "AmdtpTransferDelayTicks(geometry, mode)",
}

# The audio side resolves the device profile once, at graph construction
# (ownership rule 2, G-19). Everything else reads ivars.device.profile.
FIND_PROFILE_ALLOWED = {
    "ASFWDriver/Audio/DriverKit/Config/AudioProfileRegistry.hpp",
    "ASFWDriver/Audio/DriverKit/Config/AudioProfileRegistry.cpp",
    "ASFWDriver/Audio/DriverKit/ASFWAudioDriverGraph.cpp",
}

# Geometry numbers that must come from their owner, not be hand-written:
# 12800 (transfer delay at 48/96/192 kHz), 1536 (the retired ring/ZTS),
# 12288 / 24576 (the V3 period and allocation).
GEOMETRY_LITERALS = ("12800", "1536", "12288", "24576")
LITERAL_SCOPES = ("ASFWDriver/Audio/", "ASFWDriver/Shared/Isoch/", "ASFWDriver/Isoch/")
LITERAL_OWNERS = {
    "ASFWDriver/Shared/Isoch/AudioHalBufferProfiles.hpp",
    "ASFWDriver/Shared/Isoch/AudioTimingGeometry.hpp",
    "ASFWDriver/Audio/Wire/AMDTP/AmdtpTransferDelay.hpp",
    # static_assert pinning the M-Audio constant to the formula.
    "ASFWDriver/Audio/Protocols/BeBoB/MAudioInternalTxTiming.cpp",
}

_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT = re.compile(r"//[^\n]*")
_STRING = re.compile(r'"(?:\\.|[^"\\\n])*"')


@dataclass(frozen=True)
class Violation:
    path: str
    line: int
    rule: str
    detail: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: [{self.rule}] {self.detail}"


def strip_comments(text: str) -> str:
    """Remove comments and string literals, keeping line numbers stable."""
    text = _BLOCK_COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    text = _LINE_COMMENT.sub("", text)
    return _STRING.sub('""', text)


def _literal_regex(digits: str) -> re.Pattern[str]:
    # Accept C++14 digit separators anywhere (12'800, 24'576).
    body = "'?".join(digits)
    return re.compile(rf"(?<![\w'.]){body}(?![\w'])")


_LITERALS = {lit: _literal_regex(lit) for lit in GEOMETRY_LITERALS}
_IDENTIFIERS = {name: re.compile(rf"\b{re.escape(name)}\b") for name in RETIRED_IDENTIFIERS}
_PATTERNS = {re.compile(p): why for p, why in RETIRED_PATTERNS.items()}
_FIND_PROFILE = re.compile(r"\bFindProfile\s*\(")


def scan_text(rel: str, text: str) -> list[Violation]:
    violations: list[Violation] = []
    check_literals = rel.startswith(LITERAL_SCOPES) and rel not in LITERAL_OWNERS
    for number, line in enumerate(strip_comments(text).split("\n"), start=1):
        for name, pattern in _IDENTIFIERS.items():
            if pattern.search(line):
                violations.append(Violation(
                    rel, number, "retired-name",
                    f"{name} was retired; use {RETIRED_IDENTIFIERS[name]}"))
        for pattern, why in _PATTERNS.items():
            if pattern.search(line):
                violations.append(Violation(rel, number, "retired-pattern", f"use {why}"))
        if rel not in FIND_PROFILE_ALLOWED and _FIND_PROFILE.search(line):
            violations.append(Violation(
                rel, number, "second-profile-lookup",
                "resolve the profile once in the graph; read ivars.device.profile"))
        if check_literals:
            for lit, pattern in _LITERALS.items():
                if pattern.search(line):
                    violations.append(Violation(
                        rel, number, "geometry-literal",
                        f"{lit} is owned by the geometry/wire headers; use the named value"))
    return violations


def scan_tree(root: Path) -> list[Violation]:
    violations: list[Violation] = []
    driver = root / "ASFWDriver"
    for path in sorted(driver.rglob("*")):
        if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        violations.extend(scan_text(rel, path.read_text(encoding="utf-8", errors="replace")))
    return violations


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args(argv)
    if not (args.root / "ASFWDriver").is_dir():
        print(f"no ASFWDriver/ under {args.root}", file=sys.stderr)
        return 2
    violations = scan_tree(args.root)
    for violation in violations:
        print(violation)
    if violations:
        print(f"\n{len(violations)} timing-geometry guard violation(s). "
              "See documentation/TIMING_GEOMETRY_OWNERSHIP.md.", file=sys.stderr)
        return 1
    print("timing-geometry guard: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
