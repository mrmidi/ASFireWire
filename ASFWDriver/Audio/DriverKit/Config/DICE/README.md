# DICE ADK Configuration Subsystem

This directory contains the device configuration and profile matching subsystem for TCAT DICE-based audio interfaces. In accordance with strict separation of concerns, the core FireWire driver only publishes raw hardware identifiers (vendor ID, model ID, GUID) and has no knowledge of audio protocols. The Audio Dext (ADK) owns all device configuration logic, split across two distinct folders:

## Subsystem Architecture

```
Config/DICE/
├── DiceDeviceProfile.hpp          # IDiceDeviceProfile interface
├── DiceProfileRegistry.hpp/.cpp   # Registry database & matcher
├── README.md                      # This file
│
├── Isoch/                         # Isoch Stream Configurations
│   ├── DiceStreamConfig.hpp/.cpp  # Stream layouts, DBS, block sizes
│   └── Profiles/                  # Vendor/device stream profiles
│       ├── FocusriteSaffireProfile.hpp/.cpp
│       └── GenericDiceProfile.hpp/.cpp
│
└── Async/                         # Async Register & Control Configuration
    ├── DiceQuirks.hpp             # Register map & format overrides
    └── Controls/                  # [TODO] Control panel & fader mappings
```

## Division of Concerns

### 1. Isoch (Isochronous Streaming)
The `Isoch/` directory defines the stream geometry of the device (PCM channels, MIDI slots, Data Block Size (DBS), blocking cadence). 
* **FocusriteSaffireProfile**: Configures 8 PCM + 1 MIDI for playback (DBS 9) and 16 PCM + 1 MIDI for capture (DBS 17).
* **GenericDiceProfile**: Serves as a 2-channel stereo fallback for conformant DICE devices.

These stream topologies directly drive graph creation (`BuildAudioGraph`), shared DMA memory allocations, and packet timeline pacing.

### 2. Async (Asynchronous Controls & Configuration)
The `Async/` directory defines control registers, fader maps, volume bounds, and routing capabilities.
* **DiceQuirks**: Specifies format quirks such as `PcmSlotEncoding::RawSigned24In32BE` for Focusrite playback (sign-extended big-endian 24-in-32 slots).
* **Controls**: (Future Work) Maps CoreAudio control requests (phantom power, clock source, phase invert) to asynchronous block write transactions.
