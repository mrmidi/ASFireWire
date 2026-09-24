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

## [0.3.1] - 2026-09-24

### Added

- Experimental Focusrite Saffire Pro 40 model `0x05` support with asymmetric 12+8 playback and 10+10 capture streams at 44.1/48 kHz. Full-duplex 20-channel playback and capture at 48 kHz were verified on a MacBook Pro and iMac.
- M-Audio FireWire 1814 and ProjectMix I/O special-firmware audio path at 48 kHz, including guarded bootloader startup, duplex streaming, and a developer Virtual UART console. The 1814 path was verified on hardware; ProjectMix I/O was tested on an earlier development build and should be retested against this release.

### Changed

- Consolidated audio identity, safety, protocol, and profile selection into a resolved device policy, with host tests for device bring-up and M-Audio streaming choreography.

### Fixed

- Clamp FireWire operating speed to observed link behavior when a device advertises a faster speed it cannot acknowledge. The Midas Venice F24 still needs a hardware recheck after this fix.
- Hardened SBP-2/SCSI target teardown, reconnect, and timeout handling, and bounded async context quiescence during bus reset.

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
