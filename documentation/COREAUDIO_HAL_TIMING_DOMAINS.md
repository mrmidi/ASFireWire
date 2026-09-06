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

## 1. There are two buffer geometries, and they are not the same thing

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

Clients do **not** share one negotiated size. They run different sizes at the
same time, and the plug-in sees the beat between them:

> your server plug-in doesn't get to say anything about the IO buffer size for
> the client. That is by design as **different clients can have different IO
> buffer sizes at the same time**. For example, Logic might want an IO buffer
> size of 64 frames whereas iTunes is generally pretty happy with the default
> size of 512. The mix engine in the HAL handles dealing with the various buffer
> sizes... The net effect is that the server plug-in has to be prepared for the
> fact that the HAL will call to read/write data of **the various sizes that will
> come up as one client's IO cycle beats against another clients**.
> — Jeff Moore [[M155](#m155)]

which is why the size the plug-in is handed looks erratic:

> an audio server plugin constantly adjusts the buffer size in a best effort to
> satisfy all clients. The `inIOBufferFrameSize` is pretty unpredictable.
> — Eric Gorouben [[M2109](#m2109)]

**Consequence for ASFW:** a client buffer size and an OHCI descriptor ring are
not comparable quantities, even though both are counted in frames. The isoch
context is paced by isoch cycles (8000/s); it is not sized in client frames.

Two things this does **not** license, both claimed here earlier and both wrong:

- *"Only one client buffer size is in force at a time."* False, per M155 above.
  A tool asking for 64 frames does not put Logic's 512 aside; both run.
- *"Client cadence therefore cannot affect the transport at all."* Not
  established. Different geometry means the numbers are not directly comparable,
  not that client timing has no influence on when refills get serviced. Open,
  and answerable only by measurement.

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

So the ZTS sequence is how the driver conveys the sample-clock-to-host-clock
mapping, and client IO cycles are scheduled against the timeline the predictor
builds from it. It is **not** the only thing the driver declares that affects
scheduling: safety offset, latency and nominal rate all participate.

### ASFW's declared geometry

`ASFWDriver/Audio/Shared/AudioHalBufferProfiles.hpp`, profile `audio-engine-v3`:

| field | value | at 48 kHz |
|---|---:|---|
| `frameRingFrames` | 8192 | 170.67 ms |
| `clientIoBudgetFrames` | 512 | 10.67 ms |
| `zeroTimestampPeriodFrames` | 8192 | **170.67 ms** |

The ZTS period currently yields ~5.9 timestamps/second. Whether that is
appropriate is **open** — it has not been validated against what Apple's own
drivers use.

These three rings are distinct and are **not** required to coincide: the audio
frame ring (8192 frames), the declared ZTS period (8192 frames), and the OHCI IT
descriptor ring (48 packets). The IOAudio "one timestamp per DMA wrap" model in
§6 describes where a hardware driver's timestamp came from; it does not impose a
relationship between ASFW's audio ring and its OHCI ring.

## 3. The IO callback's `hostTime` is a prediction — but it is the same clock

> When an application receives an audio callback with a particular
> `AudioTimeStamp`, the HostTime (in the future) in that `AudioTimeStamp` says
> when the beginning of that block of audio will be consumed by the device
> (which is based on the ZeroTimeStamps provided by the device). The audio
> system's choice of this HostTime **has already accounted for things like
> IOBufferSize, device safety offset, etc.**
> — Dan Klingler [[M2110](#m2110)]

Three things follow:

1. `hostTime` is **not** a reading of the clock at the moment of the call. It is
   an output of the HAL's ZTS predictor — a time attached to a *specific event*
   (when that block is scheduled to be consumed/captured by the device).
2. It anchors to the **beginning of the block**, not to any other frame in it.
3. It **already contains** IO buffer size and safety offset. Adding either again
   double-counts.

**It is nevertheless the same clock as `mach_absolute_time()`:**

> The host time in the HAL uses the same time base... The timebase is the one
> that `clock_get_uptime()` already returns. — Jeff Moore [[M8567](#m8567)]

So differencing a `hostTime` against a driver-side `mach_absolute_time()` sample
is arithmetically meaningful, and expresses lead or lateness against that
scheduled event. An earlier revision of this document claimed the opposite —
that the two were incompatible domains and the sign was unstable. **That was
wrong**, and the correction matters, because "lead against the scheduled
consumption instant" is a legitimate and useful thing to measure.

What must be got right is **which event** each endpoint names. J4 is specified as
"capture publication -> client read". `hostTime` is not the instant of the client
read; it is the scheduled reference plane of the block being read. Pairing it
with our publication stamp therefore measures something real but *not J4* — and
because the reference plane sits earlier than publication under normal
operation, the difference reverses. The defect is a mis-specified endpoint, not
a clock mismatch.

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

**Consequence for ASFW — stated carefully.** RTL's residual (`RTL_ts` measured
minus declared hardware latency) is delay on the physical path that our
declaration does not account for. It does **not** follow that the fix is to
declare it.

Latency is measured from the timestamp's reference plane, not from the earlier
moment the HAL handed us the samples. A residual can equally be:

- **real undeclared transport delay** — then declaring it is correct;
- **excessive buffering of our own** — then it should be *removed*, not declared;
- **a timing defect** (e.g. a mis-anchored timeline) — declaring it would bake
  the bug into the property and hide it;
- **measurement error** in the tool.

ASFW's residual is currently displaced by an unexplained whole number of
288-frame ring laps (§6), so at least part of it falls in the third or fourth
category. **Declaring the present figure would conceal the problem rather than
describe the device.** Settle the lap question first. See
`AUDIO_LATENCY_LEDGER_AND_SSOT_PLAN.md`.

## 5. Buffer size changes (IOAudio / HAL plug-in era — scope this)

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

> **Scope.** These two messages are 2004 and describe the **IOAudio / HAL
> plug-in** model. `kAudioDevicePropertyBufferFrameSize` does not even exist for
> an AudioServerPlugin (M155), and AudioDriverKit uses
> `RequestDeviceConfigurationChange` / `PerformDeviceConfigurationChange`
> instead. Treat this section as background on the HAL's expectations, **not** as
> a rule ASFW must implement. Verify current ADK obligations against the SDK
> headers and `ADKVirtualAudioLab`.

## 6. The zero timestamp: cadence, and what the sources do and do not say

### Where a ZTS comes from

> `takeTimeStamp()` is meant to be called from your **_primary_ hardware
> interrupt that gets raised when your DMA wraps around the ring buffer**.
> — Jeff Moore [[M3770](#m3770)]

For an IOAudio hardware driver the ZTS period is therefore the ring buffer size,
one timestamp per DMA wrap. Apple gives no recommended *number*; the stated
constraints are about the timestamp's **accuracy and provenance**, not its rate.
Taking it in a secondary interrupt injects scheduling-latency jitter, and that
gets worse exactly when the machine is loaded [[M22275](#m22275)].

> **Scope.** "From the primary interrupt" is the IOAudio-era prescription for
> getting an *accurate* timestamp. It is not a prohibition on reconstructing an
> earlier hardware time after the fact — Apple's DriverKit audio guidance
> explicitly permits computing a timestamp for a past hardware event. What must
> be accurate is the **value**; where the code runs is a means to that end.

### What the predictor does

> The HAL's internal clock runs off of the time stamps your driver provides. The
> clock has to **filter** these time stamps to derive the true rate of the
> device. This filter code has a certain amount of **delay built into it to help
> manage the jitter**. When the clock starts up, it **primes itself with the
> nominal rate**. It takes **a few time stamps** for the clock to work the actual
> time stamp data through the filter and lock on to the true rate.
> — Jeff Moore [[M14477](#m14477)]

So a coarse ZTS period costs time-to-lock, and until the filter locks the HAL
runs on the *nominal* rate rather than the device's true rate. ASFW's period is
170.67 ms at 48 kHz, so "a few time stamps" is on the order of a second.

> **"A few" is not quantified by the source.** An earlier revision of this
> document tabulated lock times from an assumed 4-8 timestamps. That range was
> invented, not sourced, and the table has been removed. If the lock interval
> matters, measure it — do not derive it from a guess.

### The first timestamp anchors the clock

> the **first time stamp a driver gives to the HAL is the most important time
> stamp the driver delivers**. The reason why is that once the HAL sees the first
> time stamp, it will **anchor it's clock** and begin doing IO. **Any error in
> the first time stamp is passed pretty much directly into the HAL's anchor
> time**, which causes the HAL to **start off** already out of synch with the
> hardware in an amount proportionate to the error. — Jeff Moore [[M3770](#m3770)]

Apple's recommended technique, given generally and again to a FireWire driver
author:

> use your DMA program to do a small transfer, raise an interrupt, and then you
> can take your first time stamp **since you know exactly where the hardware is
> at that point**. — [[M3770](#m3770)], [[M22275](#m22275)]

> you are picking a random point in time prior to starting the hardware and
> calling that your zero point. Frankly, **this is the worst way to do this
> because it has no relationship to when the hardware really started**.
> — Jeff Moore [[M22275](#m22275)]

Plus one FireWire-specific requirement:

> you'll need to have an **independent idea about how the FireWire clock relates
> to the CPU clock**. — Jeff Moore [[M22275](#m22275)]

> **What this does not say.** M3770 says the error goes into the anchor and the
> HAL *starts off* out of sync. It does **not** say the error is permanent or
> never re-derived. M14477 describes an ongoing filter, and M835 describes
> resynchronisation mechanisms. Whether an initial anchor error persists in ASFW
> is an open question to be measured, not a quotable fact.

### The 288-frame RTL offsets — open, with one candidate weakened

`RTL_raw` has been measured at six stream starts as `582.95 + k x 288` frames,
k in {0,1,2,4,7,12}; 288 frames = one 48-packet OHCI IT descriptor-ring lap.
Stable to sd 0.01 within a run; k varies between starts.

**Ruled out:** the first-refill seed. `[TxLapSeed]` reported a clean seed
(`elapsedCycles=3, lapsLostEst=0`) on the very context that measured 4 laps up,
and the same single arm later measured 12 laps up.

**Weakened:** "a TX CommandPtr lap ambiguity corrupts the first ZTS." On the
Duet the ZTS is published from the **receive** path
(`DirectAudioReceiveConsumer.cpp:498`, `HardwareTimelineSource::Receive`), not
from TX completions, so the direct TX-CommandPtr-to-first-ZTS mechanism this
document previously asserted **does not exist in the current code**. A related
mechanism remains plausible — TX frame-to-packet assignment being displaced
*relative to* the RX-anchored timeline — but that is a different claim and is
unverified.

**Still live:** correlator aliasing. An unrefilled ring re-transmits the same 48
packets, so the stimulus genuinely repeats at 288-frame spacing and a weak-SNR
correlator can lock onto the wrong repeat. Evidence is mixed: a 60/60 run at
36.2 dB SNR still landed 4 laps up, while the 12-lap run had ~30% trial
acceptance.

**Next trace (driver-side, no cable/gain dependency):** pair the **first RX ZTS**
with the **TX frame-to-packet / presentation assignment** for the same epoch, and
add an identifiable loopback marker so the acoustic measurement can be aligned
against driver-side truth rather than trusted on its own. Surfacing
`maxCompletionDelta` / `maxCompletionDeltaEvents` (written at
`IsochTxDmaRing.cpp:793,795`, read nowhere) gives an independent check for real
lap loss.

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
- **"`hostTime` and `mach_absolute_time()` are incompatible domains."** Wrong:
  same timebase [[M8567](#m8567)]. J4's defect is a mis-specified *event*.
- **"Only one client IO buffer size is in force at a time."** Wrong: M155 says
  clients run different sizes simultaneously.
- **"The residual is the missing latency declaration."** Only one of four
  possibilities, and not the likeliest while the lap offset is unexplained.
- **Tabulating predictor lock times from an assumed "4-8 timestamps".** The
  source says "a few" and gives no number.
- **"An initial anchor error is permanent."** Not stated by the source.

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
| <a id="m8567"></a>M8567 | 2010-10-14 | **Jeff Moore (Apple)** | Re: Virtual audio device glitches | `2010/Oct/msg00112.html` |

M2110's author is not an Apple address; it is retained because it is the
clearest statement of the `hostTime` contract in the corpus and is consistent
with M155. Treat M155/M14741/M35404/M30713 (Apple) as authoritative and M2110 as
corroborated-but-secondary.
