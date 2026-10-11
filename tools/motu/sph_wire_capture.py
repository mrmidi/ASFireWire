#!/usr/bin/env python3
"""Summarise passive-observer MOTU SPH captures (one CSV per direction).

The observer keeps each packet's bus cycle and its first SPH. With both
directions captured on one bus, the host's transmit SPH and the device's
capture SPH on the same cycle can be compared directly: their difference is
the host's presentation phase against the device's own sample clock, with no
model in between.

Input columns (observer README): wire_sec, wire_cycle, packet, sph_cycle,
sph_offset, optional burst and gap_before. Uses only the standard library and
never accesses hardware. Data files are read, never executed.
"""
import argparse
import csv
import json
import statistics
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

TICKS_PER_CYCLE = 3072
CYCLES_PER_SECOND = 8000
TICKS_PER_SECOND = TICKS_PER_CYCLE * CYCLES_PER_SECOND


@dataclass(frozen=True)
class DataPacket:
    index: int
    burst: int
    gap_before: bool
    wire_sec: int
    wire_cycle: int
    sph_ticks: int  # on the one-second SPH timeline

    @property
    def lead_ticks(self):
        """SPH ahead of the start of the cycle the packet appeared on the bus."""
        return wrap_difference(self.sph_ticks, self.wire_cycle * TICKS_PER_CYCLE)


def wrap_difference(a, b, period=TICKS_PER_SECOND):
    """a - b on a wrapping timeline, folded to (-period/2, period/2]."""
    value = (a - b) % period
    return value - period if value > period // 2 else value


def load_data_packets(path):
    packets = []
    with open(path, newline='') as file:
        for index, row in enumerate(csv.DictReader(file)):
            if 'cycle' in row and 'sph' in row:
                # Compact fixture cycles are already unwrapped across the
                # observer's eight-second timer. Burst gaps remain explicit
                # in cycle spacing; never infer samples across them.
                cycle, sph = int(row['cycle']), int(row['sph'], 16)
                previous_cycle = packets[-1].wire_sec * CYCLES_PER_SECOND + packets[-1].wire_cycle if packets else cycle
                packets.append(DataPacket(index=index, burst=0,
                    gap_before=cycle - previous_cycle > 2,
                    wire_sec=cycle // CYCLES_PER_SECOND, wire_cycle=cycle % CYCLES_PER_SECOND,
                    sph_ticks=((sph >> 12) & 0x1fff) * TICKS_PER_CYCLE + (sph & 0xfff)))
                continue
            if row['packet'] != 'DATA':
                continue
            packets.append(DataPacket(
                index=int(row.get('index', index)),
                burst=int(row.get('burst') or 0),
                gap_before=row.get('gap_before') == 'yes',
                wire_sec=int(row['wire_sec']),
                wire_cycle=int(row['wire_cycle']),
                sph_ticks=int(row['sph_cycle']) * TICKS_PER_CYCLE + int(row['sph_offset'])))
    return packets


def step_histogram(packets):
    """SPH steps between consecutive DATA packets, not across observer gaps."""
    steps = Counter()
    for previous, packet in zip(packets, packets[1:]):
        if packet.gap_before or packet.burst != previous.burst:
            continue
        steps[wrap_difference(packet.sph_ticks, previous.sph_ticks)] += 1
    return steps


def mean_step(steps, near, window=8):
    """Mean of the steps within `window` ticks of `near` (one DATA packet apart)."""
    picked = {step: count for step, count in steps.items() if abs(step - near) <= window}
    total = sum(picked.values())
    return sum(step * count for step, count in picked.items()) / total if total else None


def same_cycle_offsets(host_to_device, device_to_host):
    """Host TX SPH minus device capture SPH, for DATA packets on the same bus cycle."""
    device = {(p.burst, p.wire_sec, p.wire_cycle): p for p in device_to_host}
    pairs = []
    for packet in host_to_device:
        match = device.get((packet.burst, packet.wire_sec, packet.wire_cycle))
        if match is not None:
            pairs.append((packet, wrap_difference(packet.sph_ticks, match.sph_ticks)))
    return pairs


def offset_summary(pairs, frame_ticks=512):
    offsets = [offset for _, offset in pairs]
    if not offsets:
        return {'pairs': 0}
    mode, mode_count = Counter(offsets).most_common(1)[0]
    return {
        'pairs': len(offsets),
        'mode_ticks': mode,
        'mode_share': round(mode_count / len(offsets), 4),
        'median_ticks': statistics.median(offsets),
        'mode_frames': mode // frame_ticks,
        'mode_residue_ticks': mode % frame_ticks,
        'within_1_tick_of_mode': round(sum(abs(o - mode) <= 1 for o in offsets) / len(offsets), 4),
    }


def offset_timeline(pairs, bucket_packets):
    """Median offset per bucket of host packets, for an acquisition timeline."""
    timeline = []
    for start in range(0, len(pairs), bucket_packets):
        bucket = pairs[start:start + bucket_packets]
        offsets = [offset for _, offset in bucket]
        timeline.append({'first_index': bucket[0][0].index, 'median_ticks': statistics.median(offsets),
                         'min_ticks': min(offsets), 'max_ticks': max(offsets)})
    return timeline


def summarize_direction(packets, frames_per_packet=8, frame_ticks=512):
    steps = step_histogram(packets)
    nominal = frames_per_packet * frame_ticks
    step = mean_step(steps, nominal)
    leads = [p.lead_ticks for p in packets]
    return {
        'data_packets': len(packets),
        'first_lead_ticks': leads[0] if leads else None,
        'lead_min_ticks': min(leads, default=None),
        'lead_max_ticks': max(leads, default=None),
        'steps_top': steps.most_common(5),
        'mean_step_ticks': step,
        # Ticks per frame against nominal: positive = longer frames, a slower
        # sample clock than the bus-derived nominal.
        'period_ppm_vs_nominal': None if step is None else (step / nominal - 1) * 1e6,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('host_to_device', type=Path, help='CSV of the host transmit channel')
    parser.add_argument('device_to_host', type=Path, nargs='?', help='CSV of the device transmit channel, same capture')
    parser.add_argument('--timeline-bucket', type=int, default=0, help='Packets per timeline bucket (0 = none)')
    args = parser.parse_args()
    host = load_data_packets(args.host_to_device)
    report = {'host_to_device': summarize_direction(host)}
    if args.device_to_host:
        device = load_data_packets(args.device_to_host)
        report['device_to_host'] = summarize_direction(device)
        pairs = same_cycle_offsets(host, device)
        report['same_cycle_offset'] = offset_summary(pairs)
        if args.timeline_bucket > 0:
            report['offset_timeline'] = offset_timeline(pairs, args.timeline_bucket)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
