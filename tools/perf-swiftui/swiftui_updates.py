#!/usr/bin/env python3
"""Summarise a SwiftUI-instrument export (swiftui-updates schema).

Export the table first; the full trace is far too large to dump:

  xcrun xctrace export --input X.trace --toc --output toc.xml
  xcrun xctrace export --input X.trace \
    --xpath '/trace-toc/run[@number="1"]/data/table[@schema="swiftui-updates"]' \
    --output swiftui-updates.xml

Then:
  swiftui_updates.py swiftui-updates.xml                 # top updates + root causes
  swiftui_updates.py swiftui-updates.xml --hierarchy \
    'Layout: UnaryChildGeometry<_FrameLayout>'           # where one update sits

Rates use the span between the first and last update row. Durations are the
instrument's own; the SwiftUI instrument adds tracing cost to the main thread,
so use Time Profiler (without this instrument) for CPU totals.
"""
import argparse
import collections
import xml.etree.ElementTree as ET


def rows(path, process):
    """Yield one dict per row, resolving xctrace id/ref compression."""
    ids = {}
    cols = None

    def value(e):
        ref = e.get('ref')
        if ref is not None:
            return ids.get(ref, ('', ''))
        v = (e.get('fmt') or (e.text or ''), e.text or '')
        for c in e.iter():
            if c.get('id') and c.get('id') not in ids:
                ids[c.get('id')] = v if c is e else (c.get('fmt') or (c.text or ''), c.text or '')
        return v

    for _, el in ET.iterparse(path, events=('end',)):
        if el.tag == 'schema':
            cols = [c.findtext('mnemonic') for c in el.findall('col')]
        elif el.tag == 'row':
            d = dict(zip(cols, (value(c) for c in el)))
            el.clear()
            if process and process not in d.get('process', ('', ''))[0]:
                continue
            yield d


def summary(args):
    agg = collections.defaultdict(lambda: [0, 0])
    causes = collections.Counter()
    cause_ns = collections.Counter()
    first = last = None
    n = 0
    for d in rows(args.xml, args.process):
        n += 1
        t = int(d['start'][1] or 0)
        first = t if first is None else min(first, t)
        last = t if last is None else max(last, t)
        ns = int(d['duration'][1] or 0)
        key = (d.get('update-type', ('', ''))[0], d.get('description', ('', ''))[0])
        agg[key][0] += 1
        agg[key][1] += ns
        rc = d.get('root-causes', ('', ''))[0]
        if rc:
            causes[rc[:140]] += 1
            cause_ns[rc[:140]] += ns
    if not n:
        print('no rows')
        return
    span = max((last - first) / 1e9, 1e-9)
    total = sum(v[1] for v in agg.values())
    print(f'rows={n} span={span:.2f}s total update time {total / 1e6:.1f} ms '
          f'({total / 1e9 / span * 100:.1f}% of one core)')
    print('\ncount     rate     total     mean  type / description')
    for (kind, desc), (c, ns) in sorted(agg.items(), key=lambda kv: -kv[1][1])[:args.top]:
        print(f'{c:6d} {c / span:7.1f}/s {ns / 1e6:7.1f} ms {ns / c / 1e3:7.1f} us  {kind} / {desc[:90]}')
    print('\nroot causes by count')
    for k, c in causes.most_common(args.top // 2):
        print(f'{c:6d} {c / span:7.1f}/s {cause_ns[k] / 1e6:7.1f} ms  {k}')


def hierarchy(args):
    where = collections.Counter()
    for d in rows(args.xml, args.process):
        if d.get('description', ('', ''))[0] == args.hierarchy:
            where[d.get('view-hierarchy', ('', ''))[0][:args.width]] += 1
    for h, c in where.most_common(args.top):
        print(f'{c:6d} | {h}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('xml', help='exported swiftui-updates table')
    p.add_argument('--process', default='ASFW', help='keep rows whose process contains this')
    p.add_argument('--hierarchy', metavar='DESCRIPTION', help='list view hierarchies for one update description')
    p.add_argument('--width', type=int, default=400, help='hierarchy characters to show, innermost first')
    p.add_argument('--top', type=int, default=30)
    args = p.parse_args()
    hierarchy(args) if args.hierarchy else summary(args)


if __name__ == '__main__':
    main()
