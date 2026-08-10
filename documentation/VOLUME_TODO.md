# VOLUME_TODO — Master volume for Terratec Phase 88 (and general ADK volume control)

**Status:** design note / not started. Goal: make the macOS volume keys (F11/F12 + menu-bar
slider) attenuate the Phase 88 output, which today runs at full scale ("screaming").

---

## Why it's at full blast

From the reference signal flow (`references/alsa-userspace-control-protocols-impl/protocols/bebob/src/terratec/phase88.rs:9-36`),
the Phase 88 has **no volume Feature Function Block on its physical outputs**. Volume FBs exist
*only* on the mixer path:

| FB | Role | Reference |
|----|------|-----------|
| `0x02`–`0x06` | mixer **input** trims (analog-in 1..8, digital-in 1/2) | `Phase88MixerPhysSourceProtocol` phase88.rs:138-153 |
| `0x07` | stream-source → mixer level | `Phase88MixerStreamSourceProtocol` phase88.rs:161-168 |
| `0x00`/`0x01` | **mixer-output-1/2 level** | `Phase88MixerOutputProtocol` phase88.rs:187-191 |

The analog outputs themselves are driven by **selectors**, not volume — each physical output
picks *either* mixer-output *or* the raw stream-input (the `or` junctions at phase88.rs:31-35).
In the direct path there is no attenuator at all.

Our code currently routes through the mixer but **pins every volume FB to `0x0000` = 0 dB (max)**
and unmutes:
`ASFWDriver/Audio/Protocols/BeBoB/Phase88Protocol.cpp:287-300` (the `steps` table — "Max Vol
Stream Playback" / "Max Vol Mixer Output"). So the hardware *can* attenuate; we're just slamming
it wide open on clock-apply.

---

## Feasibility: yes — machinery already exists

**SDK class:** `IOUserAudioLevelControl` (DriverKit25.5 SDK, `AudioDriverKit.framework`).
- `Create(driver, isSettable, decibelValue, {min,max} dB range, element, scope, classID)`
- override `HandleChangeDecibelValue(float)` / `HandleChangeScalarValue(float)` — called when the
  HAL/keys move the level
- class ID `IOUserAudioClassID::VolumeControl = 'vlme'`, scope
  `kIOUserAudioObjectPropertyScopeOutput`, element `IOUserAudioObjectPropertyElementMain = 0`

**Keyboard-keys requirement:** for F11/F12 + menu-bar slider to be active, the device must be the
**default output** *and* expose a **settable master `VolumeControl`** on output scope, **element 0**
(a per-channel-only control leaves the keys greyed). Add a `MuteControl` boolean on element 0 too
and the mute key works.

**Existing plumbing pattern to mirror:** `ASFWProtocolBooleanControl : IOUserAudioBooleanControl`
(phantom power / phase invert) —
- `ASFWDriver/Audio/DriverKit/Controls/ASFWProtocolBooleanControl.{iig,cpp}`
- created + `audioDevice.AddControl()` in `AudioControlBuilder.cpp:119`
- flow: `HandleChangeControlValue → ownerDriver->ApplyProtocolBooleanControl → RPC to nub → device protocol`

A `ASFWProtocolLevelControl : IOUserAudioLevelControl` is a near-copy of that.

---

## Two implementation options

### Option A — hardware volume (recommended for Phase 88)

`HandleChangeDecibelValue` → RPC → `Phase88Protocol` sends `AudioFunctionBlockCommand kVolume` to
mixer-output FBs `0x00`/`0x01` (the ones already written in Phase88Protocol.cpp:298-299), **driven
by the control instead of pinned to max**.

- AV/C volume is signed 16-bit in **1/256 dB**, `0x0000` = 0 dB (max), `0x8000` = −∞ (mute) — this
  is exactly the reference's `LEVEL_MIN/MAX/STEP` (`bebob/src/lib.rs:285-289`). Clean dB mapping.
  e.g. −12 dB → −12×256 = −3072 = `0xF400`.
- Attenuates in hardware → full bit depth preserved.
- **Caveat:** governs the *mixer-output* path (12×2 stereo). Read `CtlAttr::Minimum/Maximum/Resolution`
  once from the device to get its real range instead of trusting placeholder constants.
- **Verify on wire first:** confirm FB `0x00`/`0x01` actually accept a non-zero volume value before
  trusting it — one HW check worth spending.

### Option B — software gain (device-independent fallback)

Same published control, but `HandleChangeScalarValue` just stores a gain scalar; apply it as a
per-sample multiply on the TX path in `Audio/Wire` (RawPcm24In32) **before** AM824 encode.

- Works for *any* device/topology; no per-keypress wire round-trip.
- Costs bit depth at low levels (digital attenuation); adds a multiply to the hot path — keep it a
  bare multiply, **no logging** (per CLAUDE.md hot-path rule).

**Recommendation:** Option A for the Phase 88 (smallest change, hardware attenuation, mixer path
already wired). Option B is the better general answer for devices with no output FB.

---

## Next steps (when picked up)

1. Add `ASFWProtocolLevelControl : IOUserAudioLevelControl` (mirror `ASFWProtocolBooleanControl`).
2. Wire it into `AudioControlBuilder` — master `VolumeControl`, output scope, element 0, settable;
   plus a master `MuteControl` boolean for the mute key.
3. Add `ApplyProtocolLevelControl` on `ASFWAudioDriver` + nub RPC (mirror the boolean path).
4. Option A: change `Phase88Protocol.cpp:287-300` so FB `0x00`/`0x01` (and/or `0x07`) take the
   control's dB value instead of `0x0000`; seed the control's initial value from a `status` read.
5. HW check: confirm mixer-output FBs accept non-max volume on the wire before trusting.
6. Regenerate `ASFW.xcodeproj` (`xcodegen generate`) after adding the new `.iig`/`.cpp` files.
