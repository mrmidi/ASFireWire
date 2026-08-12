#!/usr/bin/env python3
"""Fail CI when normalized identity/audio layer boundaries regress."""

from __future__ import annotations

import re
import sys
from pathlib import Path


def source_files(root: Path):
    for suffix in ("*.hpp", "*.cpp", "*.iig"):
        yield from root.rglob(suffix)


def relative(path: Path, root: Path) -> str:
    return str(path.relative_to(root))


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_boundaries.py <repository-root>", file=sys.stderr)
        return 2

    repo = Path(sys.argv[1]).resolve()
    driver = repo / "ASFWDriver"
    violations: list[str] = []

    # Protocols/AVC owns standards-only AV/C. Its local Audio/ subdirectory is
    # an AV/C Audio Subunit implementation; includes that resolve into the
    # top-level ASFWDriver/Audio tree are the forbidden dependency.
    avc = driver / "Protocols" / "AVC"
    include_pattern = re.compile(r"^\s*#\s*include\s*[\"<]([^\">]+)[\">]", re.MULTILINE)
    for path in source_files(avc):
        text = path.read_text(encoding="utf-8")
        for include in include_pattern.findall(text):
            if include.startswith("ASFWDriver/Audio/"):
                violations.append(
                    f"{relative(path, repo)} includes top-level audio: {include}"
                )
                continue
            if include.startswith("../"):
                resolved = (path.parent / include).resolve()
                try:
                    resolved.relative_to(driver / "Audio")
                except ValueError:
                    pass
                else:
                    violations.append(
                        f"{relative(path, repo)} includes top-level audio: {include}"
                    )

    all_sources = list(source_files(driver))
    retired_symbols = (
        "AudioProfileRegistry",
        "DiceProfileRegistry",
        "DeviceProtocolFactory",
        "DeviceStreamModeQuirks",
        "UsesPlug0Inventory",
    )
    for path in all_sources:
        text = path.read_text(encoding="utf-8")
        for symbol in retired_symbols:
            if symbol in text:
                violations.append(
                    f"{relative(path, repo)} references retired global policy {symbol}"
                )

    # GUID lookup is intentionally plural and diagnostic. Routing code must use
    # DeviceInstanceId/DeviceRouteToken, never recreate a single-result lookup.
    singular_guid_lookup = re.compile(
        r"\b(?:Find|Get|Lookup|Resolve)Device(?:Instance)?By(?:Observed)?Guid\b",
        re.IGNORECASE,
    )
    plural_guid_lookup = re.compile(
        r"\b(?:FindDevicesByObservedGuid|FindInstancesByObservedGuid)\s*\("
    )
    diagnostic_owners = {
        "ASFWDriver/Discovery/IDeviceManager.hpp",
        "ASFWDriver/Discovery/DeviceManager.hpp",
        "ASFWDriver/Discovery/DeviceManager.cpp",
        "ASFWDriver/Discovery/DeviceRegistry.hpp",
        "ASFWDriver/Discovery/DeviceRegistry.cpp",
    }
    for path in all_sources:
        text = path.read_text(encoding="utf-8")
        rel = relative(path, repo)
        if singular_guid_lookup.search(text):
            violations.append(f"{rel} declares or uses a singular GUID lookup")
        if plural_guid_lookup.search(text) and rel not in diagnostic_owners:
            violations.append(f"{rel} uses diagnostic GUID lookup in production routing")

    # High-level graph/composition code may copy identity evidence for display,
    # but it may not branch on vendor/model/GUID. Those decisions belong to the
    # declarative catalog or a family-local resolver.
    decision_roots = (
        driver / "Controller",
        driver / "Audio" / "Core",
        driver / "Audio" / "DriverKit",
        driver / "UserClient",
    )
    identity_term = r"(?:vendor(?:Id|ID)|model(?:Id|ID)|observedGuid|GetObservedGuid)\b"
    comparison = r"(?:==|!=|<=|>=|(?<!-)>|<(?!<))"
    identity_branch = re.compile(
        r"(?:\bswitch\s*\([^)]*" + identity_term + r"|"
        r"\bif\s*\([^)]*(?:" + identity_term +
        r"[^)]*" + comparison + r"|" + comparison + r"[^)]*" +
        identity_term + r"))",
        re.IGNORECASE | re.DOTALL,
    )
    for root in decision_roots:
        for path in source_files(root):
            text = path.read_text(encoding="utf-8")
            if identity_branch.search(text):
                violations.append(
                    f"{relative(path, repo)} branches on vendor/model/GUID outside catalog/family"
                )

    # The audio DriverKit service receives a copied, validated property blob.
    # A C++ profile type or pointer in an IIG contract would recreate the
    # cross-service lifetime hazard this architecture is designed to remove.
    for path in source_files(driver / "Audio" / "DriverKit"):
        if path.suffix != ".iig":
            continue
        text = path.read_text(encoding="utf-8")
        if "ResolvedAudioEndpointProfile" in text:
            violations.append(
                f"{relative(path, repo)} exposes resolved profile state across IIG"
            )

    if violations:
        print("Architecture boundary violations:", file=sys.stderr)
        for violation in violations:
            print(f"  - {violation}", file=sys.stderr)
        return 1

    print("Architecture boundaries verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
