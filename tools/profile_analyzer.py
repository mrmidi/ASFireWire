#!/usr/bin/env python3
"""Capture and summarize symbolicated Time Profiler stacks without Instruments UI."""
import argparse
import collections
import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser()
parser.add_argument("--pid", type=int)
parser.add_argument("--seconds", type=int, default=15)
parser.add_argument("--trace", type=Path, required=True)
parser.add_argument("--json", type=Path, required=True)
args = parser.parse_args()
if args.pid:
    subprocess.run(["xcrun", "xctrace", "record", "--template", "Time Profiler",
                    "--attach", str(args.pid), "--time-limit", f"{args.seconds}s",
                    "--output", str(args.trace)], check=True)
xml = subprocess.check_output(["xcrun", "xctrace", "export", "--input", str(args.trace),
    "--xpath", '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]'])
root = ET.fromstring(xml)
definitions = {e.get("id"): e for e in root.iter() if e.get("id")}
def resolve(element):
    return definitions[element.get("ref")] if element is not None and element.get("ref") else element
def number(element):
    return int(resolve(element).text)
leaf = collections.Counter()
inclusive = collections.Counter()
categories = collections.Counter()
times = []
total = 0
skipped = 0
groups = {
    "swiftui_graph": ("SwiftUI", "SwiftUICore", "AttributeGraph", "Observation"),
    "metal_driver": ("Metal", "MetalKit", "AGXMetal", "IOGPU"),
}
for row in root.findall(".//row"):
    stack = resolve(row.find("tagged-backtrace"))
    if stack is None or row.find("weight") is None:
        skipped += 1
        continue
    weight = number(row.find("weight"))
    frames = list(stack)
    names = [resolve(frame).get("name", "") for frame in frames]
    binaries = [(resolve(resolve(frame).find("binary")).get("name", "")
                 if resolve(frame).find("binary") is not None else "") for frame in frames]
    total += weight
    times.append(number(row.find("sample-time")))
    if names:
        leaf[names[0]] += weight
    for name in set(names):
        inclusive[name] += weight
    for category, libraries in groups.items():
        if any(any(lib in binary for lib in libraries) for binary in binaries):
            categories[category] += weight
    if any("ObservationRegistrar" in name for name in names):
        categories["observation_registrar"] += weight
    if any("AudioAnalysisEngine" in name for name in names):
        categories["analysis_engine"] += weight
    if any("AnalyzerPlotRenderer" in name for name in names):
        categories["plot_renderer"] += weight
duration = (max(times) - min(times)) if times else 0
def ranking(counter):
    return [{"symbol": name, "cpu_ms": value / 1e6, "pct": round(value / total * 100, 2)}
            for name, value in counter.most_common(30)] if total else []
report = {
    "trace": str(args.trace), "samples": len(times), "unattributed_rows": skipped, "sampled_cpu_ms": total / 1e6,
    "sample_span_ms": duration / 1e6,
    "estimated_cpu_percent": round(total / duration * 100, 2) if duration else None,
    "categories_inclusive": ranking(categories), "self": ranking(leaf), "inclusive": ranking(inclusive),
    "note": "Statistical samples; inclusive categories overlap. Compare equal workloads/build configurations.",
}
args.json.write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({k: v for k, v in report.items() if k not in ("inclusive",)}, indent=2))
