# AV/C Units screen

The app's "AV/C Units" section shows each unit as a dashboard: device header (name, discovery state, sample rate and
supported rates, stream channels, MIDI ports, audio sync, subunits), then five tabs.

| Tab | Shows |
|---|---|
| Signal | Playback and capture channels by name, MIDI ports, and the selectors with the input each is set to |
| Controls | Mute / volume the driver read, one card per feature block (read-only) |
| Plugs | Unit plug counts, the Music plugs (destination / source, clusters), and every plug's decoded format |
| Capabilities | The Music identifier descriptor (version, capabilities, audio formats, MIDI, sync) and the Audio function blocks |
| Diagnostics | Probes sent and failed (grouped), descriptors read, response times, spec departures; bus tests in Debug mode |

## The rule: the driver decodes, the app displays

The app contains **no AV/C knowledge**. It reads the driver's cached discovery document
(`asfw.avc.discovery`, version 1; `UserClient/WireFormats/AVCDiscoveryDocument.cpp`) and shows what it says:

- names come from the driver's spec tables ("MUTE_CONTROL", "non-blocking+blocking", "audio SYNC", subunit and opcode names);
- values come decoded (a feature control carries `on` / `db` and the spec's text; a plug format carries its rate and its
  entries by name; a selector carries its inputs and what each is);
- the Music and Audio descriptors come parsed (`snapshot.contents`): plugs and clusters, MIDI labels, activity, the
  identifier's capabilities, function blocks with their names and what feeds each.

The only things the app does itself are presentation: grouping, trimming the text every channel name shares, a level bar
scale (-60 to +12 dB), and splitting camel case in an error kind. Adding a field to the screen means adding it to the
document first. All additions to the document are additive; the version stays 1, and an older driver's document still
decodes (the screen shows what it has: `AvcUnitsRenderTests.aDocumentFromAnOlderDriverStillDecodes`).

## Data flow

`AvcUnitsStore.reload()` reads the unit list, the device names and each unit's document from the driver. It sends
nothing to a device. "Re-scan" asks the driver to run discovery again (behind the existing confirmation) and reloads.

## Looking at it without the app

`AvcUnitsRenderTests.everyTabRenders` renders each tab (dark mode, real Phase 88 document from
`tests/golden/avc/phase88__discovery_document.json`) to a PNG. Set `TEST_RUNNER_ASFW_RENDER_DIR=<dir>` to keep them.
The simulated Phase 88 names its capture channels "Input N" (the sim does not answer the signal-source probes that select
the capture plug); a live device gives "Line_1/2 left" and so on.
