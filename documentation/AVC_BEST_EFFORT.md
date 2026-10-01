# Best-effort AV/C audio publication

A standard AV/C Config ROM unit can enter discovery without a vendor/model
profile. An ordinary unprofiled BeBoB or Oxford catalog row takes the same
path. Explicit firmware hazards, bootloader personas, and specialized device
families retain their existing command policies.

Discovery must find an Audio or Music subunit. It first uses the existing
routed descriptor graph. If descriptors cannot provide geometry, it reads the
current compound AM824 formats of unit ISO input/output plug 0. PCM and MIDI
counts and the current rate come from those replies. No clock or routing is
changed by these queries. Without a descriptor map, PCM uses the reference
PCM-first slot layout; this is explicitly recorded as a fallback map.

Publication requires usable PCM geometry in both directions and matching
current rates. Unsupported content, MIDI-only streams, malformed geometry,
and unavailable formats produce no audio nub. The generic runtime uses the
shared AV/C/CMP lifecycle and the published geometry, with no vendor mixer
programming. It exposes the observed rate; another rate requires fresh
geometry. Device-specific working implementations remain selected as before.

Inspect the discovery-to-stream path with:

```sh
/usr/bin/log show --last 10m --info --debug --style compact \
  --predicate 'eventMessage CONTAINS "[AvcGeometry]" OR eventMessage CONTAINS "[AvcGraphConfig]" OR eventMessage CONTAINS "[AvcPublish]" OR eventMessage CONTAINS "[AvcRuntime]" OR eventMessage CONTAINS "[AvcGraphBind]"'
```

Discovery and publication demonstrate that capabilities were usable. Actual
streaming on an untested device remains best effort: device firmware may require
additional quirks or controls, which should be investigated with an AV/C Report.
