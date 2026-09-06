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

## 6. The zero timestamp: cadence, and why the *first* one dominates

### Where a ZTS comes from

> `takeTimeStamp()` is meant to be called from your **_primary_ hardware
> interrupt that gets raised when your DMA wraps around the ring buffer**.
> — Jeff Moore [[M3770](#m3770)]

So the ZTS period *is* the ring buffer size, and one ZTS is taken per DMA wrap.
ASFW's `frameRingFrames == zeroTimestampPeriodFrames == 8192` matches that model.

Apple gives no recommended *number*. The stated constraints are about where the
timestamp comes from, not how often:

- from the **primary** interrupt at a known hardware position — not a secondary
  interrupt, not a timer, not `performAudioEngineStart()`;
- taking it in a secondary interrupt injects scheduling-latency jitter, which
  gets worse exactly when the machine is loaded [[M22275](#m22275)].

### What the predictor does, and what a coarse period costs

> The HAL's internal clock runs off of the time stamps your driver provides. The
> clock has to **filter** these time stamps to derive the true rate of the
> device. This filter code has a certain amount of **delay built into it to help
> manage the jitter**. When the clock starts up, it **primes itself with the
> nominal rate**. It takes **a few time stamps** for the clock to work the actual
> time stamp data through the filter and lock on to the true rate.
> — Jeff Moore [[M14477](#m14477)]

The cost of a coarse period is therefore time-to-lock, measured in timestamps:

| ZTS period | one ZTS | "a few" (say 4–8) = time to lock |
|---:|---:|---:|
| 512 fr | 10.7 ms | ~43–85 ms |
| 2048 fr | 42.7 ms | ~170–340 ms |
| **8192 fr (ASFW)** | **170.7 ms** | **~0.7–1.4 s** |

Until the filter locks, the HAL runs on the **nominal** rate, not the device's
true rate. At ASFW's period that is roughly a second of every stream start spent
on an unlocked clock, and the filter has correspondingly few samples to work
with. This is a consequence worth measuring, not yet a proven defect.

### The first timestamp is the anchor, and its error is permanent

> it kind of goes without saying, but **the first time stamp a driver gives to
> the HAL is the most important time stamp the driver delivers**. The reason why
> is that once the HAL sees the first time stamp, it will **anchor it's clock**
> and begin doing IO. **Any error in the first time stamp is passed pretty much
> directly into the HAL's anchor time**, which causes the HAL to start off
> already out of synch with the hardware in an amount proportionate to the error
> in the first time stamp.
>
> This is why drivers need to bend over backward to make the first time stamp as
> accurate as possible. — Jeff Moore [[M3770](#m3770)]

Apple's recommended technique for getting an accurate first ZTS, given twice —
once generally, once to a FireWire driver author:

> use your DMA program to do a small transfer, raise an interrupt, and then you
> can take your first time stamp **since you know exactly where the hardware is
> at that point**. — Jeff Moore [[M3770](#m3770)], [[M22275](#m22275)]

And the anti-pattern, stated to that same FireWire author:

> you are picking a random point in time prior to starting the hardware and
> calling that your zero point. Frankly, **this is the worst way to do this
> because it has no relationship to when the hardware really started**. However,
> it is a pretty common mistake that many driver writers have made.
> — Jeff Moore [[M22275](#m22275)]

Plus one FireWire-specific requirement:

> you'll need to have an **independent idea about how the FireWire clock relates
> to the CPU clock**. This is something that has to be factored into your
> calculations that are producing time stamps. — Jeff Moore [[M22275](#m22275)]

### Why this matters to the 288-frame RTL offsets — hypothesis

**Status: hypothesis, not established.** It is recorded because it is the first
mechanism that accounts for every observed property.

`RTL_ts` has been measured at six stream starts as `582.95 + k x 288` frames
(RTL_raw), k in {0,1,2,4,7,12}. 288 frames = one 48-packet TX descriptor-ring
lap. Within a run the figure is stable to sd 0.01; between starts k changes.

If the first ZTS boundary is computed from a TX packet index that carries the
mod-48 CommandPtr lap ambiguity (see `[[tx-ring-lap-offset]]` and
`DecodeHardwarePacketIndex`), then a first ZTS wrong by k laps anchors the HAL's
clock 288k frames wrong — and per M3770 that error goes in **directly** and is
never re-derived. That predicts exactly what is seen:

| observation | explained by a wrong first ZTS? |
|---|---|
| offsets are integer multiples of one ring lap | yes — the ambiguity is in whole laps |
| stable within a run (sd 0.01) | yes — the HAL anchors once and keeps it |
| varies between starts | yes — the ambiguity resolves differently per arm |
| `[TxLapSeed]` reports a clean seed while `RTL_ts` is displaced | yes — the seed measures the *refill cursor*; the ZTS boundary is a different path with its own lap ambiguity |

**Before treating this as established** it has to be separated from the
competing explanation, which is that a weak-SNR correlator aliases onto the
repeating ring content (an unrefilled ring re-transmits the same 48 packets, so
the stimulus genuinely repeats at 288-frame spacing). Both predict lap-quantised
offsets. The discriminator is driver-side: surface `maxCompletionDelta` and the
first published ZTS boundary, and check them against a measurement with high
trial acceptance.

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
| <a id="m3770"></a>M3770 | 2008-12-09 | **Jeff Moore (Apple)** | Re: PCI Audio Driver and takeTimeStamp | `2008/Dec/msg00084.html` |
| <a id="m22275"></a>M22275 | 2009-02-27 | **Jeff Moore (Apple)** | Re: Driver seems a little dizzy while running more than one media player | `2009/Feb/msg00533.html` |
| <a id="m14477"></a>M14477 | 2004-10-05 | **Jeff Moore (Apple)** | Re: HAL timing issues | `2004/Oct/msg00041.html` |
| <a id="m835"></a>M835 | 2013-12-12 | **Jeff Moore (Apple)** | Re: AudioServerPlugin: resynchronization | `2013/Dec/msg00059.html` |

M2110's author is not an Apple address; it is retained because it is the
clearest statement of the `hostTime` contract in the corpus and is consistent
with M155. Treat M155/M14741/M35404/M30713 (Apple) as authoritative and M2110 as
corroborated-but-secondary.
