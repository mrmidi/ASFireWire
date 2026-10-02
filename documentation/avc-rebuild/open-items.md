# AV/C graph work: open items (hand-off, 2026-09-28)

Done and saved: `fixtures/` (Phase 88 + Duet descriptors, SIGNAL SOURCE edges, inquiries, Apple↔Duet FireBug
trace, `graph_build.py` + graphs), `applefwaudio-graph-rules.md` (Apple's builder: slot map, streams = clusters,
names, MIDI, clock, fallback). **Further research is still needed on the items below.**

1. **The Phase 88 descriptor at 96 kHz: not read.**
   - The rate CONTROL was sent during active 48 kHz playback. Both plugs were ACCEPTED at 96 kHz, then **all
     async to the Phase 88 died**: descriptor OPEN/CLOSE, PLUG INFO and a ROM quadlet read all timed out.
   - Ring records 6072–6094 say "AT completion never arrived … Possible AT context stall", ack code 0.
     `IntEventSet 0x003800B1` (reqTxComplete pending) on two reads. Isochronous kept running at 48 kHz
     geometry; replay resets went 1 → 2.
   - **Unresolved: our AT servicing, or the DM1000 wedged?** Needs AT context registers `0x180`/`0x18C`, which
     MCP doesn't expose. A possible driver bug. **The user replugged the Phase 88 on 2026-09-28, so that live
     state is gone.** To study it: reproduce (rate CONTROL while our 48 kHz stream runs) with AT context register
     capture added first. **96 kHz read DONE (2026-09-28, playback stopped): the music descriptor is byte-identical to 48 kHz (2410 B); saved in the fixture. Rate restored to 48 kHz.** The user expects the
     descriptor not to change with the rate.
   - **PARKED by the user (2026-09-28): not to be investigated.** The user's view: the host did something
     invalid (changing the clock while streams run), not a device or AT defect worth chasing. **Rule for the
     new stack:** a rate change is refused while streams are running; stop the streams, change the rate, restart.
     Before implementing, check the reference ordering (Linux `bebob_stream.c`, Apple's rate path).
2. **The Duet's feature control bitmap: DONE (2026-09-28).** Measured with STATUS + INQUIRY: FB1 has mute and
   volume on channels 0, 1, 2, and nothing else. The Duet's bitmap doesn't describe that under either bit order,
   and its length fields are 1 byte instead of the spec's 2. Rule: confirm controls by STATUS; use the
   descriptor only as a hint. Details: `fixtures/duet_descriptors.md`, `fixtures/duet_fb1_status.json`.
3. **Selector values:** settled. Value = 0-based `source_ID` index (Audio Subunit 1.0 line 2249); all 10 Phase 88
   selectors check out. **Still open:** the first byte of SIGNAL SOURCE replies (`10`/`30`/`70`). Check ta1394
   `ccm` and AVCVideoServices, or the CCM spec.
4. **Apple's host-plug and sync-destination selection** (`AppleFWAudioDevice` fields `+397`/`+396` = `0x18D`/`0x18C`,
   `+5740` = `0x166C`): still open. Next step: in IDA, find the instructions in `InitializeDeviceInfo`
   (`0x3a34`–`0x609b`) that store to those offsets. The session timed out on 2026-09-28 before the query ran.
   The vtable base for slot lookups is `0x3F0E0`. Our derivation via SIGNAL SOURCE works meanwhile.
5. **Runtime changes: answered (2026-09-28).** Takashi Sakamoto's crates define NOTIFY generically (ta1394
   `general/src/lib.rs:425` `AvcNotify`, `:568-590` `notify()`: accept only `CHANGED`), but **no command
   implements it**. The runtimes **poll**: the OXFW runtime uses a 50 ms interval timer
   (`runtime/oxfw/src/main.rs:219-297`), and the BeBoB runtime has similar measure code. The reference answer
   for runtime changes is periodic polling. NOTIFY support on our devices is untested.
6. **Breadth:** only two devices. The Orpheus descriptor and Apple's 174-command capture are with the fork's
   author; the 1814 stays catalog-driven.
7. **The CoreAudio shape** (one stream per cluster like Apple, or one wide stream as today): decided 2026-10-02: keep the current wide stream and record clusters.
