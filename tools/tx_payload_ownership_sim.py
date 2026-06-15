#!/usr/bin/env python3
"""
TX Payload Ownership Simulator (Defect B / FW-53).

Models the three-cursor race that decides whether an isochronous TX DATA packet
ships real PCM or silence, and compares the two candidate fixes (PUSH vs PULL)
across lead, ring size, safety offset, and producer-stall jitter via Monte Carlo.

------------------------------------------------------------------------------
THE MODEL
------------------------------------------------------------------------------
Three frame-domain cursors, all nominally 48 kHz, advancing over FireWire
cycles (8000 packets/s, 125 us each). Blocking 48 kHz cadence = (8,8,8,0)
frames per packet => 6 frames/packet average, 6000 data + 2000 no-data per sec.

  T  hardware transmit position. Deterministic: packet i is transmitted at
     cycle i.
  W  CoreAudio write frontier. CoreAudio writes ahead of playout by a fixed
     safety offset, so W(c) = frameAtCycle(c) + safety_offset_frames. (W > T
     by construction.)
  E  producer commit/exposure frontier. The producer targets c + lead packets
     ahead of transmit, but only advances when it actually runs; a stall on the
     single Default queue freezes E, and the backlog is committed in a burst
     when the stall ends.

CORRECTNESS (per DATA packet i):

  DMA deadline (both models): packet i must be committed to the DMA ring at
  least `hardware_ring` packets before it is transmitted. commit_cycle[i] must
  be <= i - hardware_ring, else it is a DMA underrun (the FATAL the refill ISR
  guards -- a different failure class from a PCM dropout).

  PUSH: a separate CoreAudio-thread writer fills the already-exposed zero slot
  when W sweeps past it. PCM survives iff the slot was committed BEFORE the
  writer reached it: commit_cycle[i] <= writer_reach_cycle[i], where
  writer_reach_cycle[i] is the cycle at which W >= frame_end[i]. A stall that
  delays commit past writer_reach => the writer already swept by => the slot
  ships its zero payload. This is `framesWithoutPacket` in the live telemetry.

  PULL: the producer fills PCM from the host ring at commit time, gated on W.
  PCM survives iff the frames already exist when committed:
  W(commit_cycle[i]) >= frame_end[i]. Committing too far ahead of W (large
  lead) means the data is not there yet => genuine underrun (emit silence).
  A stall DELAYS commit, which only makes W more likely to be ahead -- pull is
  robust to stalls until commit slips inside the DMA deadline.

Net: PUSH dropout rises once a stall exceeds the lead margin (lead - safety);
PULL dropout stays ~0 provided safety_offset >= lead (so data is present at
commit), bounded below by hardware_ring. The sim quantifies both.

Constants default to ASFWDriver/Shared/Isoch/AudioTimingGeometry.hpp.

Usage:
    python3 tx_payload_ownership_sim.py run                # current geometry, both models
    python3 tx_payload_ownership_sim.py run --model pull --lead 56
    python3 tx_payload_ownership_sim.py sweep --param lead --min 48 --max 256 --step 8
    python3 tx_payload_ownership_sim.py sweep --param safety-pkts --min 0 --max 96 --step 4
    python3 tx_payload_ownership_sim.py sweep --param stall-max --min 0 --max 200 --step 10
"""

from __future__ import annotations

import argparse
import random
from dataclasses import dataclass, field, replace
from typing import List, Tuple

# -----------------------------------------------------------------------------
# Geometry constants (ASFWDriver/Shared/Isoch/AudioTimingGeometry.hpp)
# -----------------------------------------------------------------------------
CADENCE: Tuple[int, ...] = (8, 8, 8, 0)        # blocking 48 kHz: D,D,D,N
AVG_FRAMES_PER_PKT = sum(CADENCE) / len(CADENCE)  # 6.0

HW_RING_PKTS_DEFAULT = 48                       # kTxHardwareRingPackets
SLACK_PKTS_DEFAULT = 96                         # kTxPreparationSlackPackets (2x HW ring)
LEAD_PKTS_DEFAULT = HW_RING_PKTS_DEFAULT + SLACK_PKTS_DEFAULT  # 144 = kTxPreparationLeadPackets
SHARED_SLOT_PKTS_DEFAULT = 192                  # kTxSharedSlotPackets
HOST_RING_FRAMES_DEFAULT = 1536                 # observed outputFrameCapacity

# Safety offset: CoreAudio leads playout by delayPackets * framesPerPacket.
# Observed in [PayloadWriter] logs as E - sampleTime ~ tens-to-~100 frames,
# i.e. roughly one IO buffer; default to ~16 packets (~96 frames). Tunable.
SAFETY_PKTS_DEFAULT = 16


@dataclass(frozen=True)
class Config:
    n_packets: int = 400_000          # ~50 s of bus time
    warmup_packets: int = 2_000       # skip startup transient from stats
    lead_pkts: int = LEAD_PKTS_DEFAULT
    hw_ring_pkts: int = HW_RING_PKTS_DEFAULT
    shared_slot_pkts: int = SHARED_SLOT_PKTS_DEFAULT
    host_ring_frames: int = HOST_RING_FRAMES_DEFAULT
    safety_pkts: int = SAFETY_PKTS_DEFAULT     # CoreAudio lead over playout, in packets
    # Producer stall process (Default-queue contention). Field note: producer
    # occasionally stalled 40-42 packets (~5 ms) even with a dedicated queue.
    p_stall: float = 0.002            # per-cycle probability of starting a stall
    stall_min: int = 4                # packets
    stall_max: int = 60               # packets
    trials: int = 8
    seed: int = 1


@dataclass
class Stats:
    data_packets: int = 0
    written: int = 0
    pcm_dropouts: int = 0             # DATA packet ships zero (the audible defect)
    dma_underruns: int = 0            # commit slipped inside the HW ring (FATAL class)

    def rate_dropout(self) -> float:
        return self.pcm_dropouts / self.data_packets if self.data_packets else 0.0

    def rate_dma(self) -> float:
        return self.dma_underruns / self.data_packets if self.data_packets else 0.0


def _frame_prefix(n: int) -> List[int]:
    """frame_start[i] = total frames transmitted before packet i."""
    prefix = [0] * (n + 1)
    for i in range(n):
        prefix[i + 1] = prefix[i] + CADENCE[i % len(CADENCE)]
    return prefix


def _simulate_commits(cfg: Config, rng: random.Random) -> List[int]:
    """Per-packet commit cycle under the producer stall process.

    commit_cycle[i] = cycle at which packet i was committed/exposed, or a value
    >= i if it was never committed before its own transmit cycle.
    """
    n = cfg.n_packets
    commit = [n + 1] * n          # sentinel: never committed in time
    committed_through = -1
    stall_until = -1
    for c in range(n):
        if c < stall_until:
            continue              # producer frozen
        if rng.random() < cfg.p_stall:
            stall_until = c + rng.randint(cfg.stall_min, cfg.stall_max)
            continue              # stall begins now; no commit this cycle
        target = min(c + cfg.lead_pkts, n - 1)
        if target > committed_through:
            for k in range(committed_through + 1, target + 1):
                commit[k] = c
            committed_through = target
    return commit


def _writer_reach(cfg: Config, prefix: List[int]) -> List[int]:
    """writer_reach[i] = cycle at which W >= frame_end[i] (W = frameAt + safety)."""
    n = cfg.n_packets
    safety_frames = int(round(cfg.safety_pkts * AVG_FRAMES_PER_PKT))
    reach = [0] * n
    c = 0
    for i in range(n):
        frame_end = prefix[i + 1]
        need = frame_end - safety_frames
        while c < n and prefix[c] < need:
            c += 1
        reach[i] = c
    return reach


def simulate_once(cfg: Config, model: str, rng: random.Random) -> Stats:
    prefix = _frame_prefix(cfg.n_packets)
    commit = _simulate_commits(cfg, rng)
    reach = _writer_reach(cfg, prefix) if model == "push" else None
    safety_frames = int(round(cfg.safety_pkts * AVG_FRAMES_PER_PKT))

    st = Stats()
    for i in range(cfg.warmup_packets, cfg.n_packets):
        if CADENCE[i % len(CADENCE)] == 0:
            continue              # no-data packet: carries no PCM
        st.data_packets += 1
        ci = commit[i]

        # DMA deadline: must be committed hw_ring packets before transmit.
        if ci > i - cfg.hw_ring_pkts:
            st.dma_underruns += 1
            st.pcm_dropouts += 1  # a missed DMA slot also ships no audio
            continue

        if model == "push":
            # PCM survives iff slot was exposed before the writer swept past.
            if ci <= reach[i]:
                st.written += 1
            else:
                st.pcm_dropouts += 1
        else:  # pull
            # PCM survives iff the frames already exist in the host ring at
            # commit time: W(ci) >= frame_end[i].
            if prefix[ci] + safety_frames >= prefix[i + 1]:
                st.written += 1
            else:
                st.pcm_dropouts += 1
    return st


def run_trials(cfg: Config, model: str) -> Tuple[float, float, float, float]:
    """Returns (mean_dropout, p95_dropout, mean_dma, worst_dropout)."""
    rng = random.Random(cfg.seed)
    drops, dmas = [], []
    for _ in range(cfg.trials):
        st = simulate_once(cfg, model, rng)
        drops.append(st.rate_dropout())
        dmas.append(st.rate_dma())
    drops.sort()
    p95 = drops[min(len(drops) - 1, int(0.95 * len(drops)))]
    return (sum(drops) / len(drops), p95, sum(dmas) / len(dmas), drops[-1])


# -----------------------------------------------------------------------------
# CLI
# -----------------------------------------------------------------------------
def _print_header(cfg: Config) -> None:
    slack = cfg.lead_pkts - cfg.hw_ring_pkts
    print(f"  geometry: lead={cfg.lead_pkts}pkt hw_ring={cfg.hw_ring_pkts} "
          f"slack={slack} shared={cfg.shared_slot_pkts} "
          f"safety={cfg.safety_pkts}pkt host_ring={cfg.host_ring_frames}fr")
    print(f"  stalls:   p={cfg.p_stall} len=[{cfg.stall_min},{cfg.stall_max}]pkt "
          f"trials={cfg.trials} packets={cfg.n_packets}")
    # PUSH survives a stall iff lead - safety > stall (slot exposed before the
    # writer sweeps), and needs lead < shared ring to avoid slot reuse.
    # PULL survives iff lead <= safety (data present at fill) AND slack >= stall
    # (commit stays ahead of the DMA deadline). Viable iff hw_ring+stall <= lead <= safety.
    print(f"  PUSH ok if  lead-safety > stall  ({cfg.lead_pkts - cfg.safety_pkts} > {cfg.stall_max}?)"
          f"  and lead < shared ({cfg.lead_pkts} < {cfg.shared_slot_pkts}?)")
    print(f"  PULL ok if  lead <= safety  ({cfg.lead_pkts} <= {cfg.safety_pkts}?)"
          f"  and slack >= stall  ({slack} >= {cfg.stall_max}?)")
    if cfg.lead_pkts >= cfg.shared_slot_pkts:
        print(f"  !! lead ({cfg.lead_pkts}) >= shared ring ({cfg.shared_slot_pkts}): "
              f"slot reuse before consume")
    print()


def cmd_run(cfg: Config, args) -> None:
    print("TX payload ownership simulation")
    _print_header(cfg)
    models = ["push", "pull"] if args.model == "both" else [args.model]
    print(f"  {'model':6} {'dropout(mean)':>14} {'dropout(p95)':>13} "
          f"{'dropout(worst)':>15} {'dma_underrun':>13}")
    for m in models:
        mean, p95, dma, worst = run_trials(cfg, m)
        print(f"  {m:6} {mean*100:>13.4f}% {p95*100:>12.4f}% "
              f"{worst*100:>14.4f}% {dma*100:>12.4f}%")


def cmd_sweep(cfg: Config, args) -> None:
    print(f"TX payload ownership sweep over '{args.param}'")
    _print_header(cfg)
    print(f"  {args.param:>12} {'push_drop':>12} {'pull_drop':>12} "
          f"{'push_dma':>10} {'pull_dma':>10}")
    v = args.min
    while v <= args.max:
        if args.param == "lead":
            c = replace(cfg, lead_pkts=v)
        elif args.param == "safety-pkts":
            c = replace(cfg, safety_pkts=v)
        elif args.param == "stall-max":
            c = replace(cfg, stall_max=max(cfg.stall_min, v))
        elif args.param == "hw-ring":
            c = replace(cfg, hw_ring_pkts=v)
        elif args.param == "shared":
            c = replace(cfg, shared_slot_pkts=v)
        else:
            raise SystemExit(f"unknown --param {args.param}")
        pmean, _, pdma, _ = run_trials(c, "push")
        qmean, _, qdma, _ = run_trials(c, "pull")
        print(f"  {v:>12} {pmean*100:>11.4f}% {qmean*100:>11.4f}% "
              f"{pdma*100:>9.3f}% {qdma*100:>9.3f}%")
        v += args.step


def build_config(args) -> Config:
    base = Config()
    overrides = {}
    for name in ("n_packets", "lead_pkts", "hw_ring_pkts", "shared_slot_pkts",
                 "safety_pkts", "stall_min", "stall_max", "trials", "seed"):
        val = getattr(args, name, None)
        if val is not None:
            overrides[name] = val
    if args.p_stall is not None:
        overrides["p_stall"] = args.p_stall
    return replace(base, **overrides)


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_common(sp):
        sp.add_argument("--lead", dest="lead_pkts", type=int)
        sp.add_argument("--hw-ring", dest="hw_ring_pkts", type=int)
        sp.add_argument("--shared", dest="shared_slot_pkts", type=int)
        sp.add_argument("--safety-pkts", dest="safety_pkts", type=int)
        sp.add_argument("--p-stall", dest="p_stall", type=float)
        sp.add_argument("--stall-min", dest="stall_min", type=int)
        sp.add_argument("--stall-max", dest="stall_max", type=int)
        sp.add_argument("--trials", type=int)
        sp.add_argument("--packets", dest="n_packets", type=int)
        sp.add_argument("--seed", type=int)

    sp_run = sub.add_parser("run", help="single configuration")
    sp_run.add_argument("--model", choices=["push", "pull", "both"], default="both")
    add_common(sp_run)

    sp_sweep = sub.add_parser("sweep", help="vary one parameter, push vs pull")
    sp_sweep.add_argument("--param",
                          choices=["lead", "safety-pkts", "stall-max", "hw-ring", "shared"],
                          required=True)
    sp_sweep.add_argument("--min", type=int, required=True)
    sp_sweep.add_argument("--max", type=int, required=True)
    sp_sweep.add_argument("--step", type=int, default=8)
    add_common(sp_sweep)

    args = p.parse_args()
    cfg = build_config(args)
    if args.cmd == "run":
        cmd_run(cfg, args)
    elif args.cmd == "sweep":
        cmd_sweep(cfg, args)


if __name__ == "__main__":
    main()
