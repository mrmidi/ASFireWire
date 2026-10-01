# AV/C Report

The **AV/C Report** sidebar page exports discovery evidence so device owners can
inspect it and attach it to investigations. It includes every discovered node
with a standard AV/C Config ROM unit (specifier `0x00A02D`, version `0x010001`)
or an existing AV/C inventory, independent of vendor/model support.

Connect the app to the driver, then select **AV/C Report → Refresh**. The core
of the report is each unit's **FCP exchange log**: every AV/C command the driver
sent the unit and the reply, since attach or the last refresh. That is the
complete discovery, whichever bring-up the device uses (generic AV/C, BeBoB plug
probes, vendor commands), so a shared report lets anyone rebuild the device's
capability graph offline. The report also contains identity and route
information, Config ROM units and bytes, AV/C plug counts, decoded Music
capabilities, and cached Music/Audio descriptors.

- **Copy Report** copies readable text with offset-labelled hexadecimal bytes.
- **Save Text Report** writes the same text to a `.txt` file.
- **Save JSON Dump** preserves structured metadata and raw bytes in a versioned
  `.json` file. Attach this file when an investigator needs the original bytes.
- **Save Binary Dump Folder** writes `report.txt`, `manifest.json`, separate
  `.bin` files containing the exact ROM, descriptor and capability bytes, and a
  `<GUID>-fcp-exchanges.json` per device in the `tools/avc/avc_discover.py` dump
  format. The readable report presents parsed data first, then a raw appendix
  with one line pair per exchange (`>` command, `<` reply).

Rebuild the capability graph from a report without hardware:

```sh
python3 tools/avc/avc_discover.py --replay <GUID>-fcp-exchanges.json
python3 tools/avc/avc_discover.py --replay snapshot.json --guid 0x...   # several devices
```

Replay matches commands with the driver's quadlet padding ignored and rebuilds
each descriptor from the READ DESCRIPTOR replies, so the driver's read chunking
does not matter.
- **Open Dump** loads a saved JSON dump for offline inspection, without a driver
  connection. Import errors retain the previously displayed report.

## Evidence scope

Refresh starts a new exchange session on each unit and reruns the discovery
attach runs: everything the unit answers. That is the generic AV/C discovery
(UNIT INFO, SUBUNIT INFO, plugs, music and audio subunit descriptors, SIGNAL
SOURCE, the current formats of unit plug 0 when the descriptors give none), then
the read-only inventory of an identified chip's extensions: BridgeCo plug info,
format lists, channel positions, sections and signal formats for BeBoB units;
the stream-format lists in both directions for Oxford units. Only a device whose
probe policy forbids discovery traffic is sent nothing (the M-Audio special
firmware, which freezes on unproven frames; Fireworks, whose unit is not AV/C);
its log keeps everything the driver has sent it since attach, and the report
says its plugs were not read. Completed, failed, skipped and timed-out captures
are identified in each device's notes. Refresh does not select stream formats
or republish audio devices. The decision of what each unit gets is
`Protocols/AVC/AvcProbeAdmission.hpp`; the extension inventories are
`Protocols/AVC/AvcExtensionInventory.cpp`.

A refusal at a format-list index is how a device ends the list; the report
counts it as "end of list", not as an error.

Each exchange records the command bytes as sent, the reply, and how it ended:
`response`, `timeout`, `busReset`, `transportError`, `responseMismatch`, or
never sent (`refusedByFilter`, `busy`, `invalid`), plus INTERIM, retries and the
bus generation. A session keeps its first 1024 exchanges (256 KiB); later ones
are counted as dropped, never overwritten.

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

JSON schema version 2 adds `exchanges` per device, with command and reply bytes
as number arrays; version 1 dumps still open. `Data` fields (`configROM`,
`capabilities`, `descriptor`) use base64. The text report renders those same bytes as hex. GUIDs are unsigned
64-bit integers; preserve integer precision when processing the JSON externally.
Imports are limited to 8 MiB and reject unsupported schema versions or oversized
blob collections. Imported bytes are displayed, never sent to hardware.

Manual capture lifecycle logs can be inspected with:

```sh
/usr/bin/log show --last 10m --info --debug --style compact \
  --predicate 'eventMessage CONTAINS "[AVCDiag]"'
```

The binary folder also includes `snapshot.json` for **Open Dump**. The manifest
maps each file to its device GUID and subunit. Capability files use ASFW's
user-client ABI and descriptor files contain descriptor payloads; the
`-fcp-exchanges.json` files are the recording of the FCP request/response frames.
