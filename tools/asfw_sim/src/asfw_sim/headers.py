"""Parse ASFW's real C++ geometry constants out of the driver headers.

ANTI-DRIFT RULE.  The simulator must never analyse a geometry the driver does
not have.  Both 2026-07 triage reports reached wrong conclusions from stale
constants (one used a 408-slot ring when the code had long carried
``kTxSharedSlotPackets = 912``; another slipped 4x on a staleness horizon), so
this module reads the numbers from the headers themselves and
``tests/test_constants_match_headers.py`` fails if the Python mirror in
``geometry.py`` disagrees.

Scope: the ``static constexpr uint32_t`` integer constants in

* ``ASFWDriver/Shared/Isoch/AudioTimingGeometry.hpp``
* ``ASFWDriver/Shared/Isoch/AudioHalBufferProfiles.hpp``
* ``ASFWDriver/Audio/Wire/AMDTP/RxSequenceReplay.hpp``

This is deliberately NOT a C++ parser.  It evaluates the restricted expression
grammar those constants actually use -- integer literals, previously-defined
identifiers, ``+ - * / %``, parentheses, and ``profile.field`` member access --
and raises on anything outside it rather than guessing.  A constant this module
cannot evaluate is a loud failure, never a silent default.
"""

from __future__ import annotations

import ast
import operator
import re
from dataclasses import dataclass
from pathlib import Path

__all__ = [
    "HeaderConstants",
    "DriverHeaders",
    "find_driver_root",
    "load_driver_headers",
    "CppEvalError",
]


class CppEvalError(RuntimeError):
    """A constant could not be parsed or evaluated from the header."""


# --- comment / literal normalisation ----------------------------------------

_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT = re.compile(r"//[^\n]*")

# `static constexpr uint32_t NAME = <expr>;`  (expression may span lines)
_UINT_CONST = re.compile(
    r"static\s+constexpr\s+uint32_t\s+(\w+)\s*=\s*([^;]+);",
    re.DOTALL,
)

# `inline constexpr AudioHalBufferProfile NAME{ "text", a, b, c, };`
_PROFILE = re.compile(
    r"inline\s+constexpr\s+AudioHalBufferProfile\s+(\w+)\s*\{(.*?)\}\s*;",
    re.DOTALL,
)

# `inline constexpr uint8_t kAudioHalBufferProfileRaw = 2;` in the #else branch
_RAW_PROFILE_DEFAULT = re.compile(
    r"#else\s*inline\s+constexpr\s+uint8_t\s+kAudioHalBufferProfileRaw\s*=\s*(\d+)\s*;",
    re.DOTALL,
)


def _strip_comments(text: str) -> str:
    return _LINE_COMMENT.sub("", _BLOCK_COMMENT.sub("", text))


_BIN_OPS = {
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.Mult: operator.mul,
    ast.Mod: operator.mod,
}


def _eval_cpp_int(expr: str, names: dict[str, int]) -> int:
    """Evaluate a restricted C++ integer constant expression.

    C++ ``/`` on unsigned operands truncates toward zero.  Every value in this
    geometry is non-negative, so Python floor division ``//`` is exact -- but we
    assert non-negativity rather than assume it, because a negative intermediate
    would silently diverge between the two languages.
    """
    # These constants wrap across lines; Python would end the expression at a
    # newline outside parentheses, so flatten to one line first.
    cleaned = " ".join(expr.split())
    cleaned = cleaned.replace("'", "")  # C++14 digit separators: 24'576'000
    cleaned = re.sub(r"\bstatic_cast<\s*\w+\s*>\s*", "", cleaned)
    cleaned = cleaned.replace(".", "__")  # profile.field -> profile__field
    cleaned = re.sub(r"\b(\d+)[uU][lL]{0,2}\b", r"\1", cleaned)  # 8000u -> 8000

    try:
        tree = ast.parse(cleaned.strip(), mode="eval")
    except SyntaxError as exc:  # pragma: no cover - defensive
        raise CppEvalError(f"cannot parse expression {expr!r}: {exc}") from exc

    def walk(node: ast.AST) -> int:
        if isinstance(node, ast.Expression):
            return walk(node.body)
        if isinstance(node, ast.Constant):
            if isinstance(node.value, int) and not isinstance(node.value, bool):
                return node.value
            raise CppEvalError(f"non-integer literal {node.value!r} in {expr!r}")
        if isinstance(node, ast.Name):
            if node.id not in names:
                raise CppEvalError(f"undefined identifier {node.id!r} in {expr!r}")
            return names[node.id]
        if isinstance(node, ast.BinOp):
            left, right = walk(node.left), walk(node.right)
            if isinstance(node.op, ast.Div):
                if left < 0 or right < 0:
                    raise CppEvalError(
                        f"negative operand in C++ integer division in {expr!r}; "
                        "Python // would diverge from C++ truncation"
                    )
                if right == 0:
                    raise CppEvalError(f"division by zero in {expr!r}")
                return left // right
            fn = _BIN_OPS.get(type(node.op))
            if fn is None:
                raise CppEvalError(f"unsupported operator in {expr!r}")
            return fn(left, right)
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.UAdd):
            return walk(node.operand)
        raise CppEvalError(f"unsupported syntax in {expr!r}: {ast.dump(node)}")

    return walk(tree)


# --- data ---------------------------------------------------------------------


@dataclass(frozen=True)
class HeaderConstants:
    """Constants scraped from one header, in declaration order."""

    path: Path
    values: dict[str, int]

    def __getitem__(self, key: str) -> int:
        try:
            return self.values[key]
        except KeyError as exc:
            raise CppEvalError(
                f"{key!r} not found in {self.path.name}; the header changed shape"
            ) from exc


@dataclass(frozen=True)
class DriverHeaders:
    """The full geometry as the driver actually defines it."""

    timing: HeaderConstants
    replay: HeaderConstants
    profile_name: str

    # --- the constants the simulator actually depends on ---------------------
    @property
    def tx_preparation_lead_packets(self) -> int:
        return self.timing["kTxPreparationLeadPackets"]

    @property
    def tx_coverage_lead_packets(self) -> int:
        return self.timing["kTxCoverageLeadPackets"]

    @property
    def tx_shared_slot_packets(self) -> int:
        return self.timing["kTxSharedSlotPackets"]

    @property
    def tx_hardware_ring_packets(self) -> int:
        return self.timing["kTxHardwareRingPackets"]

    @property
    def tx_data_horizon_packets(self) -> int:
        return self.timing["kTxDataHorizonPackets"]

    @property
    def tx_exposure_lead_frames(self) -> int:
        return self.timing["kTxExposureLeadFrames"]

    @property
    def timeline_slots(self) -> int:
        return self.timing["kTimelineSlots"]

    @property
    def hal_io_period_frames(self) -> int:
        return self.timing["kHalIoPeriodFrames"]

    @property
    def frame_ring_frames(self) -> int:
        return self.timing["kFrameRingFrames"]

    @property
    def tx_packets_per_group(self) -> int:
        return self.timing["kTxPacketsPerGroup"]

    @property
    def frames_per_data_packet(self) -> int:
        return self.timing["kFramesPerDataPacket"]

    @property
    def min_avg_cadence_packets(self) -> int:
        return self.timing["kMinAvgCadencePackets"]

    @property
    def min_avg_cadence_frames(self) -> int:
        return self.timing["kMinAvgCadenceFrames"]

    @property
    def scheduling_jitter_frames(self) -> int:
        return self.timing["kSchedulingJitterFrames"]

    @property
    def hal_zero_timestamp_period_frames(self) -> int:
        return self.timing["kHalZeroTimestampPeriodFrames"]

    @property
    def replay_capacity(self) -> int:
        return self.replay["kCapacity"]

    @property
    def replay_read_delay(self) -> int:
        return self.replay["kReadDelay"]


# --- loading ------------------------------------------------------------------


def find_driver_root(start: Path | None = None) -> Path:
    """Walk up from ``start`` to the repository root containing ``ASFWDriver/``."""
    here = (start or Path(__file__).resolve()).resolve()
    for candidate in [here, *here.parents]:
        if (candidate / "ASFWDriver" / "Shared" / "Isoch").is_dir():
            return candidate
    raise CppEvalError(
        f"could not locate the ASFireWire repository root above {here}; "
        "asfw_sim must run from inside the checkout"
    )


def _parse_active_profile(text: str) -> tuple[str, dict[str, int]]:
    """Return the active HAL buffer profile's name and its fields.

    Mirrors ``SelectAudioHalBufferProfile`` + the ``#else`` default of
    ``kAudioHalBufferProfileRaw``.  If the build ever passes
    ``-DASFW_AUDIO_HAL_BUFFER_PROFILE`` the sim would need that flag too, so we
    read the in-header default and record which profile it selected.
    """
    profiles: list[tuple[str, dict[str, int]]] = []
    for name, body in _PROFILE.findall(text):
        parts = [p.strip() for p in body.split(",") if p.strip()]
        if len(parts) < 4:
            raise CppEvalError(f"profile {name} has {len(parts)} fields, expected 4")
        label = parts[0].strip('"')
        profiles.append(
            (
                label,
                {
                    "frameRingFrames": int(parts[1]),
                    "clientIoBudgetFrames": int(parts[2]),
                    "zeroTimestampPeriodFrames": int(parts[3]),
                },
            )
        )

    if not profiles:
        raise CppEvalError("no AudioHalBufferProfile definitions found")

    match = _RAW_PROFILE_DEFAULT.search(text)
    if match is None:
        raise CppEvalError(
            "could not find the default kAudioHalBufferProfileRaw; "
            "AudioHalBufferProfiles.hpp changed shape"
        )
    index = int(match.group(1))
    if index >= len(profiles):
        raise CppEvalError(f"profile index {index} out of range ({len(profiles)})")
    return profiles[index]


# <cstdint> limit macros the geometry headers reference (e.g. kNoInfo).
_CSTDINT_LIMITS = {
    "UINT8_MAX": 0xFF,
    "UINT16_MAX": 0xFFFF,
    "UINT32_MAX": 0xFFFFFFFF,
}


def _scrape_uint_constants(text: str, seed: dict[str, int]) -> dict[str, int]:
    """Evaluate every ``static constexpr uint32_t`` in declaration order."""
    names = {**_CSTDINT_LIMITS, **seed}
    out: dict[str, int] = {}
    for name, expr in _UINT_CONST.findall(text):
        value = _eval_cpp_int(expr, names)
        names[name] = value
        out[name] = value
    if not out:
        raise CppEvalError("no static constexpr uint32_t constants found")
    return out


def load_driver_headers(root: Path | None = None) -> DriverHeaders:
    """Read the live geometry out of the driver headers."""
    repo = root or find_driver_root()
    shared = repo / "ASFWDriver" / "Shared" / "Isoch"
    timing_path = shared / "AudioTimingGeometry.hpp"
    profiles_path = shared / "AudioHalBufferProfiles.hpp"
    replay_path = (
        repo / "ASFWDriver" / "Audio" / "Wire" / "AMDTP" / "RxSequenceReplay.hpp"
    )

    for path in (timing_path, profiles_path, replay_path):
        if not path.is_file():
            raise CppEvalError(f"expected driver header not found: {path}")

    profile_name, profile_fields = _parse_active_profile(
        _strip_comments(profiles_path.read_text(encoding="utf-8"))
    )
    seed = {
        f"kActiveAudioHalBufferProfile__{field}": value
        for field, value in profile_fields.items()
    }

    timing = _scrape_uint_constants(
        _strip_comments(timing_path.read_text(encoding="utf-8")), seed
    )
    replay = _scrape_uint_constants(
        _strip_comments(replay_path.read_text(encoding="utf-8")), {}
    )

    return DriverHeaders(
        timing=HeaderConstants(timing_path, timing),
        replay=HeaderConstants(replay_path, replay),
        profile_name=profile_name,
    )
