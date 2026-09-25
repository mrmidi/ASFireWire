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

### Fixed

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

### Changed

- DICE: the sample rates offered to macOS now come from the device's own list of supported rates, as Focusrite's and TC Applied Technologies' drivers do, instead of a fixed 44.1 and 48 kHz. A device that also supports 32 kHz now offers it, and a rate the device does not list is refused before anything is sent to it. Rates above 48 kHz (for example 88.2 and 96 kHz on the Saffire Pro 24 DSP) are listed but not yet supported: choosing one is refused, and the device stays at its current rate. Every device still starts at 48 kHz.
- DICE (internal): the seven per-model profiles are replaced by one, with a short per-model description (name, playback encoding, two framing flags). Channel and stream counts always come from the device now, so a device whose counts differ from what the old profile expected is no longer refused. The Alesis MultiMix keeps its single playback stream, as in libffado. Hardware confirmation is pending.
- Audio (internal): every device family now implements one streaming interface that the session scheduler calls directly. The old callback interface and its adapter are gone, and DICE devices no longer turn each step into a callback and back. FireWire traffic is unchanged, checked against the recorded traces of every family.
- Audio: stream start, stop, rate changes and recovery for every device family now go through one per-device session scheduler that replaces the previous coordinator. Requests that overlap are combined into one restart. After three failed recoveries in a row the streams stay stopped until playback is started again; this limit now applies to DICE and MOTU devices too, which previously retried without limit (AV/C devices previously allowed four). The FireWire traffic is otherwise unchanged, checked against traces recorded from the previous code. Hardware confirmation is pending.
- Focusrite Saffire Pro 40 (original revision) playback now uses the same raw 24-in-32 PCM encoding as the other Saffire models, without AM824 labels. This matches Focusrite's own driver; the earlier contributor verification used AM824 labels, so a retest on hardware is welcome.
- DICE: bursts of device events (bus resets, configuration changes, timing faults) now cause one stream restart once the device has been quiet for 400 ms, as the vendor drivers do, instead of one restart per event. Starting, stopping and rate changes from macOS are not delayed. Other device families are unchanged. Hardware confirmation is pending.
- DICE: the driver now keeps ownership of a DICE device while it is connected, claiming it once per bus reset, instead of claiming it at every start and releasing it at every stop. This matches the vendor and Linux drivers and removes a few transactions from every start and stop. The first register read of a start is now the section table; a meaningless read before it is gone. Hardware confirmation is pending.
- DICE: stream start and stop now run as one straight sequence instead of a chain of callbacks. The FireWire traffic is unchanged: it matches, transaction for transaction, the recorded traces of the previous code on five DICE devices. Hardware confirmation is pending.

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
