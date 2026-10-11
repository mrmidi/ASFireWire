#!/usr/bin/env python3
"""Extract DATA cycle/first-SPH fixtures from contributor observer CSVs.

Usage: python3 tools/motu/compact_sph_capture.py <source sph directory> <output directory>
No audio, GUIDs, register content or packet interpolation is retained.
"""
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path


def compact(path):
    original = path.read_bytes()
    output = io.StringIO(newline='')
    writer = csv.writer(output,lineterminator='\n')
    writer.writerow(('cycle','sph'))
    first = last = previous = None
    total = data = 0
    elapsed = 0
    for row in csv.DictReader(io.StringIO(original.decode())):
        current = int(row['wire_sec'])*8000+int(row['wire_cycle'])
        elapsed = current if previous is None else elapsed+(current-previous)%64000
        previous = current
        if first is None:
            first = elapsed
        last = elapsed
        total += 1
        if row['packet'] == 'DATA':
            writer.writerow((elapsed,f"{int(row['sph_hex'],16):08x}"))
            data += 1
    return output.getvalue(),dict(source_sha256=hashlib.sha256(original).hexdigest(),
        first_cycle=first,last_cycle=last,total_packets=total,data_packets=data,
        continuous='start-0.5s' in path.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    parser.add_argument('output',type=Path)
    args = parser.parse_args()
    manifest = {}
    args.output.mkdir(parents=True,exist_ok=True)
    for path in sorted(args.source.glob('*.csv')):
        text,meta = compact(path)
        (args.output/path.name).write_text(text)
        manifest[path.name] = meta
    (args.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')

if __name__ == '__main__':
    main()
