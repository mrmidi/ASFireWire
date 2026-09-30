# AV/C Report

The **AV/C Report** sidebar page exports discovery evidence so device owners can
inspect it and attach it to investigations. It includes every discovered node
with a standard AV/C Config ROM unit (specifier `0x00A02D`, version `0x010001`)
or an existing AV/C inventory, independent of vendor/model support.

Connect the app to the driver, then select **AV/C Report → Refresh**. The report
contains identity and route information, Config ROM units and raw exported bytes,
AV/C unit/subunit plug counts, decoded Music capabilities and channel names,
raw capability wire blobs, and cached Music/Audio descriptors.

- **Copy Report** copies readable text with offset-labelled hexadecimal bytes.
- **Save Text Report** writes the same text to a `.txt` file.
- **Save JSON Dump** preserves structured metadata and raw bytes in a versioned
  `.json` file. Attach this file when an investigator needs the original bytes.
- **Save Binary Dump Folder** writes `report.txt`, `manifest.json`, and separate
  `.bin` files containing the exact ROM, descriptor and capability bytes for mocks
  and tests. The readable report presents parsed data first, then a raw appendix.
- **Open Dump** loads a saved JSON dump for offline inspection, without a driver
  connection. Import errors retain the previously displayed report.

## Evidence scope

Refresh explicitly requests the driver's normal AV/C discovery probes and waits
for a terminal completion status. This is a manual live operation, not an
automatic poll. It reuses the driver's existing command filters and device probe
policy; restricted device paths are marked skipped rather than receiving an
unsafe generic command sequence. Completed, failed, skipped and timed-out
captures are identified in each device's notes. It does not select stream formats
or republish audio devices.

The timestamp is capture time. The app checks device routing before and after
capture and retains the previous report if a node or generation changes. An
older driver without completion status must be updated before a live capture.

An AV/C-capable node with no cached subunit data still appears, with its missing
inventory marked explicitly. Missing data does not prove the hardware rejects a
command. A zero-length blob is reported separately from an unavailable blob.

Capability bytes are ASFW's user-client serialization, **not** raw AV/C command
responses. Descriptor bytes are the cached device descriptor content. Config ROM
bytes are the existing Config ROM export; a mismatched cache generation is omitted.
The current user-client API bounds each raw blob to 4096 bytes. Larger or absent
descriptors are marked unavailable rather than silently truncated.

JSON schema version 1 uses base64 for `Data` fields (`configROM`, `capabilities`,
`descriptor`). The text report renders those same bytes as hex. GUIDs are unsigned
64-bit integers; preserve integer precision when processing the JSON externally.
Imports are limited to 8 MiB and reject unsupported schema versions or oversized
blob collections. Imported bytes are displayed, never sent to hardware.

Manual capture lifecycle logs can be inspected with:

```sh
/usr/bin/log show --last 10m --info --debug --style compact \
  --predicate 'eventMessage CONTAINS "[AVCDiag]"'
```

The binary folder also includes `snapshot.json` for **Open Dump**. The manifest
maps each `.bin` file to its device GUID and subunit. Capability files use ASFW's
user-client ABI and descriptor files contain descriptor payloads; neither is a
recording of complete FCP request/response packets.
