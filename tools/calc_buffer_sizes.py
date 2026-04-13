#!/usr/bin/env python3
"""
TX Buffer / Latency calculator for ASFW (48k-centric).

Features:
- Parse compile-time TX profiles (A/B/C) from AudioTxProfiles.hpp
- Deterministic timing/depth calculations
- Monte Carlo simulation for underrun/overrun risk
- Optional target sweep to find minimum safe ring target
- Console + JSON + CSV outputs
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import random
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple


# -----------------------------------------------------------------------------
# Driver-aligned constants (48k path)
# -----------------------------------------------------------------------------
K_CYCLES_PER_SECOND = 8000
K_NANOS_PER_CYCLE = 125_000
K_CIP_HEADER_BYTES = 8
K_BYTES_PER_SAMPLE = 4
K_TRANSFER_CHUNK_FRAMES = 256  # fixed by design
K_TX_QUEUE_CAPACITY_DEFAULT = 4096
K_RING_FRAMECOUNT_DEFAULT = 4096  # AudioRingBuffer<4096>
K_RING_CAPACITY_DEFAULT = K_RING_FRAMECOUNT_DEFAULT - 1  # one slot reserved
K_MAX_PACKET_PAYLOAD_BYTES = 4096

# ADK currently configured in ASFWAudioDriver.cpp
K_ADK_OUTPUT_LATENCY_FRAMES_DEFAULT = 24
K_ADK_OUTPUT_SAFETY_FRAMES_DEFAULT = 32

# Trial safety cap
K_MAX_TRIALS_DEFAULT = 5000

# Known-safe fallback profile values (keep in sync with AudioTxProfiles.hpp)
FALLBACK_PROFILES: Dict[str, "TxBufferProfile"] = {
    "A": None,  # filled below to avoid forward-reference in dataclass literal
    "B": None,
    "C": None,
}


@dataclass(frozen=True)
class TxBufferProfile:
    name: str
    start_wait_frames: int
    startup_prime_limit_frames: int
    rb_target_frames: int
    rb_max_frames: int
    max_chunks_per_refill: int


FALLBACK_PROFILES["A"] = TxBufferProfile("A", 256, 512, 512, 768, 6)
FALLBACK_PROFILES["B"] = TxBufferProfile("B", 512, 0, 1024, 1536, 8)
FALLBACK_PROFILES["C"] = TxBufferProfile("C", 128, 256, 256, 384, 4)


@dataclass
class ParsedProfiles:
    profiles: Dict[str, TxBufferProfile]
    selected_profile: str
    source_path: str
    used_fallback: bool
    warnings: List[str]


@dataclass
class EffectiveConfig:
    profile: str
    sample_rate: int
    channels: int
    stream_mode: str
    core_buffer_frames: int
    tx_queue_capacity: int
    ring_capacity: int
    start_wait: int
    prime_limit: int
    rb_target: int
    rb_max: int
    max_chunks: int
    duration_sec: float
    trials: int
    jitter_model: str
    jitter_std_us: float
    refill_hz: float
    refill_jitter_std_us: float
    seed: Optional[int]
    jitter_samples_ms: Optional[List[float]]
    include_adk_latency: bool
    adk_output_latency_frames: int
    adk_output_safety_frames: int


@dataclass
class DeterministicResult:
    sample_rate: int
    channels: int
    stream_mode: str
    cycles_per_second: int
    nanos_per_cycle: int
    packetization: Dict[str, float]
    depth_conversions: Dict[str, Dict[str, float]]
    startup: Dict[str, object]
    latency_estimates: Dict[str, Dict[str, float]]
    notes: List[str]


@dataclass
class TrialResult:
    trial_index: int
    underrun_events: int
    underrun_frames: int
    underrun_windows: int
    overrun_events: int
    overrun_frames: int
    min_ring_fill: int
    max_ring_fill: int
    mean_ring_fill: float
    min_queue_fill: int
    max_queue_fill: int
    mean_queue_fill: float
    time_under_target_ms: int
    glitch: bool


@dataclass
class MonteCarloSummary:
    enabled: bool
    trials: int
    duration_sec: float
    jitter_model: str
    jitter_std_us: float
    seed: Optional[int]
    glitch_trials: int
    glitch_probability: float
    glitch_probability_ci_low: float
    glitch_probability_ci_high: float
    underrun_trials: int
    overrun_trials: int
    avg_underrun_events: float
    avg_underrun_frames: float
    avg_underrun_windows: float
    avg_overrun_events: float
    avg_time_under_target_ms: float
    p95_time_under_target_ms: float
    avg_mean_ring_fill: float
    avg_mean_queue_fill: float


@dataclass
class SweepRow:
    rb_target: int
    glitch_probability: float
    glitch_probability_ci_high: float
    avg_underrun_events: float
    avg_overrun_events: float
    meets_threshold: bool


def fail(message: str) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(2)


def frames_to_ms(frames: float, sample_rate: int) -> float:
    return (frames * 1000.0) / float(sample_rate)


def frames_to_cycles(frames: float, sample_rate: int) -> float:
    return frames * (float(K_CYCLES_PER_SECOND) / float(sample_rate))


def load_jitter_samples_ms(path: Path) -> List[float]:
    if not path.exists():
        fail(f"jitter samples file not found: {path}")
    samples_ms: List[float] = []
    try:
        with path.open("r", encoding="utf-8") as f:
            reader = csv.reader(f)
            for row in reader:
                if not row:
                    continue
                token = row[0].strip()
                if not token:
                    continue
                try:
                    # File values are interpreted as jitter in microseconds.
                    samples_ms.append(float(token) / 1000.0)
                except ValueError:
                    # Ignore header or malformed rows.
                    continue
    except OSError as exc:
        fail(f"failed to read jitter samples: {exc}")
    if not samples_ms:
        fail("jitter samples file has no numeric values")
    return samples_ms


def wilson_interval(successes: int, total: int, z: float = 1.96) -> Tuple[float, float]:
    if total <= 0:
        return (0.0, 0.0)
    phat = successes / float(total)
    denom = 1.0 + (z * z) / float(total)
    center = (phat + (z * z) / (2.0 * total)) / denom
    margin = (z * math.sqrt((phat * (1.0 - phat) / total) + (z * z) / (4.0 * total * total))) / denom
    lo = max(0.0, center - margin)
    hi = min(1.0, center + margin)
    return (lo, hi)


def parse_tx_profiles(header_path: Path) -> ParsedProfiles:
    warnings: List[str] = []
    if not header_path.exists():
        warnings.append(f"profile header missing: {header_path}")
        return ParsedProfiles(
            profiles=dict(FALLBACK_PROFILES),
            selected_profile="B",
            source_path=str(header_path),
            used_fallback=True,
            warnings=warnings,
        )

    try:
        text = header_path.read_text(encoding="utf-8")
    except OSError as exc:
        warnings.append(f"failed to read profile header: {exc}")
        return ParsedProfiles(
            profiles=dict(FALLBACK_PROFILES),
            selected_profile="B",
            source_path=str(header_path),
            used_fallback=True,
            warnings=warnings,
        )

    # Strip comments for easier parsing.
    no_comments = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    no_comments = re.sub(r"//.*", "", no_comments)

    macro_matches = re.findall(
        r"^\s*#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+([^\s]+)",
        no_comments,
        flags=re.MULTILINE,
    )
    macros: Dict[str, str] = {k: v for (k, v) in macro_matches}

    profile_matches = re.findall(
        r"inline\s+constexpr\s+TxBufferProfile\s+kTxProfile([ABC])\s*\{\s*"
        r"\"([ABC])\"\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\};",
        no_comments,
        flags=re.DOTALL,
    )

    parsed_profiles: Dict[str, TxBufferProfile] = {}
    for suffix, name, start_wait, prime_limit, rb_target, rb_max, max_chunks in profile_matches:
        if suffix != name:
            warnings.append(f"profile suffix/name mismatch: kTxProfile{suffix} uses name={name}")
        parsed_profiles[name] = TxBufferProfile(
            name=name,
            start_wait_frames=int(start_wait),
            startup_prime_limit_frames=int(prime_limit),
            rb_target_frames=int(rb_target),
            rb_max_frames=int(rb_max),
            max_chunks_per_refill=int(max_chunks),
        )

    # Determine default selected profile.
    # ASFW_TX_TUNING_PROFILE is now a plain integer: 0=A, 1=B, 2=C
    selected_profile = "B"
    tuning_expr = macros.get("ASFW_TX_TUNING_PROFILE", "0").strip()
    profile_map = {"0": "A", "1": "B", "2": "C"}
    selected_profile = profile_map.get(tuning_expr, selected_profile)

    used_fallback = False
    if not parsed_profiles:
        warnings.append("no profiles parsed from header; using fallback profile set")
        used_fallback = True
    for name, fallback in FALLBACK_PROFILES.items():
        if name not in parsed_profiles:
            warnings.append(f"missing {name} in parsed profiles; using fallback")
            parsed_profiles[name] = fallback
            used_fallback = True

    if selected_profile not in parsed_profiles:
        warnings.append(f"selected profile {selected_profile} missing; falling back to B")
        selected_profile = "B"
        used_fallback = True

    return ParsedProfiles(
        profiles=parsed_profiles,
        selected_profile=selected_profile,
        source_path=str(header_path),
        used_fallback=used_fallback,
        warnings=warnings,
    )


def stream_mode_packet_model(sample_rate: int, stream_mode: str) -> Dict[str, float]:
    if sample_rate == 48_000 and stream_mode == "blocking":
        return {
            "data_packets_per_8_cycles": 6.0,
            "no_data_packets_per_8_cycles": 2.0,
            "samples_per_data_packet": 8.0,
            "avg_frames_per_cycle": 6.0,
            "avg_frames_per_ms": 48.0,
        }
    if sample_rate == 48_000 and stream_mode == "non-blocking":
        return {
            "data_packets_per_8_cycles": 8.0,
            "no_data_packets_per_8_cycles": 0.0,
            "samples_per_data_packet": 6.0,
            "avg_frames_per_cycle": 6.0,
            "avg_frames_per_ms": 48.0,
        }

    # Deterministic fallback model for non-48k runs (simulation remains 48k-only).
    avg_frames_per_cycle = float(sample_rate) / float(K_CYCLES_PER_SECOND)
    samples_per_packet = max(1.0, round(avg_frames_per_cycle))
    return {
        "data_packets_per_8_cycles": 8.0,
        "no_data_packets_per_8_cycles": 0.0,
        "samples_per_data_packet": float(samples_per_packet),
        "avg_frames_per_cycle": avg_frames_per_cycle,
        "avg_frames_per_ms": float(sample_rate) / 1000.0,
    }


def validate_profile(profile: TxBufferProfile, tx_queue_capacity: int, ring_capacity: int) -> None:
    if profile.start_wait_frames <= 0:
        fail("startWait must be > 0")
    if profile.rb_target_frames <= 0:
        fail("rbTarget must be > 0")
    if profile.rb_target_frames > profile.rb_max_frames:
        fail("rbTarget must be <= rbMax")
    if profile.max_chunks_per_refill <= 0:
        fail("maxChunks must be > 0")
    if profile.start_wait_frames > tx_queue_capacity:
        fail(f"startWait ({profile.start_wait_frames}) exceeds txQueueCapacity ({tx_queue_capacity})")
    if profile.rb_max_frames > ring_capacity:
        fail(f"rbMax ({profile.rb_max_frames}) exceeds ringCapacity ({ring_capacity})")


def validate_config(cfg: EffectiveConfig) -> None:
    if cfg.sample_rate <= 0:
        fail("sample-rate must be > 0")
    if cfg.channels <= 0:
        fail("channels must be > 0")
    if cfg.core_buffer_frames <= 0:
        fail("core-buffer-frames must be > 0")
    if cfg.tx_queue_capacity <= 0:
        fail("tx-queue-capacity must be > 0")
    if cfg.ring_capacity <= 0:
        fail("ring-capacity must be > 0")
    if cfg.start_wait <= 0:
        fail("start-wait must be > 0")
    if cfg.rb_target <= 0:
        fail("rb-target must be > 0")
    if cfg.rb_target > cfg.rb_max:
        fail("rb-target must be <= rb-max")
    if cfg.max_chunks <= 0:
        fail("max-chunks must be > 0")
    if cfg.start_wait > cfg.tx_queue_capacity:
        fail("start-wait exceeds tx-queue-capacity")
    if cfg.rb_max > cfg.ring_capacity:
        fail("rb-max exceeds ring-capacity")
    if cfg.duration_sec <= 0:
        fail("duration-sec must be > 0")
    if cfg.trials < 0:
        fail("trials must be >= 0")
    if cfg.jitter_std_us < 0:
        fail("jitter-std-us must be >= 0")
    if cfg.refill_hz <= 0:
        fail("refill-hz must be > 0")
    if cfg.refill_hz > K_CYCLES_PER_SECOND:
        fail(f"refill-hz must be <= {K_CYCLES_PER_SECOND}")
    if cfg.refill_jitter_std_us < 0:
        fail("refill-jitter-std-us must be >= 0")
    if cfg.jitter_model == "empirical" and (cfg.jitter_samples_ms is None or len(cfg.jitter_samples_ms) == 0):
        fail("jitter-model empirical requires --jitter-samples-csv")


def deterministic_calc(cfg: EffectiveConfig) -> DeterministicResult:
    packet = stream_mode_packet_model(cfg.sample_rate, cfg.stream_mode)
    samples_per_data_packet = int(packet["samples_per_data_packet"])

    payload_bytes = K_CIP_HEADER_BYTES + (samples_per_data_packet * cfg.channels * K_BYTES_PER_SAMPLE)
    payload_ok = payload_bytes <= K_MAX_PACKET_PAYLOAD_BYTES

    startup_prefill_frames = min(cfg.start_wait, cfg.tx_queue_capacity)
    if cfg.prime_limit == 0:
        startup_prime_budget_frames = startup_prefill_frames
        prime_limit_desc = "unbounded (limited by available queue fill)"
        prime_limit_hit = False
    else:
        startup_prime_budget_frames = min(cfg.prime_limit, startup_prefill_frames)
        prime_limit_desc = f"bounded to {cfg.prime_limit} frames"
        prime_limit_hit = cfg.prime_limit <= startup_prefill_frames

    startup_meets_wait = (cfg.prime_limit == 0) or (cfg.prime_limit >= cfg.start_wait)

    depth_conversions = {
        "start_wait": {
            "frames": float(cfg.start_wait),
            "ms": frames_to_ms(cfg.start_wait, cfg.sample_rate),
            "cycles": frames_to_cycles(cfg.start_wait, cfg.sample_rate),
        },
        "rb_target": {
            "frames": float(cfg.rb_target),
            "ms": frames_to_ms(cfg.rb_target, cfg.sample_rate),
            "cycles": frames_to_cycles(cfg.rb_target, cfg.sample_rate),
        },
        "rb_max": {
            "frames": float(cfg.rb_max),
            "ms": frames_to_ms(cfg.rb_max, cfg.sample_rate),
            "cycles": frames_to_cycles(cfg.rb_max, cfg.sample_rate),
        },
        "queue_capacity": {
            "frames": float(cfg.tx_queue_capacity),
            "ms": frames_to_ms(cfg.tx_queue_capacity, cfg.sample_rate),
            "cycles": frames_to_cycles(cfg.tx_queue_capacity, cfg.sample_rate),
        },
    }

    # Latency views:
    # - Transport estimate: CoreAudio callback period + IT ring target.
    # - ADK-inclusive estimate: transport + output latency + safety offset.
    transport_frames = cfg.core_buffer_frames + cfg.rb_target
    transport_max_frames = cfg.core_buffer_frames + cfg.rb_max
    adk_added_frames = cfg.adk_output_latency_frames + cfg.adk_output_safety_frames

    latency_estimates = {
        "transport_only": {
            "frames_est": float(transport_frames),
            "ms_est": frames_to_ms(transport_frames, cfg.sample_rate),
            "frames_worst_est": float(transport_max_frames),
            "ms_worst_est": frames_to_ms(transport_max_frames, cfg.sample_rate),
        },
        "adk_inclusive": {
            "enabled": 1.0 if cfg.include_adk_latency else 0.0,
            "added_frames": float(adk_added_frames),
            "added_ms": frames_to_ms(adk_added_frames, cfg.sample_rate),
            "frames_est": float(transport_frames + adk_added_frames),
            "ms_est": frames_to_ms(transport_frames + adk_added_frames, cfg.sample_rate),
            "frames_worst_est": float(transport_max_frames + adk_added_frames),
            "ms_worst_est": frames_to_ms(transport_max_frames + adk_added_frames, cfg.sample_rate),
        },
    }

    notes: List[str] = []
    if cfg.sample_rate != 48_000:
        notes.append("non-48k mode uses generic deterministic packet model; full cadence simulation is disabled")
    if not payload_ok:
        notes.append(f"payload {payload_bytes}B exceeds OHCI payload cap {K_MAX_PACKET_PAYLOAD_BYTES}B")
    if cfg.prime_limit == 0:
        notes.append("startupPrimeLimit=0 modeled as unbounded (bounded by current queue fill)")
    if not startup_meets_wait:
        notes.append("startup prime limit is below start-wait target; start can still proceed after wait loop")
    if cfg.start_wait < cfg.core_buffer_frames:
        notes.append("start_wait < core_buffer_frames: low startup headroom before first callback")
    drain_per_refill = float(cfg.sample_rate) / float(cfg.refill_hz)
    if cfg.rb_max < (cfg.rb_target + drain_per_refill):
        notes.append("rb_max is close to rb_target relative to refill interval; target clipping risk is elevated")

    return DeterministicResult(
        sample_rate=cfg.sample_rate,
        channels=cfg.channels,
        stream_mode=cfg.stream_mode,
        cycles_per_second=K_CYCLES_PER_SECOND,
        nanos_per_cycle=K_NANOS_PER_CYCLE,
        packetization={
            "samples_per_data_packet": float(samples_per_data_packet),
            "data_packets_per_8_cycles": packet["data_packets_per_8_cycles"],
            "no_data_packets_per_8_cycles": packet["no_data_packets_per_8_cycles"],
            "avg_frames_per_cycle": packet["avg_frames_per_cycle"],
            "avg_frames_per_ms": packet["avg_frames_per_ms"],
            "payload_bytes": float(payload_bytes),
            "payload_ok": 1.0 if payload_ok else 0.0,
        },
        depth_conversions=depth_conversions,
        startup={
            "start_prefill_frames_assumed": startup_prefill_frames,
            "prime_budget_frames": startup_prime_budget_frames,
            "prime_limit_desc": prime_limit_desc,
            "prime_limit_hit_possible": prime_limit_hit,
            "prime_budget_meets_start_wait": startup_meets_wait,
        },
        latency_estimates=latency_estimates,
        notes=notes,
    )


def blocking_cycle_is_data(cycle_index: int) -> bool:
    # Matches BlockingCadence48k: NO-DATA when cycle % 4 == 0
    return (cycle_index % 4) != 0


def jitter_draw_ms(
    rng: random.Random,
    model: str,
    std_us: float,
    empirical_samples_ms: Optional[List[float]] = None,
) -> float:
    if model == "empirical":
        if not empirical_samples_ms:
            return 0.0
        return rng.choice(empirical_samples_ms)

    if std_us <= 0 or model == "none":
        return 0.0

    std_ms = std_us / 1000.0
    if model == "gaussian":
        return rng.gauss(0.0, std_ms)
    if model == "uniform":
        # Match std dev approximately: a = std * sqrt(3)
        a = std_ms * math.sqrt(3.0)
        return rng.uniform(-a, a)
    return 0.0


def apply_legacy_refill(
    queue_fill: int,
    ring_fill: int,
    cfg: EffectiveConfig,
) -> Tuple[int, int, int]:
    pumped = 0
    if ring_fill < cfg.rb_target:
        want = cfg.rb_target - ring_fill
        chunks = 0
        while want > 0 and chunks < cfg.max_chunks:
            if queue_fill <= 0:
                break
            rb_space = cfg.ring_capacity - ring_fill
            if rb_space <= 0:
                break

            to_read = min(want, queue_fill, rb_space, K_TRANSFER_CHUNK_FRAMES)
            if to_read <= 0:
                break

            queue_fill -= to_read
            ring_fill += to_read
            pumped += to_read
            want -= to_read
            chunks += 1

            if ring_fill >= cfg.rb_max:
                break
    return queue_fill, ring_fill, pumped


def run_single_trial(cfg: EffectiveConfig, trial_index: int, seed: int) -> TrialResult:
    rng = random.Random(seed)

    cycles_total = int(math.ceil(cfg.duration_sec * float(K_CYCLES_PER_SECOND)))
    callback_interval_ms = (cfg.core_buffer_frames * 1000.0) / float(cfg.sample_rate)
    refill_interval_ms = 1000.0 / float(cfg.refill_hz)

    # Start condition: IT waits for start_wait fill then pre-primes from queue->ring.
    queue_fill = min(cfg.start_wait, cfg.tx_queue_capacity)
    ring_fill = 0
    prime_budget = queue_fill if cfg.prime_limit == 0 else min(cfg.prime_limit, queue_fill)
    prime_budget = min(prime_budget, cfg.ring_capacity - ring_fill)
    queue_fill -= prime_budget
    ring_fill += prime_budget

    # Producer scheduling.
    next_callback_time_ms = callback_interval_ms + jitter_draw_ms(
        rng, cfg.jitter_model, cfg.jitter_std_us, cfg.jitter_samples_ms
    )
    if next_callback_time_ms < 0.0:
        next_callback_time_ms = 0.0

    # Refill scheduling (independent from callback jitter).
    next_refill_time_ms = refill_interval_ms + jitter_draw_ms(
        rng, cfg.jitter_model, cfg.refill_jitter_std_us, cfg.jitter_samples_ms
    )
    if next_refill_time_ms < 0.0:
        next_refill_time_ms = 0.0

    cycle_index = 0

    underrun_events = 0
    underrun_frames = 0
    underrun_windows = 0
    in_underrun_window = False
    overrun_events = 0
    overrun_frames = 0

    min_ring_fill = ring_fill
    max_ring_fill = ring_fill
    min_queue_fill = queue_fill
    max_queue_fill = queue_fill

    ring_fill_sum = 0.0
    queue_fill_sum = 0.0
    under_target_cycles = 0

    for cycle in range(cycles_total):
        now_ms = (float(cycle) * 1000.0) / float(K_CYCLES_PER_SECOND)

        while next_callback_time_ms <= now_ms:
            write_frames = cfg.core_buffer_frames
            free = cfg.tx_queue_capacity - queue_fill
            written = min(write_frames, free)
            queue_fill += written
            if written < write_frames:
                overrun_events += 1
                overrun_frames += (write_frames - written)

            jitter = jitter_draw_ms(rng, cfg.jitter_model, cfg.jitter_std_us, cfg.jitter_samples_ms)
            interval = callback_interval_ms + jitter
            # Avoid pathological or negative intervals.
            if interval < 0.1:
                interval = 0.1
            next_callback_time_ms += interval

        while next_refill_time_ms <= now_ms:
            queue_fill, ring_fill, _ = apply_legacy_refill(queue_fill, ring_fill, cfg)
            refill_jitter = jitter_draw_ms(
                rng,
                cfg.jitter_model,
                cfg.refill_jitter_std_us,
                cfg.jitter_samples_ms,
            )
            refill_step = refill_interval_ms + refill_jitter
            if refill_step < 0.05:
                refill_step = 0.05
            next_refill_time_ms += refill_step

        if cfg.stream_mode == "blocking":
            is_data = blocking_cycle_is_data(cycle_index)
            needed = 8 if is_data else 0
        else:
            needed = 6

        if needed > 0:
            if ring_fill < needed:
                underrun_events += 1
                underrun_frames += (needed - ring_fill)
                if not in_underrun_window:
                    underrun_windows += 1
                    in_underrun_window = True
                ring_fill = 0
            else:
                ring_fill -= needed
                in_underrun_window = False
        cycle_index += 1

        if ring_fill < cfg.rb_target:
            under_target_cycles += 1

        min_ring_fill = min(min_ring_fill, ring_fill)
        max_ring_fill = max(max_ring_fill, ring_fill)
        min_queue_fill = min(min_queue_fill, queue_fill)
        max_queue_fill = max(max_queue_fill, queue_fill)
        ring_fill_sum += ring_fill
        queue_fill_sum += queue_fill

    mean_ring_fill = ring_fill_sum / float(cycles_total) if cycles_total > 0 else 0.0
    mean_queue_fill = queue_fill_sum / float(cycles_total) if cycles_total > 0 else 0.0
    time_under_target_ms = int(round((under_target_cycles * 1000.0) / float(K_CYCLES_PER_SECOND)))
    glitch = (underrun_events > 0) or (overrun_events > 0)

    return TrialResult(
        trial_index=trial_index,
        underrun_events=underrun_events,
        underrun_frames=underrun_frames,
        underrun_windows=underrun_windows,
        overrun_events=overrun_events,
        overrun_frames=overrun_frames,
        min_ring_fill=min_ring_fill,
        max_ring_fill=max_ring_fill,
        mean_ring_fill=mean_ring_fill,
        min_queue_fill=min_queue_fill,
        max_queue_fill=max_queue_fill,
        mean_queue_fill=mean_queue_fill,
        time_under_target_ms=time_under_target_ms,
        glitch=glitch,
    )


def percentile(values: List[float], p: float) -> float:
    if not values:
        return 0.0
    if p <= 0:
        return min(values)
    if p >= 100:
        return max(values)
    idx = (len(values) - 1) * (p / 100.0)
    lo = int(math.floor(idx))
    hi = int(math.ceil(idx))
    if lo == hi:
        return values[lo]
    frac = idx - lo
    return values[lo] * (1.0 - frac) + values[hi] * frac


def run_monte_carlo(cfg: EffectiveConfig, keep_trials: bool = True) -> Tuple[MonteCarloSummary, List[TrialResult]]:
    if cfg.trials <= 0:
        summary = MonteCarloSummary(
            enabled=False,
            trials=0,
            duration_sec=cfg.duration_sec,
            jitter_model=cfg.jitter_model,
            jitter_std_us=cfg.jitter_std_us,
            seed=cfg.seed,
            glitch_trials=0,
            glitch_probability=0.0,
            glitch_probability_ci_low=0.0,
            glitch_probability_ci_high=0.0,
            underrun_trials=0,
            overrun_trials=0,
            avg_underrun_events=0.0,
            avg_underrun_frames=0.0,
            avg_underrun_windows=0.0,
            avg_overrun_events=0.0,
            avg_time_under_target_ms=0.0,
            p95_time_under_target_ms=0.0,
            avg_mean_ring_fill=0.0,
            avg_mean_queue_fill=0.0,
        )
        return summary, []

    base_seed = cfg.seed if cfg.seed is not None else random.randint(1, 2_147_483_647)
    trials: List[TrialResult] = []
    glitch_trials = 0
    underrun_trials = 0
    overrun_trials = 0
    sum_underrun_events = 0.0
    sum_underrun_frames = 0.0
    sum_underrun_windows = 0.0
    sum_overrun_events = 0.0
    sum_time_under = 0.0
    sum_mean_ring = 0.0
    sum_mean_queue = 0.0
    time_under_values: List[float] = []

    for i in range(cfg.trials):
        trial_seed = base_seed + i * 7919
        t = run_single_trial(cfg, i, trial_seed)
        if keep_trials:
            trials.append(t)
        if t.glitch:
            glitch_trials += 1
        if t.underrun_events > 0:
            underrun_trials += 1
        if t.overrun_events > 0:
            overrun_trials += 1
        sum_underrun_events += t.underrun_events
        sum_underrun_frames += t.underrun_frames
        sum_underrun_windows += t.underrun_windows
        sum_overrun_events += t.overrun_events
        sum_time_under += t.time_under_target_ms
        sum_mean_ring += t.mean_ring_fill
        sum_mean_queue += t.mean_queue_fill
        time_under_values.append(float(t.time_under_target_ms))

    time_under_values.sort()
    ci_low, ci_high = wilson_interval(glitch_trials, cfg.trials)

    summary = MonteCarloSummary(
        enabled=True,
        trials=cfg.trials,
        duration_sec=cfg.duration_sec,
        jitter_model=cfg.jitter_model,
        jitter_std_us=cfg.jitter_std_us,
        seed=base_seed,
        glitch_trials=glitch_trials,
        glitch_probability=float(glitch_trials) / float(cfg.trials),
        glitch_probability_ci_low=ci_low,
        glitch_probability_ci_high=ci_high,
        underrun_trials=underrun_trials,
        overrun_trials=overrun_trials,
        avg_underrun_events=sum_underrun_events / float(cfg.trials),
        avg_underrun_frames=sum_underrun_frames / float(cfg.trials),
        avg_underrun_windows=sum_underrun_windows / float(cfg.trials),
        avg_overrun_events=sum_overrun_events / float(cfg.trials),
        avg_time_under_target_ms=sum_time_under / float(cfg.trials),
        p95_time_under_target_ms=percentile(time_under_values, 95.0),
        avg_mean_ring_fill=sum_mean_ring / float(cfg.trials),
        avg_mean_queue_fill=sum_mean_queue / float(cfg.trials),
    )
    return summary, trials


def parse_sweep_spec(spec: str, rb_max: int) -> List[int]:
    s = spec.strip().lower()
    if s == "auto":
        step = 64
        start = max(64, min(step, rb_max))
        return list(range(start, rb_max + 1, step))

    m = re.fullmatch(r"(\d+):(\d+):(\d+)", s)
    if m:
        start = int(m.group(1))
        stop = int(m.group(2))
        step = int(m.group(3))
        if step <= 0:
            fail("sweep step must be > 0")
        if start <= 0 or stop <= 0:
            fail("sweep range values must be > 0")
        if start > stop:
            fail("sweep start must be <= stop")
        return list(range(start, stop + 1, step))

    if "," in s:
        vals = []
        for token in s.split(","):
            token = token.strip()
            if not token:
                continue
            if not token.isdigit():
                fail(f"invalid sweep token: {token}")
            vals.append(int(token))
        if not vals:
            fail("empty sweep list")
        return sorted(set(vals))

    fail("invalid --sweep format. Use auto, min:max:step, or comma-separated list")
    return []


def run_sweep(
    cfg: EffectiveConfig,
    sweep_targets: List[int],
    risk_threshold: float,
) -> Tuple[List[SweepRow], Optional[int]]:
    rows: List[SweepRow] = []
    selected: Optional[int] = None

    for target in sweep_targets:
        if target <= 0:
            continue
        if target > cfg.rb_max:
            continue

        sweep_cfg = EffectiveConfig(**asdict(cfg))
        sweep_cfg.rb_target = target
        summary, _ = run_monte_carlo(sweep_cfg, keep_trials=False)
        meets = summary.glitch_probability_ci_high <= risk_threshold
        if meets and selected is None:
            selected = target
        rows.append(
            SweepRow(
                rb_target=target,
                glitch_probability=summary.glitch_probability,
                glitch_probability_ci_high=summary.glitch_probability_ci_high,
                avg_underrun_events=summary.avg_underrun_events,
                avg_overrun_events=summary.avg_overrun_events,
                meets_threshold=meets,
            )
        )

    return rows, selected


def print_console_report(
    cfg: EffectiveConfig,
    parsed: ParsedProfiles,
    deterministic: DeterministicResult,
    summary: MonteCarloSummary,
    warnings: List[str],
    sweep_rows: Optional[List[SweepRow]] = None,
    sweep_pick: Optional[int] = None,
) -> None:
    print("=" * 78)
    print("ASFW TX Buffer / Latency Calculator (48k-centric)")
    print("=" * 78)
    print(f"Profile source: {parsed.source_path}")
    print(f"Selected profile: {cfg.profile} (fallback={parsed.used_fallback})")
    print(
        f"Mode: {cfg.stream_mode}, rate={cfg.sample_rate}Hz, channels={cfg.channels}, "
        f"coreBuffer={cfg.core_buffer_frames}f, queueCap={cfg.tx_queue_capacity}f, refillHz={cfg.refill_hz:g}"
    )
    print()

    p = deterministic.packetization
    print("Packetization:")
    print(
        f"  data/no-data per 8 cycles: {p['data_packets_per_8_cycles']:.0f}/{p['no_data_packets_per_8_cycles']:.0f}, "
        f"samples/dataPkt={p['samples_per_data_packet']:.0f}, payload={p['payload_bytes']:.0f}B"
    )
    print(
        f"  avg frames/cycle={p['avg_frames_per_cycle']:.3f}, avg frames/ms={p['avg_frames_per_ms']:.3f}, "
        f"payload_ok={'YES' if p['payload_ok'] > 0.5 else 'NO'}"
    )
    print()

    print("Depths:")
    for key in ("start_wait", "rb_target", "rb_max", "queue_capacity"):
        d = deterministic.depth_conversions[key]
        print(f"  {key:>12}: {d['frames']:.0f}f | {d['ms']:.3f}ms | {d['cycles']:.2f} cycles")
    print()

    s = deterministic.startup
    print("Startup pre-prime:")
    print(
        f"  prefill={s['start_prefill_frames_assumed']}f, primeBudget={s['prime_budget_frames']}f, "
        f"limit={s['prime_limit_desc']}, meetsStartWait={'YES' if s['prime_budget_meets_start_wait'] else 'NO'}"
    )
    print()

    l = deterministic.latency_estimates
    transport = l["transport_only"]
    adk = l["adk_inclusive"]
    print("Latency estimates:")
    print(
        f"  transport-only: {transport['frames_est']:.0f}f ({transport['ms_est']:.3f}ms), "
        f"worst={transport['frames_worst_est']:.0f}f ({transport['ms_worst_est']:.3f}ms)"
    )
    if cfg.include_adk_latency:
        print(
            f"  adk-inclusive : {adk['frames_est']:.0f}f ({adk['ms_est']:.3f}ms), "
            f"worst={adk['frames_worst_est']:.0f}f ({adk['ms_worst_est']:.3f}ms) "
            f"(added={adk['added_frames']:.0f}f/{adk['added_ms']:.3f}ms)"
        )
    else:
        print(
            f"  adk-inclusive : disabled (would add {adk['added_frames']:.0f}f/{adk['added_ms']:.3f}ms)"
        )
    print()

    if summary.enabled:
        print("Monte Carlo:")
        print(
            f"  trials={summary.trials}, duration={summary.duration_sec:.2f}s, "
            f"jitter={summary.jitter_model} cbStd={summary.jitter_std_us:.1f}us "
            f"refillStd={cfg.refill_jitter_std_us:.1f}us, seed={summary.seed}"
        )
        print(
            f"  glitchProb={summary.glitch_probability:.4f} "
            f"(95% CI {summary.glitch_probability_ci_low:.4f}..{summary.glitch_probability_ci_high:.4f}), "
            f"glitchTrials={summary.glitch_trials}, "
            f"underrunTrials={summary.underrun_trials}, overrunTrials={summary.overrun_trials}"
        )
        print(
            f"  avgUnderrunEvents={summary.avg_underrun_events:.3f}, "
            f"avgUnderrunFrames={summary.avg_underrun_frames:.3f}, "
            f"avgUnderrunWindows={summary.avg_underrun_windows:.3f}, "
            f"avgOverrunEvents={summary.avg_overrun_events:.3f}, "
            f"avgTimeUnderTarget={summary.avg_time_under_target_ms:.1f}ms "
            f"(p95={summary.p95_time_under_target_ms:.1f}ms)"
        )
        print(
            f"  avgMeanRingFill={summary.avg_mean_ring_fill:.2f}f, "
            f"avgMeanQueueFill={summary.avg_mean_queue_fill:.2f}f"
        )
    else:
        print("Monte Carlo: disabled")
    print()

    if sweep_rows is not None:
        print("Sweep:")
        if not sweep_rows:
            print("  no valid sweep rows")
        else:
            print("  target | glitchProb | ciHigh | avgUnderruns | avgOverruns | meets")
            for row in sweep_rows:
                print(
                    f"  {row.rb_target:>6} | {row.glitch_probability:>10.4f} | {row.glitch_probability_ci_high:>6.4f} | "
                    f"{row.avg_underrun_events:>11.3f} | {row.avg_overrun_events:>10.3f} | "
                    f"{'YES' if row.meets_threshold else 'NO'}"
                )
            if sweep_pick is not None:
                print(f"  recommended minimum safe target: {sweep_pick} frames")
            else:
                print("  no target met risk threshold")
        print()

    all_warnings = list(parsed.warnings) + warnings + deterministic.notes
    if all_warnings:
        print("Warnings/Notes:")
        for w in all_warnings:
            print(f"  - {w}")
        print()


def write_json_output(
    path: Path,
    cfg: EffectiveConfig,
    parsed: ParsedProfiles,
    deterministic: DeterministicResult,
    summary: MonteCarloSummary,
    trials: List[TrialResult],
    sweep_rows: Optional[List[SweepRow]],
    sweep_pick: Optional[int],
    warnings: List[str],
) -> None:
    cfg_dict = asdict(cfg)
    samples = cfg_dict.get("jitter_samples_ms")
    cfg_dict["jitter_samples_count"] = len(samples) if isinstance(samples, list) else 0
    cfg_dict["jitter_samples_ms"] = None

    payload = {
        "config": cfg_dict,
        "parsed_profiles": {
            "selected_profile": parsed.selected_profile,
            "used_fallback": parsed.used_fallback,
            "source_path": parsed.source_path,
            "profiles": {k: asdict(v) for (k, v) in parsed.profiles.items()},
            "warnings": parsed.warnings,
        },
        "deterministic": asdict(deterministic),
        "monte_carlo_summary": asdict(summary),
        "trials": [asdict(t) for t in trials],
        "sweep": {
            "rows": [asdict(r) for r in sweep_rows] if sweep_rows is not None else None,
            "recommended_target": sweep_pick,
        },
        "warnings": warnings,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")


def write_csv_output(
    path: Path,
    trials: List[TrialResult],
    sweep_rows: Optional[List[SweepRow]],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as f:
        if sweep_rows is not None:
            writer = csv.DictWriter(
                f,
                fieldnames=[
                    "kind",
                    "rb_target",
                    "glitch_probability",
                    "glitch_probability_ci_high",
                    "avg_underrun_events",
                    "avg_overrun_events",
                    "meets_threshold",
                ],
            )
            writer.writeheader()
            for row in sweep_rows:
                writer.writerow(
                    {
                        "kind": "sweep",
                        "rb_target": row.rb_target,
                        "glitch_probability": f"{row.glitch_probability:.8f}",
                        "glitch_probability_ci_high": f"{row.glitch_probability_ci_high:.8f}",
                        "avg_underrun_events": f"{row.avg_underrun_events:.8f}",
                        "avg_overrun_events": f"{row.avg_overrun_events:.8f}",
                        "meets_threshold": int(row.meets_threshold),
                    }
                )
        else:
            writer = csv.DictWriter(
                f,
                fieldnames=[
                    "kind",
                    "trial_index",
                    "underrun_events",
                    "underrun_frames",
                    "underrun_windows",
                    "overrun_events",
                    "overrun_frames",
                    "min_ring_fill",
                    "max_ring_fill",
                    "mean_ring_fill",
                    "min_queue_fill",
                    "max_queue_fill",
                    "mean_queue_fill",
                    "time_under_target_ms",
                    "glitch",
                ],
            )
            writer.writeheader()
            for t in trials:
                writer.writerow(
                    {
                        "kind": "trial",
                        "trial_index": t.trial_index,
                        "underrun_events": t.underrun_events,
                        "underrun_frames": t.underrun_frames,
                        "underrun_windows": t.underrun_windows,
                        "overrun_events": t.overrun_events,
                        "overrun_frames": t.overrun_frames,
                        "min_ring_fill": t.min_ring_fill,
                        "max_ring_fill": t.max_ring_fill,
                        "mean_ring_fill": f"{t.mean_ring_fill:.8f}",
                        "min_queue_fill": t.min_queue_fill,
                        "max_queue_fill": t.max_queue_fill,
                        "mean_queue_fill": f"{t.mean_queue_fill:.8f}",
                        "time_under_target_ms": t.time_under_target_ms,
                        "glitch": int(t.glitch),
                    }
                )


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="ASFW TX buffer and latency calculator (48k-centric)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Examples:
  calc_buffer_sizes.py --profile C --stream-mode blocking --trials 400
  calc_buffer_sizes.py --profile B --include-adk-latency --json-out /tmp/tx.json
  calc_buffer_sizes.py --profile A --sweep 128:1024:64 --risk-threshold 0.01 --csv-out /tmp/sweep.csv
""",
    )
    parser.add_argument("--profile", choices=["A", "B", "C"], help="profile preset to use")
    parser.add_argument("--sample-rate", type=int, default=48_000)
    parser.add_argument("--channels", type=int, default=2)
    parser.add_argument("--stream-mode", choices=["blocking", "non-blocking"], default="blocking")
    parser.add_argument("--core-buffer-frames", type=int, default=512)
    parser.add_argument("--tx-queue-capacity", type=int, default=K_TX_QUEUE_CAPACITY_DEFAULT)
    parser.add_argument("--start-wait", type=int, help="override profile startWait")
    parser.add_argument("--prime-limit", type=int, help="override profile startupPrimeLimit (0=unbounded)")
    parser.add_argument("--rb-target", type=int, help="override profile rbTarget")
    parser.add_argument("--rb-max", type=int, help="override profile rbMax")
    parser.add_argument("--max-chunks", type=int, help="override profile maxChunksPerRefill")
    parser.add_argument("--duration-sec", type=float, default=10.0)
    parser.add_argument("--trials", type=int, default=200)
    parser.add_argument("--jitter-model", choices=["gaussian", "uniform", "none", "empirical"], default="gaussian")
    parser.add_argument("--jitter-std-us", type=float, default=120.0)
    parser.add_argument("--jitter-samples-csv", type=Path, default=None, help="empirical jitter samples (microseconds, first column)")
    parser.add_argument("--refill-hz", type=float, default=1000.0)
    parser.add_argument("--refill-jitter-std-us", type=float, default=0.0)
    parser.add_argument("--seed", type=int, default=None)
    parser.add_argument(
        "--sweep",
        type=str,
        default=None,
        help="target sweep spec: auto | min:max:step | v1,v2,v3",
    )
    parser.add_argument("--risk-threshold", type=float, default=0.01)
    parser.add_argument("--include-adk-latency", action="store_true")
    parser.add_argument("--json-out", type=Path, default=None)
    parser.add_argument("--csv-out", type=Path, default=None)
    parser.add_argument("--profile-header", type=Path, default=None, help="optional explicit AudioTxProfiles.hpp path")
    parser.add_argument("--ring-capacity", type=int, default=K_RING_CAPACITY_DEFAULT)
    parser.add_argument("--adk-output-latency-frames", type=int, default=K_ADK_OUTPUT_LATENCY_FRAMES_DEFAULT)
    parser.add_argument("--adk-output-safety-frames", type=int, default=K_ADK_OUTPUT_SAFETY_FRAMES_DEFAULT)
    parser.add_argument("--keep-trials", action="store_true", help="retain per-trial rows in JSON output")
    parser.add_argument("--force", action="store_true", help="disable safety cap for high trial counts")
    return parser


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()

    warnings: List[str] = []
    if args.risk_threshold < 0.0 or args.risk_threshold > 1.0:
        fail("risk-threshold must be in [0,1]")

    jitter_samples_ms: Optional[List[float]] = None
    if args.jitter_samples_csv is not None:
        jitter_samples_ms = load_jitter_samples_ms(args.jitter_samples_csv)
        if len(jitter_samples_ms) < 32:
            warnings.append(f"empirical jitter sample set is small ({len(jitter_samples_ms)} rows)")

    script_path = Path(__file__).resolve()
    repo_root = script_path.parent.parent
    default_header = repo_root / "ASFWDriver" / "Isoch" / "Config" / "AudioTxProfiles.hpp"
    header_path = args.profile_header if args.profile_header else default_header

    parsed = parse_tx_profiles(header_path)

    profile_name = args.profile if args.profile else parsed.selected_profile
    if profile_name not in parsed.profiles:
        fail(f"profile {profile_name} is unavailable")
    profile = parsed.profiles[profile_name]

    # Apply overrides.
    start_wait = args.start_wait if args.start_wait is not None else profile.start_wait_frames
    prime_limit = args.prime_limit if args.prime_limit is not None else profile.startup_prime_limit_frames
    rb_target = args.rb_target if args.rb_target is not None else profile.rb_target_frames
    rb_max = args.rb_max if args.rb_max is not None else profile.rb_max_frames
    max_chunks = args.max_chunks if args.max_chunks is not None else profile.max_chunks_per_refill

    if args.trials > K_MAX_TRIALS_DEFAULT and not args.force:
        warnings.append(
            f"trials capped from {args.trials} to {K_MAX_TRIALS_DEFAULT}; use --force to override"
        )
        trials = K_MAX_TRIALS_DEFAULT
    else:
        trials = args.trials

    cfg = EffectiveConfig(
        profile=profile_name,
        sample_rate=args.sample_rate,
        channels=args.channels,
        stream_mode=args.stream_mode,
        core_buffer_frames=args.core_buffer_frames,
        tx_queue_capacity=args.tx_queue_capacity,
        ring_capacity=args.ring_capacity,
        start_wait=start_wait,
        prime_limit=prime_limit,
        rb_target=rb_target,
        rb_max=rb_max,
        max_chunks=max_chunks,
        duration_sec=args.duration_sec,
        trials=trials,
        jitter_model=args.jitter_model,
        jitter_std_us=args.jitter_std_us,
        refill_hz=args.refill_hz,
        refill_jitter_std_us=args.refill_jitter_std_us,
        seed=args.seed,
        jitter_samples_ms=jitter_samples_ms,
        include_adk_latency=args.include_adk_latency,
        adk_output_latency_frames=args.adk_output_latency_frames,
        adk_output_safety_frames=args.adk_output_safety_frames,
    )

    validate_profile(
        TxBufferProfile(
            name=cfg.profile,
            start_wait_frames=cfg.start_wait,
            startup_prime_limit_frames=cfg.prime_limit,
            rb_target_frames=cfg.rb_target,
            rb_max_frames=cfg.rb_max,
            max_chunks_per_refill=cfg.max_chunks,
        ),
        tx_queue_capacity=cfg.tx_queue_capacity,
        ring_capacity=cfg.ring_capacity,
    )
    validate_config(cfg)

    deterministic = deterministic_calc(cfg)

    # 48k-centric guardrail: Monte Carlo only for 48k.
    if cfg.sample_rate != 48_000 and cfg.trials > 0:
        warnings.append(
            f"sample-rate {cfg.sample_rate}Hz is outside 48k simulation scope; Monte Carlo disabled"
        )
        cfg = EffectiveConfig(**{**asdict(cfg), "trials": 0})

    keep_trials = args.keep_trials or (args.csv_out is not None and args.sweep is None)
    summary, trials_data = run_monte_carlo(cfg, keep_trials=keep_trials)

    sweep_rows: Optional[List[SweepRow]] = None
    sweep_pick: Optional[int] = None
    if args.sweep is not None:
        if cfg.trials <= 0:
            warnings.append("sweep requested but trials=0; no sweep executed")
            sweep_rows = []
        else:
            targets = parse_sweep_spec(args.sweep, cfg.rb_max)
            sweep_rows, sweep_pick = run_sweep(cfg, targets, args.risk_threshold)

    print_console_report(
        cfg=cfg,
        parsed=parsed,
        deterministic=deterministic,
        summary=summary,
        warnings=warnings,
        sweep_rows=sweep_rows,
        sweep_pick=sweep_pick,
    )

    if args.json_out:
        write_json_output(
            path=args.json_out,
            cfg=cfg,
            parsed=parsed,
            deterministic=deterministic,
            summary=summary,
            trials=trials_data,
            sweep_rows=sweep_rows,
            sweep_pick=sweep_pick,
            warnings=warnings,
        )

    if args.csv_out:
        write_csv_output(args.csv_out, trials_data, sweep_rows)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
