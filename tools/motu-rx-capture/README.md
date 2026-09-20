# MOTU RX timing capture

Build and use with a driver containing selector 64:

```sh
swiftc tools/motu-rx-capture/main.swift -o /tmp/motu-rx-capture
/tmp/motu-rx-capture arm 0001f20001234567
/tmp/motu-rx-capture dump 0001f20001234567 > motu-48000-rx.txt
```

Replace the example with the device's full GUID. Optional final argument selects
RX stream (default 0). `stop GUID` disarms without discarding the capture.
`dump` freezes and exports the capture; retry if the command reports busy.
Capture uses fixed storage, copies at most 64 packets, and does not allocate,
wait, or log in the RX callback. It does not change device registers or start audio.
A new `arm` refuses while already armed; dump/stop first. The last snapshot
survives stream stop/recovery until the next arm or driver teardown.

For startup, arm while the MOTU endpoint is enumerated but audio is stopped,
then start playback and dump. For steady state, arm during playback and dump.
Repeat at 44.1 and 48 kHz and label the files with the selected clock source
and firmware from the device UI. Those fields remain Unknown/0 in the capture
unless independently known; zero sample rate means the binding was unavailable. The first captured
packet updates the session rate, and every packet records its binding rate. This avoids treating the ROM software-version/model selector as a
firmware version. These are device-produced timing vectors **under ASFW**, not
proof of what the original MOTU driver transmits.

Output preserves the full per-packet receive timestamp, epoch, CIP words,
packet length, configured block stride, raw SPHs and decoded tick offsets.
Malformed/truncated packets remain visible, including timestamp validity,
trailing bytes, and SPH truncation. Every stored SPH is exported (up to 32 per
packet); this is a timing diagnostic, not a full PCM packet dump. `dropped`
includes packets beyond the fixed window and packets skipped during control
access. Inspect it before treating the capture as a continuous run.

No hardware capture is performed by building or testing this tool. Opening the
user client requires the same local permissions as the existing asfw-version CLI.
