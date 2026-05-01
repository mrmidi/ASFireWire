# FireWire Audio Device Metadata

ASFireWire keeps a generated metadata library for FireWire audio/music
devices. The library is used for identification, logging, and read-only UI
diagnostics.

Recognition is not support. Most imported records are metadata-only and must
not publish CoreAudio devices or start protocol handlers until ASFireWire has a
tested binding path for that protocol/device.

## Sources

- FFADO device configuration: https://raw.githubusercontent.com/adiknoth/ffado/master/libffado/configuration
- systemd IEEE1394 hwdb: https://raw.githubusercontent.com/systemd/systemd/main/hwdb.d/80-ieee1394-unit-function.hwdb
- Takashi Sakamoto config-ROM collection reference: https://raw.githubusercontent.com/takaswie/am-config-roms/main/README.rst

The generator is `tools/generate_firewire_audio_profiles.py`. It produces:

- `ASFWDriver/Protocols/Audio/FireWireAudioDeviceProfiles.hpp`
- `ASFW/Models/FireWireDeviceProfiles.generated.swift`

## Support Policy

- `SupportedBinding` means current ASFireWire code intentionally binds this
  profile to an audio/control path.
- `Deferred` means the identity is known, but binding is intentionally disabled
  until the driver supports the required stream geometry or control model.
- `MetadataOnly` means the profile is diagnostic information only.

New FFADO/systemd records default to `MetadataOnly`. Do not promote a device to
`SupportedBinding` because a public table says it is DICE, BeBoB, FireWorks, or
another known family. Runtime stream geometry must be discovered safely or added
from a tested ASFireWire profile.

## Current Binding Defaults

- Alesis MultiMix FireWire remains bound through the tested generic DICE path
  and known fallback profile.
- Focusrite Saffire Pro 14/24/24 DSP remain bound through the current DICE
  paths; larger/multi-stream Focusrite profiles remain deferred.
- Apogee Duet remains AV/C-driven.
- Midas Venice F32 is recognized exactly as `0x10c73f / 0x000001` and uses the
  current generic DICE runtime-probe path, with no hardcoded channel fallback.
