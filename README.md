# ASFireWire

[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=mrmidi_ASFW&metric=alert_status&token=3ca1b3d10414117bb3e75b1779090b4ea47f1585)](https://sonarcloud.io/summary/new_code?id=mrmidi_ASFW) [![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/mrmidi/ASFireWire)

ASFireWire restores FireWire (IEEE 1394) functionality removed from macOS Tahoe (26).
It rebuilds the stack with DriverKit and PCIDriverKit, with AudioDriverKit for audio
and SCSIControllerDriverKit for SBP-2 devices.

I started it to keep useful FireWire audio hardware running on modern Macs.
Contributors have since helped extend it to DV cameras and film scanners.

## Download and installation

**[Download ASFW and read the installation guide →](https://www.asfw.net/install/)**

The website provides signed and notarized builds, installation requirements,
upgrade instructions, and release notes.
Its tester guide covers Apple silicon Macs running macOS 26 or later.

Local development builds and older unsigned GitHub packages use a different signing
and installation path. Do not apply their SIP-disabled instructions to the hosted
signed build. See [Installing](https://github.com/mrmidi/ASFireWire/wiki/Installing)
and [Building and Signing](https://github.com/mrmidi/ASFireWire/wiki/Building-and-Signing).

Intel results are recorded separately in the compatibility wiki; the hosted tester
guide is not a claim that every Intel Mac or patched macOS installation works.

## Support development

For well over a year, ASFireWire has effectively been my full-time job — just without
the salary.

The project is free and open source, but the work behind it isn't free. Financial
contributions help cover my everyday living expenses and development tools, making
it possible for me to continue developing and maintaining ASFireWire.

ASFireWire is also a comprehensive real-world implementation built on Apple's
DriverKit frameworks. Its public source brings together controller initialization,
DMA, interrupts, device discovery, protocol handling, audio, and SCSI integration
in one stack, providing a practical reference for low-level driver development on
modern macOS. [Read more about the work](https://www.asfw.net/support/).

If ASFireWire has helped you keep your hardware running, or you believe this kind of
independent engineering work deserves to continue, please consider supporting it.

**[GitHub Sponsors](https://github.com/sponsors/mrmidi) ·
[Buy Me a Coffee](https://buymeacoffee.com/mistermidi)**

## Current capabilities

- FireWire controller bring-up, bus management, topology, and device discovery.
- Asynchronous transactions and isochronous communication.
- Audio streaming through AV/C, DICE/TCAT, MOTU, and RME device protocols.
- DV capture to `.dv` files and AV/C tape transport control.
- SBP-2/SCSI integration for storage devices and scanners.

Contributors have verified DV capture from various MiniDV camcorders and scanning
with Nikon Coolscan 4000 and 9000 devices. These are results for particular devices,
not guarantees for an entire protocol family.

See [Architecture Overview](https://github.com/mrmidi/ASFireWire/wiki/Architecture-Overview)
for how the stack fits together and
[Device Compatibility](https://github.com/mrmidi/ASFireWire/wiki/Device-Compatibility)
for tested behavior, limitations, evidence, and contributor credits.

## Hardware compatibility

**[Check your device and host →](https://github.com/mrmidi/ASFireWire/wiki/Device-Compatibility)**

Support varies by model, chipset revision, firmware, and implementation maturity.
The wiki distinguishes maintainer-tested and contributor-tested results, enabled
implementations without hardware validation, and recognized but unsupported devices.
Reports identify the tested build where available and preserve known regressions.

Generic AV/C or DICE discovery does not guarantee working audio. A device appearing
in the app, or telemetry showing packets, does not establish playback or capture.

## Testing and contributing

Hardware tests, reproducible failures, diagnostic traces, and regression reports are
particularly valuable. A well-documented failed test can help as much as a success.

- [Hardware reports and compatibility](https://github.com/mrmidi/ASFireWire/wiki/Device-Compatibility).
- [Reporting Issues](https://github.com/mrmidi/ASFireWire/wiki/Reporting-Issues).
- [Exporting and collecting logs](https://github.com/mrmidi/ASFireWire/wiki/Logging).
- [Contribution policy](CONTRIBUTING.md).

Include your device, Mac, Intel/Apple silicon architecture, macOS version/build,
ASFW version, actual loaded driver build, connection chain, and reproduction steps.
Attach relevant logs and reports when available; explain when they cannot be collected.
Descriptions provide context, but logs and other evidence usually make a failure
possible to investigate.

Discuss substantial code changes in an issue before implementing them. Focused fixes,
simplification, and maintaining compatibility take priority over adding more code.
Contributors remain responsible for understanding and validating AI-assisted work.

## Documentation

| Topic | Resource |
| --- | --- |
| Public documentation | [Wiki home](https://github.com/mrmidi/ASFireWire/wiki) |
| Installation and downloads | [Website guide](https://www.asfw.net/install/) |
| Architecture and layer boundaries | [Architecture Overview](https://github.com/mrmidi/ASFireWire/wiki/Architecture-Overview) |
| Protocol background and historical references | [FireWire Fundamentals](https://github.com/mrmidi/ASFireWire/wiki/FireWire-Fundamentals) |
| Local builds, signing, and tests | [Building and Signing](https://github.com/mrmidi/ASFireWire/wiki/Building-and-Signing) |
| Hardware results | [Device Compatibility](https://github.com/mrmidi/ASFireWire/wiki/Device-Compatibility) |
| Troubleshooting | [Troubleshooting](https://github.com/mrmidi/ASFireWire/wiki/Troubleshooting) |
| Developer onboarding and optional MCP diagnostics | [Development](https://github.com/mrmidi/ASFireWire/wiki/Development) |

Implementation contracts live alongside source. `documentation/fixtures/` retains
captured hardware reports and test evidence; public technical guides live in the wiki.
[CHANGELOG.md](CHANGELOG.md) records changes associated with releases.

## License

ASFireWire is licensed under the [Apache License, Version 2.0](LICENSE).
Contributions use the same terms (inbound = outbound, per section 5).

Linux, Apple, libffado, and other reference stacks are consulted as read-only
behavioral sources. They are not distributed with this repository, and their code
must not be copied into the driver. See [NOTICE](NOTICE) for attribution.

Hardware testers and code contributors are credited in the compatibility wiki and
release history. Both make this work possible.

## Contact and professional opportunities

- [Discord community](https://discord.gg/c82rmSEEPY).
- Email: [me@mrmidi.net](mailto:me@mrmidi.net).
- [LinkedIn](https://www.linkedin.com/in/mrmidi/).

I'm open to employment or contract opportunities in systems programming, driver
and low-level C/C++ development, embedded systems, and real-time audio/video software.
This project is a public example of my engineering work.
