# Retired DICE DriverKit configuration path

The former DriverKit-side DICE profile registry was removed by the runtime
endpoint normalization. This directory is retained only as a historical path
marker and must not regain source files.

Current ownership is:

- `DeviceProfiles/Audio/AudioDeviceCatalog.*` — declarative identity and safety;
- `Audio/Families/DICE/` — DICE profile building and provider policy;
- `Audio/Devices/` — per-unit resolution and immutable endpoint sessions;
- `Audio/Duplex/` — identity-free stream planning; and
- `Audio/DriverKit/Config/AudioDriverConfig.*` — validation of the copied,
  versioned endpoint-profile property.

The AudioDriverKit service never looks up a device by GUID, vendor, or model.
It consumes the serialized endpoint snapshot published on its nub.
