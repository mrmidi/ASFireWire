# CoreAudio HAL timing domains

Which buffer size means what, what the IO callback's `hostTime` actually is, and
what `kAudioDevicePropertyLatency` is allowed to contain.

Sourced from the Apple `coreaudio-api` mailing list archive (see
[Citations](#citations)). These are statements by the CoreAudio HAL authors and
are the behavioural ground truth for the questions below — the headers and
`ctx7` describe the *API surface* and say almost nothing about these semantics.

> **Why this document exists.** Every one of the errors in
> [Mistakes this corrects](#mistakes-this-corrects) was made in-tree, more than
> once, by reasoning about frame counts without asking which domain they lived
> in. Two numbers both counted in "frames" are not comparable by that fact
> alone.

## 1. There are two buffer geometries, and they are unrelated

| | Client IO buffer | Driver ring + ZTS period |
|---|---|---|
| Property | `kAudioDevicePropertyBufferFrameSize` | `kAudioDevicePropertyZeroTimeStampPeriod` |
| Scope | **per client process** | the device / driver |
| Who sets it | the app (Logic's "I/O Buffer Size") | the driver |
| Meaning to the driver | almost none | it *is* the driver's ring geometry |

> `kAudioDevicePropertyBufferFrameSize` represents the IO buffer size (in
> frames) for the client process. It is probably a non-intuitive thing, but
> this value actually has surprisingly little meaning for the server plug-in.
> — Jeff Moore, Apple Core Audio [[M155](#m155)]

The HAL negotiates one device buffer size across all clients, so a single
client's requested size is not what the device runs at:

> an audio server plugin constantly adjusts the buffer size in a best effort to
> satisfy all clients. The `inIOBufferFrameSize` is pretty unpredictable.
> — Eric Gorouben [[M2109](#m2109)]

**Consequence for ASFW:** changing a client's IO buffer size cannot re-arm,
re-prefill or otherwise perturb the OHCI isochronous context. The isoch context
is paced by isoch cycles (8000/s) and its descriptor ring is transport geometry.
A client buffer size and a DMA descriptor ring are not comparable quantities
even though both are counted in frames.

## 2. Sync runs off zero timestamps, not off buffer sizes

> The hardware provides a sequence of time stamps that map the sample clock to
> the host clock. The interval between these time stamps is precisely the number
> of frames that `kAudioDevicePropertyZeroTimeStampPeriod` reports. This
> corresponds precisely with the zero time stamps that an IOAudio-based driver
> provides that are based on the driver's ring buffer.
>
> The HAL takes this sequence of time stamps and uses them as the input into a
> **predictor**. The predictor provides the HAL with the ability to know what
> host time a given sample time will correspond to. This is then used to
> schedule all the IO threads so that they do their work at the appropriate
> time. — Jeff Moore [[M155](#m155)]

So the ZTS sequence is the only channel by which the driver influences HAL
scheduling. Client buffer sizes are scheduled *inside* the timeline the
predictor produces.

### ASFW's declared geometry

`ASFWDriver/Audio/Shared/AudioHalBufferProfiles.hpp`, profile `audio-engine-v3`:

| field | value | at 48 kHz |
|---|---:|---|
| `frameRingFrames` | 8192 | 170.67 ms |
| `clientIoBudgetFrames` | 512 | 10.67 ms |
| `zeroTimestampPeriodFrames` | 8192 | **170.67 ms** |

The ZTS period is the predictor's only input and currently updates ~5.9×/second.
Whether that is appropriate is **open** — it has not been validated against what
Apple's own drivers use, and it is the driver-side knob that actually governs
HAL scheduling.

## 3. The IO callback's `hostTime` is a prediction, not a clock reading

> When an application receives an audio callback with a particular
> `AudioTimeStamp`, the HostTime (in the future) in that `AudioTimeStamp` says
> when the beginning of that block of audio will be consumed by the device
> (which is based on the ZeroTimeStamps provided by the device). The audio
> system's choice of this HostTime **has already accounted for things like
> IOBufferSize, device safety offset, etc.**
> — Dan Klingler [[M2110](#m2110)]

Three things follow, all of which have been got wrong in-tree:

1. `hostTime` is **not** `mach_absolute_time()` at the moment of the call. It is
   an output of the HAL's ZTS predictor, in the HAL's timeline.
2. It anchors to the **beginning of the block**, not to any other frame in it.
3. It **already contains** IO buffer size and safety offset. Adding either again
   double-counts.

**Never difference a `hostTime` against a `mach_absolute_time()` sample taken
inside the driver.** Those are different domains; the result has no meaning and
its sign is not even stable. Measure within one domain: either both endpoints
wall-clock, or both endpoints ZTS-predicted.

## 4. What `kAudioDevicePropertyLatency` must contain

> The device's `kAudioDevicePropertyLatency` should describe the **additional**
> delay from the consumption of the samples to hearing it at the speaker.
> — Dan Klingler [[M2110](#m2110)]

> the driver's safety offset affects the latency as well. This is the number of
> frames the HAL must leave in front of the engine's output head to assure
> things get transferred correctly. If you were trying to calculate the earliest
> a frame of audio will hit the wire, you would sum the start time with the
> safety offset and the latency. — Jeff Moore [[M30713](#m30713)]

So the declaration contract is:

```
declared latency  = delay AFTER the HAL hands us the samples
                    (wire transit + device buffering + DAC)
NOT included      = IO buffer size, safety offset   (HAL already counts these)
earliest on wire  = start time + safety offset + latency
```

**Consequence for ASFW:** the residual RTL reports (`RTL_ts` measured minus
declared hardware latency) is, by this definition, exactly the part of the
FireWire path we fail to declare in `kAudioDevicePropertyLatency`. It is not a
mystery term — it is the missing declaration, and it is the Phase 2
reference-plane input. See `AUDIO_LATENCY_LEDGER_AND_SSOT_PLAN.md`.

## 5. Buffer size changes are synchronous, and imply four notifications

> When an app calls `AudioDeviceSetProperty()` to change the IO buffer size on
> an IOAudio device, unless the call is being made from the actual IO thread,
> the entire operation happens on the thread on which the
> `AudioDeviceSetProperty()` call was made and completes in it's entirety before
> `AudioDeviceSetProperty()` even returns. If the call is made on the IO thread,
> it returns immediately and the HAL defers performing the operation until after
> it has finished calling all the IOProcs for the current cycle.
> — Jeff Moore [[M35404](#m35404)]

> a change to `kAudioDevicePropertyBufferFrameSize` usually also implies a
> change to `kAudioDevicePropertyBufferSize` too. You are also be forgetting to
> send the notification to both the input and output sides.
> — Jeff Moore [[M14741](#m14741)]

i.e. four notifications, and a plug-in is expected to emulate HAL semantics
exactly: *"Your plug-in is expected to emulate the semantics of the rest of the
HAL. It's part of the reason writing these plug-ins is so hard."* [[M14741](#m14741)]

## Mistakes this corrects

Recorded so they are not repeated:

- **"Logic is at 512, and 512 frames = 1.78 TX ring laps, so the client buffer
  drives lap loss."** Wrong: compares a client-process buffer against an OHCI
  descriptor ring. Unrelated domains (§1).
- **"Changing the client buffer size did not re-arm the TX context — that's a
  finding."** Not a finding; §1 says it never could.
- **"J4 reverses because F5 anchors to the span's oldest frame and F4 to its
  newest."** Wrong mechanism. J4 reverses because it differences a ZTS-predicted
  `hostTime` against a driver wall-clock stamp (§3).
- **Reading a client's displayed "Resulting Latency" as a device property.** It
  is client-side arithmetic over that client's own buffer size plus our declared
  safety offset and device latency.

## Citations

Archive: local mirror of `lists.apple.com` `coreaudio-api`, reconstructed from
the Internet Archive. Tooling and corpus at
`/Users/mrmidi/DEV/MailingList/wayback-machine-downloader` (not in this repo).

Look a message up by its archive path:

```bash
cd /Users/mrmidi/DEV/MailingList/wayback-machine-downloader
python3 -c "import sqlite3;print(sqlite3.connect('coreaudio_archive.db').execute(
  'select body from messages where archive_path=?',('2013/Mar/msg00154.html',)).fetchone()[0])"
# or:  ./mail_tool.py show <id>   /   ./mail_tool.py search "<terms>" --apple-only
```

| tag | date | author | subject | archive path |
|---|---|---|---|---|
| <a id="m155"></a>M155 | 2013-03-26 | **Jeff Moore (Apple)** | Re: Buffer size change property? | `2013/Mar/msg00154.html` |
| <a id="m2109"></a>M2109 | 2015-04-24 | Eric Gorouben | AudioServer plugin: how to calculate latency? | `2015/Apr/msg00057.html` |
| <a id="m2110"></a>M2110 | 2015-04-24 | Dan Klingler | Re: AudioServer plugin: how to calculate latency? | `2015/Apr/msg00058.html` |
| <a id="m14741"></a>M14741 | 2004-12-10 | **Jeff Moore (Apple)** | Re: kAudioDevicePropertyBufferFrameSize notification in a driver | `2004/Dec/msg00088.html` |
| <a id="m35404"></a>M35404 | 2004-12-13 | **Jeff Moore (Apple)** | Re: kAudioDevicePropertyBufferFrameSize notification in a driver | `2004/Dec/msg00104.html` |
| <a id="m30713"></a>M30713 | 2002-05-30 | **Jeff Moore (Apple)** | Re: Stream/Device Latency | `2002/May/msg00150.html` |

M2110's author is not an Apple address; it is retained because it is the
clearest statement of the `hostTime` contract in the corpus and is consistent
with M155. Treat M155/M14741/M35404/M30713 (Apple) as authoritative and M2110 as
corroborated-but-secondary.
