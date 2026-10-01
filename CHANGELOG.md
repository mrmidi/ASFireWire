# Changelog

All notable changes to ASFireWire are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

> **Release process:** entries accumulate under `[Unreleased]`. To cut a release,
> rename that heading to the version being tagged (e.g. `## [0.3.0] - 2026-09-01`),
> add a fresh empty `[Unreleased]`, commit, then push a matching `v*` tag.
>
> The release workflow extracts the section whose version matches the tag and uses
> it as the GitHub Release body. A tag with no matching section still releases —
> the workflow logs a warning and publishes with the standard install/safety notes
> only. See `.github/workflows/release.yml`.

## [Unreleased]

> **First release since 0.3.1.** Version 0.3.2 was prepared but never tagged; its changes ship here and are merged into the lists below.
>
> **Best effort, no guarantees.** This release enables audio for many AV/C and DICE devices that have never been run on ASFireWire. Some will work, some won't. Turn your volume down before the first attach, and please report every result, including "it just works": https://asfirewire.mistermidi.chatgpt.site/test-device/

### Added

- Avid Mbox Pro (3rd generation) DICE audio interface support, including its startup router and mixer configuration. Contributed and hardware-tested by KinoLab07. (#162)
- AV/C: generic discovery and streaming for AV/C audio devices (BeBoB, Oxford) that have no catalog entry. The driver reads what the device reports about itself (plugs, stream formats, the Music and Audio Subunit descriptors, channel names) and publishes it from that, then streams with the standard IEC 61883 plug-connection sequence at the rate the device is already running. Only command forms already proven on real hardware are sent. Known devices keep their own tested setup. Untested on most hardware. Limits: one sample rate per device, playback-only devices are not published yet, and units that send no usable timestamps (some Oxford 970) may play but not record. (#165)
- DICE: every DICE model the catalog recognises now streams through the generic DICE path, with channel counts and rates read from the device: Focusrite Liquid Saffire 56, Saffire Pro 26 and Saffire Pro 40 (TCD3070); PreSonus StudioLive 16.4.2 and 32.4.2; Alesis iO14 / iO26; Mackie Onyx 1640i (DICE run) and Onyx Blackbird; Weiss ADC2, AFI1, Vesta, DAC2, DAC202, Maya and MAN301. The Weiss DACs, the Vesta and the MAN301 use the INT202's output-only policy. None has run on this code. (#166)
- DICE: a DICE unit with no catalog entry is recognised from its Config ROM alone, by the same rule as Linux (interface version 1, unit specifier equal to the GUID's vendor ID, the vendor's category byte, GUID product field equal to the unit model), and streams through the generic DICE path. (#166)
- AV/C Report screen in the app: reruns AV/C discovery on every device and saves the complete FCP conversation as a text report, a JSON dump or a binary dump folder. This is the report to attach for AV/C devices. (#165)
- Audio Analyzer screen (preview): levels and true peak, goniometer and correlation, spectrum (L/R or M/S, peak hold), EBU R128 loudness at 48 kHz, waveform and ring-buffer diagnostics, read directly from the driver's output ring on the GPU. Not yet optimised: about 20% of one CPU core while the screen is open in our measurements, and it may keep running while its window is hidden. Close the screen when you're not using it.
- RME Fireface FF400 and FF800 duplex audio at 48 kHz (18 and 28 channels). The device's saved clock source and mixer routing are kept, an external clock is accepted only when locked at 48 kHz, and unsupported firmware, rates and clock states are refused before streaming. Uses headerless 24-bit PCM packets and neutral skipped isochronous cycles. (#161)
- PreSonus FireStudio Project (`0x000a92:0x00000b`) as a DICE device. (#158)
- The app's System Logs screen can export the driver's retained log ring. (#161)

### Changed

- Audio: the largest buffer size macOS offers is now 4096 frames at 32–96 kHz (it was 576), so applications such as Pro Tools can use 1024 and 2048-frame buffers. Larger buffers have not yet been tried in a DAW. (#149)
- AV/C (internal): the AV/C layer is rebuilt as typed frame and command codecs, one FCP transaction engine, plain-data unit and subunit models, and Music/Audio Subunit descriptor parsing. Devices are published from what discovery found, not from per-device publishers; the per-device BeBoB profile chain is gone. The stream-format command tries `0xBF` first and falls back to `0x2F` once, remembering the answer only after it works; BridgeCo units start on `0x2F`. (#165)
- AV/C: UNIT INFO is always sent with its five operands, as Linux and Apple send it. The bare form wedged a TerraTec Phase 88 badly enough to need a power cycle. (#165)
- AV/C: before a manual rescan, the app asks you to turn speakers and headphones down, since a misbehaving device can reset the bus during playback. (#165)
- DICE: before the first write to a device, the driver now checks that its section table is plausible and that its `GLOBAL_VERSION` major is 1, as Linux does, and refuses the device otherwise. (#166)
- Build: the driver and host tests are compiled as C++26; CI builds on the Xcode 27 image. (#165)
- Audio: each device now has one hardware sample timeline, and the clock macOS reads (its zero timestamps) is a projection of it. Every sample rate on macOS's ladder gets a timeline; on M-Audio the transmit side owns the clock and receive stays out of it. (#150)
- Audio: transmit packets are filled once, from the macOS output ring, and transmit no longer runs ahead of receive. The isochronous transmit queue is finite and completes on descriptor status (ring of 504), the receive replay history grew from 512 to 2048 frames, and the Saffire output safety offset is now 24 frames. (#151)
- Tests: host tests for the SCSI adapter's target-ID and per-task decisions. Contributed by Mathias Hellevang. (#144)
- DICE: an Alesis MultiMix that reports more than one playback stream is now published and streams every one of them, as Alesis's own driver does. It used to be refused, following libffado, which assumes the second stream is not real. The one MultiMix we have a register dump of reports a single playback stream and is unaffected. Hardware confirmation is pending.
- DICE: the sample rates offered to macOS now come from the device's own list of supported rates, as Focusrite's and TC Applied Technologies' drivers do, instead of a fixed 44.1 and 48 kHz. A device that also supports 32 kHz now offers it, and a rate the device does not list is refused before anything is sent to it. Rates above 48 kHz (for example 88.2 and 96 kHz on the Saffire Pro 24 DSP) are listed but not yet supported: choosing one is refused, and the device stays at its current rate. Every device still starts at 48 kHz.
- DICE (internal): the seven per-model profiles are replaced by one, with a short per-model description (name, playback encoding, two framing flags). Channel and stream counts always come from the device now, so a device whose counts differ from what the old profile expected is no longer refused. Hardware confirmation is pending.
- Audio (internal): every device family now implements one streaming interface that the session scheduler calls directly. The old callback interface and its adapter are gone, and DICE devices no longer turn each step into a callback and back. FireWire traffic is unchanged, checked against the recorded traces of every family.
- Audio: stream start, stop, rate changes and recovery for every device family now go through one per-device session scheduler that replaces the previous coordinator. Requests that overlap are combined into one restart. After three failed recoveries in a row the streams stay stopped until playback is started again; this limit now applies to DICE and MOTU devices too, which previously retried without limit (AV/C devices previously allowed four). The FireWire traffic is otherwise unchanged, checked against traces recorded from the previous code. Hardware confirmation is pending.
- Focusrite Saffire Pro 40 (original revision) playback now uses the same raw 24-in-32 PCM encoding as the other Saffire models, without AM824 labels. This matches Focusrite's own driver; the earlier contributor verification used AM824 labels, so a retest on hardware is welcome.
- DICE: bursts of device events (bus resets, configuration changes, timing faults) now cause one stream restart once the device has been quiet for 400 ms, as the vendor drivers do, instead of one restart per event. Starting, stopping and rate changes from macOS are not delayed. Other device families are unchanged. Hardware confirmation is pending.
- DICE: the driver now keeps ownership of a DICE device while it is connected, claiming it once per bus reset, instead of claiming it at every start and releasing it at every stop. This matches the vendor and Linux drivers and removes a few transactions from every start and stop. The first register read of a start is now the section table; a meaningless read before it is gone. Hardware confirmation is pending.
- DICE: stream start and stop now run as one straight sequence instead of a chain of callbacks. The FireWire traffic is unchanged: it matches, transaction for transaction, the recorded traces of the previous code on five DICE devices. Hardware confirmation is pending.

### Fixed

- M-Audio: the capture write position is now aligned to the clock macOS reads at when the transmit side owns that clock, and a stop/start can bind the receive stream again. This is the suspected cause of a ProjectMix I/O report of silence on every capture channel on 0.3.1-era builds; it has not been confirmed on a ProjectMix yet. (#163)
- OHCI: after a bus reset, the driver could stop handling interrupts entirely. Unplugging one device and plugging in another then left the new device undetected until the driver was restarted. The interrupt handler now masks the bus-reset event itself, acknowledges the Self-ID events it has read, and runs again until no enabled event is pending. Hardware confirmation of the replug case is pending. (#164)
- AV/C: a bus reset during discovery could crash the driver in two ways. The failed commands' completions ran nested inside each other until the stack overflowed (a 227-command Phase 88 attach); they now run one after another. And the reset was passed to AV/C units while the discovery lock was held, which aborted the driver on a recursive lock; each restart reset the bus again, so it looked like the device was boot-looping. The units are now notified after the lock is released. (#165)
- BeBoB: the Phase 88 sends its channels in planar order (S/PDIF left, Out 1/3/5/7, S/PDIF right, Out 2/4/6/8, MIDI), but they were streamed in channel order, so channel 1 reached the S/PDIF slot and only one side played. Discovery now asks the device for its channel positions and places playback and capture channels accordingly, falling back to channel order if the reply is malformed. The MIDI section is now recognised too. The Phase 88's master volume now starts at -35 dB on both channels. (#160)
- OHCI: isochronous context interrupt masks are read and cleared once, after the global acknowledge. A faulted transmit context is no longer treated as stopped until the controller shows it inactive. Starting a transmit context now clears its whole control register and only its own event, and the cycle-master decision is re-applied after a `cycleTooLong` interrupt. (#152, #153, #154)
- Async: Z=3 packets are now hot-appended to the transmit chain, and a transmit context that is still active is never re-armed. Contributed by Mathias Hellevang. (#147)
- SBP-2: the driver no longer terminates SBP-2 targets while it is itself stopping. Contributed by Mathias Hellevang. (#148)
- Audio: a bus reset in the middle of a restart no longer leaves the device silent, and a restart is handed to CoreAudio while it is running the streams.
- Audio: the transmit producer is quiesced on stop for every device family, and transmit packets sent as DATA are armed with valid AM824 silence rather than zeros.
- Audio: the transmit frame cursor starts at the projected frame, the transmit packet index is carried as 64 bits end to end, and the exhausted-packet counter is reset per stream.
- DICE: a device reporting more streams than TC Applied Technologies' drivers accept (more than two it sends, more than four it receives) is now refused, as those drivers refuse it; it used to be shrunk to a smaller device. A failed read of the stream registers now fails the device's setup instead of publishing whatever was read before the failure.
- DICE: the bring-up skipped the clock-select write whenever the device had already been asked for the target rate, even if it was running at another rate, so the start timed out. It now also checks the rate the device actually reached and rewrites the setting when they differ. This restores a fix that was validated on a Saffire Pro 24 DSP but never committed.
- Sample-rate changes made while no audio is playing were undone by the next start: the stream restarted at the previous rate while macOS rendered the new one, so playback came out at the wrong pitch (44.1 kHz played about 9% sharp on a 48 kHz device clock). The start now uses the rate you picked.
- Focusrite Saffire Pro 24 DSP: waits during stream start (clock change accepted, clock lock, source lock) failed immediately instead of waiting, because this model's protocol was created without a timer. It now waits like every other DICE device.
- Audio: stopping playback after a start that had been refused (for example by a bus reset during setup) could crash the driver, through an internal consistency check that is active in every build. The stop now succeeds.
- Audio: a timing fault reported just after playback stopped could restart the streams although nothing was playing. It is now ignored.
- Audio: a second start request for a device that was already streaming re-ran the whole start over the live streams. It is now recognised as already done.
- DICE: a start was refused whenever another device on the bus held isochronous channel 0 or 1, because DICE devices asked for exactly those. The bus's isochronous resource manager now picks free channels from 0–31, as Linux does, and the device is told which ones.
- DICE: when a device changed its stream configuration on its own and said so, the streams kept running with the old one. They now restart, as the vendor drivers do. The notification the device sends during our own start is ignored.
- DICE: with two DICE devices connected, one device's notifications could complete the other's clock-change wait. Each device's notifications now reach only that device.
- DICE: after a sample-rate change made while no audio was playing, the next start found the device still at the old rate and set it a second time. The rate change now waits (up to one second) until the device actually runs at the new rate.

## [0.3.1] - 2026-09-24

### Added

- Focusrite Saffire Pro 40 original revision (`0x05`) with asymmetric 12+8 playback and 10+10 capture streams. Full-duplex 20-channel audio at 48 kHz was contributor-verified on a MacBook Pro and iMac. The later TCD3070 revision is not enabled. (#106)
- PreSonus StudioLive 24.4.2 profile and protocol wiring from a contributed register capture, including asymmetric playback geometry. No streaming result is recorded yet. (#122, #124)
- MOTU protocol-v2 audio support for the 828mkII and UltraLite: vendor-register control, per-block source-packet-header timing, PCM channel mapping, and the UltraLite's stream quirks. Both are enabled in-tree but need audio verification on hardware. (#121)
- Mackie Onyx 400F Echo Fireworks audio path and Onyx-i Oxford-run AV/C audio path. The Onyx-i identity and format were captured from an 820i; neither family has an audio-verified report yet. (#107)
- M-Audio FireWire 1814 and ProjectMix I/O special-firmware audio path, fixed at 48 kHz. It includes guarded 1814 bootloader startup, safe AV/C commands, duplex streaming, and device-specific channel mapping. The 1814 playback, capture, and cold start were maintainer-verified; ProjectMix I/O worked on an earlier development build and still needs a retest of this release. (#143)
- Developer MCP diagnostics for the M-Audio Virtual UART shell and the running driver's version. (#143)

### Changed

- Consolidated device identity, safe probing, support status, protocol selection, and stream traits in the audio device catalog. Audio backends and publication now consume the resolved decision instead of independently rematching device IDs. (#143)
- Consolidated audio duplex session ownership, start/stop lifecycle, and backend interfaces; tightened publication gates and completion handling under concurrent start and teardown. (#130)
- Resolve and validate geometry separately for each playback and capture stream. Nub publication and playback framing use the same resolved geometry, and a live endpoint rejects incompatible geometry changes instead of silently changing channel counts. (#129, #131)
- Moved content framing and payload encoding behind the audio wire-codec seam, with explicit presentation plans and transport descriptors. MOTU now uses that seam and its own device timing policy. (#132)
- Expanded host fixtures for DICE and M-Audio devices, added a captured 1814 happy-path choreography, and ran host tests under ASan/UBSan with DriverKit stub lifetime checks. (#123, #126, #128, #143)

### Fixed

- FireWire IRM contention and cycle-master behavior on small buses; bandwidth accounting, channel release, and operational link-speed selection now follow verified bus behavior. A device advertising S400 but failing to acknowledge at that speed can use the observed working speed. The Midas Venice F24 still needs a hardware recheck. (#117, #118, #133, #135, #137)
- OHCI isochronous transmit ring-lap recovery, transmit fault locking, async receive ring-straddle recycling, startup watchdog arming, and bounded AT/AR quiescence during reset. (#102, #111, #136, #140)
- SBP-2/SCSI teardown, reconnect, cancellation, timeout, and target-lifecycle races, including use-after-free and data-buffer/page-table leaks. Added a host test rig for the real target bridge and session stack. (#112, #134, #141, #142)
- AV/C pending-command completion and FCP test teardown, plus Echo Fireworks response-window and transport-lifetime handling. (#109, #127)
- MOTU UltraLite transmit and receive timing, source-packet-header placement, channel-to-port order, no-data handling, and device naming. (#121, #132)
- DICE stream-count aggregation and invalid-geometry publication paths; contributed fixtures pin the expected geometry for Venice, Saffire, and StudioLive devices. (#129, #131)
- DriverKit build architecture selection on Apple Silicon so the dext is built as `arm64e`. (#108)

## [0.3.0] - 2026-08-08

See the [v0.3.0 release notes](https://github.com/mrmidi/ASFireWire/releases/tag/v0.3.0).

<!--
Use these headings, omitting any that are empty:

### Added
### Changed
### Deprecated
### Removed
### Fixed
### Security
-->

[Unreleased]: https://github.com/mrmidi/ASFireWire/commits/main
