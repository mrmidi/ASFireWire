# DICE ADK Configuration Subsystem

The audio-side description of TCAT DICE devices. A DICE device reports its own
stream geometry, rates, clock and channel names from its registers
(`documentation/DICE_TCAT_ARCHITECTURE.md` §4.1), so what lives here is only what
the device cannot report.

## Layout

```
Config/DICE/
├── DiceDeviceProfile.hpp   # IDiceDeviceProfile (also used by the AV/C profiles)
├── DiceProfile.hpp/.cpp    # The one DICE profile, built from a DiceProfileSpec
├── README.md               # This file
├── Isoch/
│   └── DiceStreamConfig.hpp  # Stream config type and direction
└── Async/
    └── DiceQuirks.hpp        # TX/RX format quirks
```

## One profile, one spec per builder

`AudioProfileRegistry` holds one `DiceProfile` per catalog builder, plus the
generic fallback. Each is a `DiceProfileSpec`:

* **name**, and range-member names picked by capture width (Midas Venice
  F16/F24/F32 share one identity);
* **TX encoding**: raw 24-in-32 (every TCAT kext) or AM824 (Weiss, generic);
* **preserveFdfInNoDataPackets** and **initializeNonAudioSlots** (off for the
  Alesis MultiMix, which carries no MIDI slot);
* **assertedPlaybackStreams**: set only for the Alesis MultiMix, whose register
  overstates its playback streams (libffado `dice_avdevice.cpp:1686-1700`).

A profile states no channel or stream counts. Its default stream configs carry
framing constants only (8 frames per packet, FDF 0x02, FMT 0x10, blocking); the
device's resolved geometry fills in the rest (`DiceAudioBackend` resolves it,
`BuildResolvedTxStreamConfig` frames from it). Safety offsets and reported
latency follow the packet-scaled ladder in `Shared/Isoch/AudioGeometryPolicy.hpp`.
