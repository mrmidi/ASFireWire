---
name: asfw-mcp-control-plane
description: Inspect and safely operate the local ASFW FireWire MCP control plane. Use when an ASFW task needs live driver status, node discovery, protocol telemetry, MCP tool discovery, or explicitly authorized guarded FireWire control through the loopback MCP endpoint.
---

# ASFW MCP Control Plane

Use the bundled client instead of reconstructing MCP HTTP/SSE sessions or pasting large tool schemas into the conversation.

## Default workflow

Run these commands from the ASFireWire repository root.

1. Confirm the app-hosted MCP server is enabled. The default local endpoint is
   `http://127.0.0.1:8766/mcp`, which is also the client's built-in
   `DEFAULT_ENDPOINT`, so no export is needed in the normal case.

   The port is user-configurable in the app's **MCP Control Plane** settings.
   On `Connection refused`, read the port shown in that panel and set the
   endpoint explicitly rather than assuming the server is down:

   ```bash
   export ASFW_MCP_ENDPOINT=http://127.0.0.1:<port>/mcp
   ```

2. Ask the versioned health resource whether deeper reads are trustworthy:

   ```bash
   python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py health
   ```

   `ready` permits targeted read-only diagnostics. `degraded` permits only
   high-level inspection until its reasons are resolved. `unavailable` means
   do not infer driver state. Preserve `expectedGeneration` for any follow-up
   bus request.

3. Run the compact read-only summary when node or protocol detail is needed:

   ```bash
   python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py summary
   ```

4. Use `tools` or `resources` only when the summary lacks the needed detail.
5. Call a read tool with typed JSON arguments only after checking its schema:

   ```bash
   python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py tools
   python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_read_quadlet '{"nodeId":0,"generation":9,"addressHigh":65535,"addressLow":4026532864}'
   ```

Pass `--endpoint` or set `ASFW_MCP_ENDPOINT` when the server uses a non-default loopback port.

## Config-ROM explorer

`asfw_get_config_rom` is a **read-only projection of the driver's discovery
cache**. It never starts a ROM fetch and never issues a FireWire transaction.
First obtain the current `deviceInstanceId` and generation from `summary`; after
any bus reset, refresh them before asking for another view. The tool keys on the
runtime **device instance**, not the physical node id — node ids are reused
across generations and instance ids are not.

The default `summary` view is deliberately compact: cache/generation status,
GUID, vendor/model/unit identity, parser diagnostics, and two reminders that
avoid common false conclusions. Use it before requesting the more detailed
views:

```bash
# Compact normal entry point: summary is default.
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py rom 1 17

# BIB bitfields, each with bit position, decoded value, and a short meaning.
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py rom 1 17 --view bib

# Parsed IEEE 1212 directory tree (default 64 entries; raw leaf bytes omitted).
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py rom 1 17 --view tree

# Big-endian cached quadlets; bounded to 64 per response and page by index.
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py rom 1 17 --view raw --start-quadlet 0 --max-quadlets 32
```

Equivalent direct calls are useful when another MCP client does not use the
bundled script:

```bash
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_get_config_rom \
  '{"deviceInstanceId":1,"generation":17,"view":"bib"}'
```

Interpret the annotated BIB carefully:

- `IRMC`, `BMC`, `CMC`, and `ISC` are the device's Config-ROM capability
  claims. They do not establish physical root, designated IRM, or Bus Manager
  ownership.
- `generation` in the BIB is the device's four-bit ROM field; it can differ
  from the current host topology generation supplied to the tool.
- `max_rec` describes an asynchronous payload code. It is not an isochronous
  audio packet size.
- A tree leaf reported as `not fetched from partial cache` is not evidence of a
  malformed ROM. The discovery cache may contain only a prefix. Likewise, a
  cached result from another generation is useful only as stale description,
  never as a target for a follow-up transaction.

## DICE registers by name

`asfw_dice_read_register` accepts a **symbolic** `register` instead of a raw
address. It resolves against the device's own section table, so no section base
is ever assumed:

```bash
# How many PCM channels does the device's playback (host->device) stream carry?
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_dice_read_register \
  '{"nodeId":0,"generation":17,"register":"RX_NUMBER_AUDIO","streamIndex":0}'

# Clock source and rate, decoded.
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_dice_read_register \
  '{"nodeId":0,"generation":17,"register":"GLOBAL_CLOCK_SELECT","decode":true}'
```

The response carries `resolvedAddress`, `sectionOffsetBytes` and (for per-stream
registers) `streamConfigSizeBytes`, so the derivation is auditable rather than
trusted. Omitting `register` falls back to the raw-address behaviour unchanged.

**TX and RX are the device's directions, not the host's.** DICE `TX` is what the
device transmits — the host's *capture*. DICE `RX` is what the device receives —
the host's *playback*. ASFW's own profile fields use the opposite convention
(`TxChannelCount` is playback), so never transcribe one onto the other. The
offsets differ too: `TX_NUMBER_AUDIO` is at `0x0C` while `RX_NUMBER_AUDIO` is at
`0x10`, because RX inserts `SEQ_START` ahead of it.

Registers with no defined decoding return the raw value only; `decode: true`
never invents an interpretation. `asfw_dice_decode_status` decodes a value you
already hold, without touching the bus:

```bash
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_dice_decode_status \
  '{"register":"GLOBAL_EXTENDED_STATUS","value":64}'
```

`GLOBAL_EXTENDED_STATUS` splits into `locked` and `slipping`; `arx1..arx4` are
the device's *receive* streams, so `arx1` reflects the host-to-device transmit.

These reads are `readOnly` but **not idempotent** — each issues a real FireWire
transaction against a live generation. Do not run them during an active audio
endurance run; prefer `asfw_get_audio_stream_health`, which reads driver-held
counters and touches no bus.

## Tool visibility is not device capability

`tools` lists a filtered view, never the full catalog. Two independent filters
apply (`ASFW/MCP/ASFWMCPCore.swift`, `listTools`):

1. **Visibility tier vs runtime mode** — `always`, `readOnly`, `developerWrite`,
   and `rawDeveloper` are admitted according to the server's current mode.
2. **Protocol-hint prefilter** — evaluated *before* the tier check, against the
   union of `protocolHints` across all currently discovered nodes.

The hint prefilter is **any-of, not all-of**: a tool is listed when it declares
no hints, or when *at least one* of its declared hints is present. A tool
declaring `["bebob", "cmp"]` is therefore listed on a `cmp`-only bus.

Two consequences:

- An absent tool means "not listed for this generation's hints and mode". It is
  **not** evidence that the driver lacks the capability or that the device lacks
  the protocol. Check `ASFWMCPToolCatalog` for the defined set and ask
  `asfw_explain_capability` for the specific reason.
- Visibility does not follow the read-only/destructive gradient. Observed with
  only an Apogee Duet attached (`avc`, `cmp`): `asfw_phase88_start_48k` and
  `asfw_phase88_stop` are listed because they declare `cmp`, while every
  read-only BeBoB diagnostic (`asfw_bebob_get_unit_plug_info`,
  `asfw_bebob_get_clock_topology`, `asfw_phase88_get_clock`, …) is hidden
  because it declares `bebob` alone. The destructive lifecycle tools are
  reachable while the state queries that would justify using them are not.
  Never infer device state from which tools appear; re-list after any
  generation change.

## Ring-first diagnostic workflow

The driver-owned log ring is the primary diagnostic source. Do not begin with
`log stream` or add `ASFW_LOG` calls to interrupt/isochronous hot paths merely
to investigate an incident. Query a small, relevant slice and preserve the
returned `nextSequence` cursor for follow-up requests.

1. Run `health`, then record `expectedGeneration` from `summary`.
2. Read `asfw_log_stats` once to establish capacity, oldest/latest sequence,
   and drop count. A non-zero drop count makes an absence-of-evidence result
   weaker; report it.
3. Query one subsystem/category at a time with `maxRecords` no larger than 200
   and, where possible, `contains`. Use `nextSequence` as the exclusive cursor
   on the next query. An empty sparse page may still advance the cursor.
4. Prefer a structured MCP read for current state and the ring for chronology.
   If an advertised read-only tool returns `capabilityUnavailable`/
   `notImplemented`, report that adapter gap; do not fabricate the state or
   fall back to a mutation.

Useful incident queries:

```bash
# Was a reset requested locally, and which node's accepted Self-ID attributed it?
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_log_query '{"categories":["BusReset"],"contains":"Reset ","maxRecords":100}'

# Compare the physical topology role with Config-ROM capability claims.
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_log_query '{"categories":["ConfigROM"],"contains":"[RoleEvidence]","maxRecords":50}'

# Inspect only CMP/FCP activity while audio is stopped or running.
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_log_query '{"categories":["CMP","FCP"],"maxLevel":"debug","maxRecords":200}'
```

### Active audio-run rule

During an active audio endurance or fault-reproduction run, do **not** call
`health`, `summary`, discovery, or any control tool unless the user explicitly
requests it.  They can perturb the app/control plane while the issue is being
timed.  A small, targeted driver-ring query is permitted only when requested;
it is read-only and never changes stream state.  Do not run broad or parallel
queries while audio is playing.

For a live TX-content incident, use the retained `DirectAudio` fault record:

```bash
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_log_query \
  '{"categories":["DirectAudio"],"contains":"[TxContent]","maxLevel":"debug","maxRecords":20}'
```

Each record names the failing stage directly, for example
`reason=not-yet-written-at-deadline`, and carries `packet`, `frame`,
`staged=[oldest,writtenEnd)`, `required`, `completion` and `committed`. Read
`frame` against `staged`: because the range is half-open, `frame == staged.end`
means the writer had committed nothing for that packet, a zero-frame shortfall
rather than a small deficit.

Pair it with the `[TxPrep]` heartbeat, which carries two independent margins:

- `margin` / `iMin` / `iMax` / `min` — our headroom against the transmit
  deadline. These stay healthy through a producer-side xrun and cannot predict
  one.
- `prodMin` / `prodRunMin` — frames the CoreAudio writer has staged beyond the
  next frame packetization will consume, as an interval minimum and a
  since-start watermark. This is the term that reaches zero when the host
  callback is late. `UINT32_MAX` means no packet was prepared in the interval.

A `[TxContent]` deadline fault with healthy `margin` but collapsing `prodMin` is
producer starvation, not a transmit-timing problem. `late1500` in the same line
settles whether preparation itself ever ran late.

There is no `[PayloadWriter]` record. That pipeline lost its producer in
`31ef0286` and was removed; `LogCategory` 21 is a retired, frozen slot. Do not
query for it.

### Stream will not start: attribute it before theorising

When a device enumerates but audio never flows, ask
`asfw_get_audio_stream_health` **first**. It is a read-only projection of
driver-held counters — no FireWire transaction, safe during an active run — and
it separates three failures that otherwise present identically as silence.

```bash
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_get_audio_stream_health '{}'
```

Each endpoint carries a `verdict` and the raw `counters` behind it:

| `verdict` | Means |
|---|---|
| `noPacketsReceived` | No packet reached the audio consumer. The IR context is not delivering — wrong iso channel, context never started, or the device stream was never enabled. Not a device-side fault yet. |
| `geometryMismatch` | Packets arrived whose data block shape our stream config rejects. The **profile and the device disagree** on channels/DBS. Host-side rejection. |
| `packetsRejected` | Packets arrived but failed decode (runt, undecodable CIP header, zero DBS). The device may be streaming correctly. |
| `deviceSendsOnlyNoData` | Valid CIP headers with SYT `0xFFFF` and no audio frames. |
| `dataNotAccepted` | Valid SYTs arrived but no replay entry was published. Inspect the SYT cadence detector, not the device. |
| `receivingData` | Data is arriving and being accepted. |

Read `deviceSendsOnlyNoData` precisely: it states **what the device sent**, not
what the device is waiting for. It is not evidence that the device requires host
timestamps first — the TCAT and Linux DICE stacks both withhold host SYT until
the device has already sent valid ones, so a NO-DATA stall does not by itself
imply a host-side handshake obligation. Report the counters; do not infer intent.

`packetsSeen` counts every packet the master stream decoded, so
`packetsSeen == noDataPackets` is the only sound basis for "the device is sending
only NO-DATA". A non-zero reject counter with `packetsSeen > 0` means the device
did send us something we threw away.

Pair it with the bring-up records, which are emitted for a stream that has **not**
established yet (bounded per start, so a healthy stream stays silent):

```bash
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_log_query \
  '{"categories":["DirectAudio"],"contains":"[RxReplayReset] phase=bootstrap","maxLevel":"debug","maxRecords":20}'
```

### Audio timing-loss first-fault query

For an AV/C duplex click, stall, or timing-loss recovery, query the driver ring
before theorizing about SYT phase.  The driver emits `[RxReplayReset]` exactly
when an already-established RX replay epoch is invalidated and before it calls
the recovery callback.  It is anomaly-only; a healthy stream has no matching
records.

```bash
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_log_stats '{}'
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_log_query \
  '{"afterSequence":0,"categories":["DirectAudio"],"contains":"[RxReplayReset]","maxLevel":"debug","maxRecords":20}'
```

The record's `reason` identifies the layer that failed:

- `packet-status`: CIP/payload decoding or input-buffer writing rejected a packet.
- `invalid-rx-timestamp`: descriptor timestamp could not be correlated with the drain cycle.
- `receive-cycle-gap`: ASFW observed a gap in its one-received-packet-per-cycle model.
- `syt-cadence-rejected`: a valid SYT produced an invalid cadence delta.
- `clock-anchor-rejected`: RX could not publish a host clock anchor.

Always report `droppedRecords` from `asfw_log_stats`.  A zero drop count makes
the first matching record authoritative for the local reset; it does not by
itself prove whether the original fault was device-side or host-side.

After the reset-provenance change is installed, interpret the two records as a
pair: `Reset request: origin=local ...` describes ASFW's outgoing action;
`Reset provenance: ... initiator=nodeN` is the accepted Self-ID attribution for
the resulting bus generation. Never attribute a remote reset to ASFW just
because it followed a local request.

## FireWire role evidence

Keep these four facts separate in reports and code reviews:

- **Physical root** comes from the Self-ID topology tree.
- **Designated IRM** is the highest physical-ID Self-ID node with contender and
  link-active asserted. It is an operational designation, not proof that its
  resource CSRs have been successfully used.
- **BIB capabilities** (`IRMC`, `BMC`, `CMC`, `ISC`) are Config-ROM claims.
- **Bus Manager ownership** comes from `BUS_MANAGER_ID` election/readback, not
  from root or IRM status.

Legacy devices can disagree across these sources. The observed Apogee Duet
example is node 2: its BIB q2 is `0x20FF5003` (`IRMC=0`, `BMC=0`, `CMC=0`,
`ISC=1`), while its accepted Self-ID is link-active + contender and therefore
designates it as the IRM/root. It is **not** a Bus Manager. Preserve both facts;
do not "correct" topology from BIB flags or infer that it is fighting for BM.
For isochronous allocation, verify the designated IRM by the normal guarded
resource transaction at allocation time rather than rejecting it from BIB alone.

## Config-ROM cache rule

The discovery cache can contain only a fetched prefix of a Config ROM. A leaf
target outside that cached prefix is not, by itself, a malformed-device-ROM
finding. Describe it as `not fetched from partial cache` unless an explicit,
generation-pinned full read proves the target is outside the actual ROM.

Same-generation retry data must never replace a richer node cache with a
shorter prefix. If the UI reports a same-generation size regression, query
`ConfigROM` ring records and treat it as a cache lifecycle bug, not proof that
the device changed its ROM.

## Safety

- Treat `summary`, `tools`, `resources`, and `read` as the normal path.
- The client refuses non-allowlisted tool calls unless `--allow-mutation` is present. This flag is only a local acknowledgement; the MCP server's developer/mutation policy remains authoritative.
- Use `--allow-mutation` only after the user explicitly authorizes the exact hardware action. Do not infer permission from a request to inspect, diagnose, capture, or test.
- Read and preserve the current generation. A generation change means prior node IDs and writes are stale; refresh the summary before any follow-up action.
- Do not use the skill to issue raw writes or control commands merely to probe a device. Prefer the MCP hardware-smoke runner's read-only mode.
- `asfw_apogee_duet_apply_format_dev` is an intentional interruption: use it only when the user authorizes the exact rate change, includes `acknowledgeInterruption: true`, and has confirmed that audio is stopped. It is not suitable for discovery or routine diagnostics.
- Do not rebuild, install, reload, or reset hardware while diagnosing unless
  the user explicitly asks. A user-built/install driver is the live artifact;
  source changes do not affect it until the user elects to rebuild.

## Focused commands

```bash
# Discover tool/resource names without expanding every response in the prompt.
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py tools
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py resources

# Read an advertised resource.
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py read asfw://telemetry/snapshot

# Inspect a read-only tool's full result when needed.
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_avc_list_units '{}'

# Query the bounded driver-owned log ring. `nextSequence` is an exclusive
# cursor for the next call; an empty page can still advance it when a sparse
# filter consumed its scan budget.
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_log_query '{"categories":["CMP"],"contains":"iPCR","maxLevel":"debug","maxRecords":200}'
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_log_stats '{}'

# BridgeCo/BeBoB generic unit PLUG_INFO (fixed, STATUS-only FCP command).
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_bebob_get_unit_plug_info '{"targetGuid":3003878663639543,"nodeId":0,"generation":2}'

# Music Subunit SYNC input and current BridgeCo clock-source topology.
python3 skills/asfw-mcp-control-plane/scripts/asfw_mcp.py call asfw_bebob_get_clock_topology '{"targetGuid":3003878663639543,"nodeId":0,"generation":2}'
```

If the endpoint is unavailable, report the connection failure and ask the user to enable the MCP Control Plane in ASFW. Do not fall back to guessed driver state.

## Optional Console correlation

Do not use Console as the normal ASFW driver-diagnosis path: the MCP driver ring
is retained, queryable, and gives the chronology needed for transport/audio
incidents.  Unified-log correlation is optional only when the question lies
outside the ring, such as app/UI behaviour around an MCP call.

```bash
log stream --info --debug --predicate 'eventMessage CONTAINS "[MCP]" OR eventMessage CONTAINS "[UserClient]" OR eventMessage CONTAINS "[FCP]"'
```

## PHASE 88 / BeBoB discovery rule

For the exact TerraTec PHASE 88 Rack FW identity (vendor `0x000AAC`, model
`0x000003`), do **not** gate BeBoB discovery behind generic AV/C `UNIT_INFO`
or `SUBUNIT_INFO`. Linux BeBoB begins with generic unit `PLUG_INFO`, then
BridgeCo extended ISO-plug type and stream-format-list STATUS commands.

- A generic AV/C inventory with zero subunits or zero ISO plugs is not evidence
  that the device has no BeBoB capabilities.
- Preserve the BridgeCo operand layout: its unit address is the AV/C subunit
  byte (`0xff`); the extension operands begin with direction. Stream-format
  list places support-status before entry index.
- Before treating a BeBoB STATUS command as unsupported, distinguish an FCP
  transport failure or bus reset from an AV/C `REJECTED`/`NOT IMPLEMENTED`
  response. Include `[FCP]` in the user-provided unified-log predicate.
