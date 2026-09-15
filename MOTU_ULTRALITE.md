# Original MOTU UltraLite support

Copyright 2026 Rafal Zalech. Licensed under the Apache License 2.0 together
with the ASFireWire project. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

This branch adds audio support for the original FireWire-only MOTU UltraLite
(protocol v2) to ASFireWire. It was developed from public protocol
documentation, the MOTU UltraLite user manual, and independently observed
device and wire behavior. Linux FireWire audio sources and Apple's legacy
driver interfaces were consulted only as behavioral references; no GPL- or
APSL-licensed implementation code was copied.

## Current scope

- Exact original UltraLite discovery and protocol-v2 register setup
- 48 kHz, 14-channel Core Audio input and output endpoint
- MOTU data-block encoding, decoding, source-packet-header timing, and stream
  startup
- Logical output routing:
  - 1-2: Main Out 1-2
  - 3-10: analog outputs 1-8
  - 11-12: S/PDIF
  - 13-14: Phones
- Logical input routing:
  - 1-8: analog inputs
  - 9-10: S/PDIF
  - 11-12: CueMix Mix1 return
  - 13-14: transport padding, not exposed as physical inputs

MIDI and additional sample rates are outside the current scope.

## Validation

The implementation was exercised with an original MOTU UltraLite connected
through the Apple Thunderbolt-to-FireWire adapter chain on Apple silicon and
macOS Tahoe. Playback and capture routing were checked channel by channel in
Max/MSP. The ASFireWire C++ test suite also covers the MOTU register plane,
profile matching, packet/block codec, timing, and transport adapter behavior.

Behavioral references:

- MOTU UltraLite manual:
  https://cdn-data.motu.com/manuals/firewire-usb-audio/UltraLite_Manual_Mac.pdf
- Linux MOTU protocol-v2 description:
  https://github.com/torvalds/linux/blob/master/sound/firewire/motu/motu-protocol-v2.c
- Linux MOTU AMDTP format description:
  https://github.com/torvalds/linux/blob/master/sound/firewire/motu/amdtp-motu.c
