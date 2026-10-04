# Duet cached discovery and dashboard findings (2026-10-03)

Read through the ASFW MCP control plane without sending device commands. Running dext:
`6253beea2f738035828588f9205026199b0812aa`, clean, `feat/avc-ccm`, built
2026-10-03T15:35:59Z. GUID `0x0003DB0A0000D112`, node 1, generation 2, discovery session 1.
Raw document: [fixture](fixtures/duet_discovery_6253bee_2026-10-03.json).

## Music identifier

Exchanges 89–91: OPEN `0060080001ff0000` → `0960080001ff0000`;
READ `00600900ff00008000000000` →
`096009001200002600000000009d012201ce000a810000060101ffffffff01c08108000403030005002e810900080090`;
CLOSE `0060080000ff0000` → `0960080000ff0000`.

The descriptor snapshot records `malformedOperands`, zero bytes retained. The READ reply is not
NOT IMPLEMENTED, but does not establish a valid Music identifier. OPEN/CLOSE succeeded and attach
continued. Do not infer capabilities from this inconsistent payload. The 464-byte Music **status**
descriptor and 56-byte Audio identifier were read successfully.

## SIGNAL SOURCE specific inquiries

All five replies are `08` (NOT IMPLEMENTED), echoing the `0F` request:

| Source → sync destination | Command | Response |
|---|---|---|
| iPCR 0 → Music 2 | `02ff1a0fff006002` | `08ff1a0fff006002` |
| External 0 → Music 2 | `02ff1a0fff806002` | `08ff1a0fff806002` |
| Music 0 → Music 2 | `02ff1a0f60006002` | `08ff1a0f60006002` |
| Music 1 → Music 2 | `02ff1a0f60016002` | `08ff1a0f60016002` |
| Music 2 → Music 2 | `02ff1a0f60026002` | `08ff1a0f60026002` |

These are actual replies to the corrected form, not simulated fixtures. The earlier
`documentation/fixtures/AVC/duet.json` also contains both `FF` and `0F` queries for
Music 2 → Music 2, both NOT IMPLEMENTED. That same edge still matches. Its other paired queries
target Music 0/1, so they are not direct comparisons for the new sync-destination probes. No CONTROL was sent by this inspection.

## Feature bitmap and confirmed controls

The 56-byte Audio identifier is byte-identical to `fixtures/duet_descriptors.json`'s Audio identifier.
It still carries master `0003` and channel `0002` bitmaps in its short-field layout.
This does not establish a different global bitmap order: see `fixtures/duet_descriptors.md` and
`open-items.md` item 2. The live cached STATUS results confirm block 1, channels 0/1/2,
control 1 = `70`, control 2 = `0000`, all without errors. The newer serializer will name and decode
these confirmed controls independently of the descriptor bitmap.

## Clock, routes and rate

No selectors, confirmed clock routes or graph clock sources are recorded. This does not prove the
absence of vendor clock controls. SIGNAL SOURCE STATUS replies record:
Music 0 → oPCR 0; Music 1 → Audio 0; iPCR 0 → Music 0;
Audio 1 → Music 1; Music 2 → Music 2. Operand 0 is `70` throughout.
These are discovery-time routes, not newly sampled current routing.

The cached playback and capture graph rates are both 44100. The driver-owned stream health,
read during this inspection, reports the same GUID streaming at 48000, two input and two output
channels, verdict `receivingData`. Successful Duet and BeBoB clock changes now publish the confirmed duplex rate into a new immutable
AVCUnit graph. Stale routes and zero rates are refused. The document reads that graph; Refresh obtains
the latest confirmed rate even when audio is stopped. Original plug probe formats remain labelled
as captured at discovery. The telemetry workaround was removed so graph consumers share one rate.

## Older document handling

The running dext predates `0bb6ab36`'s parsed contents and names. The app now identifies the missing
extension, explains the driver update needed, retains raw probe address/opcode identities and
subunit types, and distinguishes absent parsed fields from unsuccessful descriptor reads.
Descriptor identity includes its raw specifier so Music status and identifier rows do not collide.
Installing a current driver is still required to obtain parsed capability/control/plug data.
