**AudioEngine v3 review — 5 September 2026**

I would hold the current low-safety configuration for corrections before hardware acceptance. The immutable PCM cache and explicit presentation plans are useful foundations, but the current implementation still has deadline, clock-observation, and recovery defects. Matching Apple's property totals does not establish that ASFW can meet the corresponding write deadlines.

Reviewed scope: V3 introduction `677a6360`, subsequent audio changes through `3ab346e8`, and the uncommitted A2 payload-rebinding/geometry changes. This is a focused review of publication, packet selection, sample coordinates, ZTS, and latency accounting; it is not an exhaustive audit of every backend or teardown path. No production source or existing draft was changed by this review.

**Findings, in priority order**

1. **[P1, uncommitted] Rebinding uses a stale command pointer, so the two-packet guard is not enforced at the address store.**

   [IsochTxDmaRing.cpp:236](/Users/mrmidi/DEV/ASFireWire/ASFWDriver/Isoch/Transmit/IsochTxDmaRing.cpp:236) publishes and hashes the alternate image before changing the descriptor address. The position used to authorize that store was sampled at line 535, before completion processing and the whole rebind scan. There is no subsequent position or elapsed-time validation. A delay of two cycles can let the controller reach the target before the store. The reported minimum distance is still calculated from the old snapshot, so `minRebindDistance >= 2` does not prove a live two-packet margin.

   The DMA reproduction advances the mock command pointer from packet 6 to packet 8 during alternate-image publication. The real ring code then changes packet 8's address and reports an accepted distance of 2. This proves the missing software guard; it does not simulate controller prefetch or assert that the resulting bytes necessarily tear. The controller can consume the old image while metadata claims the new one.

   Correction: establish and enforce an update deadline at descriptor publication, with a missed-update path that retains the armed image. Account for elapsed time and controller progress during software work. A fresh read alone is not a proof against an arbitrarily long subsequent deschedule. The precise prefetch/update contract still needs reference or hardware evidence. Linux [ohci.c:3208](/Users/mrmidi/DEV/ASFireWire/references/linux-ohci-firewire-low-level-stack/ohci.c:3208) constructs and synchronizes a packet's payload addresses before appending its descriptors; it does not validate A2's live-address replacement rule. The new comment equating a 32-byte prefetch quantum to two packet times is not sufficient evidence for that rule.

2. **[P1, uncommitted] The safety formula measures from transmission, while the sample coordinates are assigned by presentation time.**

   [AudioGeometryPolicy.hpp:75](/Users/mrmidi/DEV/ASFireWire/ASFWDriver/Audio/Shared/AudioGeometryPolicy.hpp:75) reduces safety to eight cycle slots, or 48 frames at 48 kHz, then applies the Duet's 50-frame floor. But [PrepareTransmitSlots:509](/Users/mrmidi/DEV/ASFireWire/ASFWDriver/Audio/DriverKit/ASFWAudioDriverZts.cpp:509) assigns presentation at `transmitBusTicks + replayEntry.sytOffset + transfer`, and the RX anchor uses that same presentation construction at [DirectAudioReceiveConsumer.cpp:467](/Users/mrmidi/DEV/ASFireWire/ASFWDriver/Audio/Engine/Direct/Rx/DirectAudioReceiveConsumer.cpp:467). The initial TX coordinate is derived from that presentation observation. Thus the frame being frozen is ahead of the HAL's current sample coordinate by both the DMA lead and the presentation lead.

   Using the 53,876-tick presentation lead recorded in RTL.md, the idealized coordinate distance is `8 × 6 + 53,876 / 512 = 153.227 frames`, before packet-phase and scheduling allowance. The function reports 50. This is a model calculation from the current code and recorded lead, not a newly measured safe offset or RTL result. Increasing the latency property does not supply the missing write lead.

   The archive addresses this distinction directly: [Jeff Moore, 2002/Aug/msg00055](/Users/mrmidi/DEV/MailingList/wayback-machine-downloader/archives/coreaudio_archives/2002/archives/coreaudio-api/2002/Aug/msg00055.html) explains that output timestamps include safety but exclude hardware latency; [2004/Oct/msg00266](/Users/mrmidi/DEV/MailingList/wayback-machine-downloader/archives/coreaudio_archives/2004/archives/coreaudio-api/2004/Oct/msg00266.html) confirms that audible output follows the output timestamp by hardware latency. The current SDK independently defines safety relative to the current hardware position. See [SetOutputSafetyOffset](https://developer.apple.com/documentation/audiodriverkit/iouseraudiodevice/setoutputsafetyoffset).

   Correction: choose one explicit reference plane for the HAL sample clock and derive the output write deadline in that coordinate system. Either retain presentation coordinates and cover the resulting frame lead, or consistently convert the HAL coordinate, planner, capture position, and latency accounting to another reference plane. Merely moving a term between reported properties cannot make an already-late PCM publication usable. Test 32/64/128-frame writes across every packet phase before lowering safety.

3. **[P1, committed V3] Reading only the newest completion skips ZTS boundaries.**

   [ASFWAudioDriverZts.cpp:236](/Users/mrmidi/DEV/ASFireWire/ASFWDriver/Audio/DriverKit/ASFWAudioDriverZts.cpp:236) reads `stampCount - 1` and submits only that packet to the timeline. [HardwareSampleTimeline.hpp:276](/Users/mrmidi/DEV/ASFireWire/ASFWDriver/Audio/Runtime/HardwareSampleTimeline.hpp:276) publishes a boundary only when it falls inside the submitted packet. It never recovers boundaries from the packets skipped between wakes. A newest NO-DATA packet also causes the generic observer to return despite earlier completed DATA.

   A two-second reproduction using the actual 48-kHz BlockingCadence and HardwareSampleTimeline produces:

   | Observation policy | Published boundary frames |
   |---|---|
   | Every packet | 0, 8192, 16384, 24576, 32768, 40960, 49152, 57344, 65536, 73728, 81920, 90112 |
   | Newest of every 6 packets | 24576, 40960 |
   | Newest of every 12 packets | 40960 |

   This affects TX-derived clock operation, including output-only/fallback paths. The first anchor in the six-packet example arrives around 512 ms after the DATA origin, already beyond a 500-ms startup allowance; startup phase and profile timeout affect the exact runtime outcome. The issue is not solved by changing Raw versus IIR.

   Correction: drain unobserved completions with a cursor and process every relevant DATA observation, or retain sufficient hardware observations to reconstruct each crossed boundary over a justified bounded interval. Test batching and coalescing through the actual observer-selection policy, including NO-DATA endings and M-Audio warm-up qualification.

4. **[P1, uncommitted] Producer acceptance and transport finality have a lost-publication race.**

   [IsochTxDmaRing.cpp:177](/Users/mrmidi/DEV/ASFireWire/ASFWDriver/Isoch/Transmit/IsochTxDmaRing.cpp:177) checks each ready marker, then advances `finalizedEnd` only after the scan at line 281. Meanwhile [DextTxSlotProvider::PublishLatePayload:188](/Users/mrmidi/DEV/ASFireWire/ASFWDriver/Audio/DriverKit/ASFWAudioDriverPrivate.hpp:188) release-publishes the marker and reports success from the still-old frontier. There is no atomic arbitration between “producer accepted” and “transport selected.”

   Reproduced schedule: frontier is 8, current command is 6; transport skips packet 8 because its marker is absent; while processing packet 9, the producer publishes packet 8 and observes `8 >= finalizedEnd`, so it reports success; transport then finalizes through 14. At the next command position 12, packet 8 is retired as image 0. The engine can count this as filled and suppress its silence attribution even though silence was transmitted.

   Correction: use one authoritative per-packet selection/finality decision with a defined linearization point, and account the transport's chosen image. Publishing an image and having that image selected are distinct events. The existing sequence of release/acquire operations does not make them one event.

5. **[P1, committed V3] A pending epoch changes the sample origin without reconciling queued TX and RX coordinates.**

   [HandlePendingTimelineEpoch:194](/Users/mrmidi/DEV/ASFireWire/ASFWDriver/Audio/DriverKit/ASFWAudioDriverZts.cpp:194) chooses `NextBoundaryAfter(lastPublishedBoundary)` and resets the TX cursor to it. Existing armed packets keep their old absolute frames/epoch, the PCM cache is invalidated, and the RX consumer's absolute frame cursor is not translated by this operation. The pending transition then continues through the normal TX preparation callback.

   The timeline reproduction has a committed next-frame cursor of 9000 and last boundary 8192. This transition assigns the next planned range to 16384: a 7384-frame gap, despite uninterrupted backend cadence. The jump size depends on where recovery occurs within the ZTS period. A bigger sample number alone does not preserve the meaning of sample time or align it with the capture ring. The separate RX timing-loss callback also invokes duplex recovery, so this path must be coordinated with that recovery rather than assumed to be the sole transition owner. The reproduction establishes the coordinate change, not a hardware teardown outcome.

   Correction: define one recovery transaction. A source switch that continues IO must preserve/reconstruct the common physical sample coordinate and deliberately handle queued packets/cache identities. A full restart must reset every participating coordinate and queue through the validated lifecycle. Add a duplex recovery test checking HAL read coordinates, RX write coordinates, pending TX ranges, and first post-transition anchors together.

6. **[P2, committed late-fill changes] Two-stream content commits are not all-or-nothing.**

   [ASFWAudioDriverZts.cpp:829](/Users/mrmidi/DEV/ASFireWire/ASFWDriver/Audio/DriverKit/ASFWAudioDriverZts.cpp:829) publishes the primary marker before calling secondary `CommitFill`, whose result is discarded. Either transport context can advance finality between the two calls. The primary may select real content while the secondary refuses or misses selection and transmits silence, contrary to the adjacent comment. Preparing both scratch images before those calls does not coordinate their publication or selection.

   Correction: give streams sharing a frame range a common publication/selection decision, or explicitly handle and report partial acceptance. Do not treat two successful scratch fills as proof of a paired hardware commit. This is a source-level finding; it was not reproduced against a physical dual-stream device.

**Corrections to the archive-derived draft**

| Draft issue | What the evidence supports |
|---|---|
| Clock algorithm is unset; only Raw exposes actual media rate | [Graph.cpp:637](/Users/mrmidi/DEV/ASFireWire/ASFWDriver/Audio/DriverKit/ASFWAudioDriverGraph.cpp:637) explicitly sets SimpleIIR. [Jeff Moore, 2004/Oct/msg00041](/Users/mrmidi/DEV/MailingList/wayback-machine-downloader/archives/coreaudio_archives/2004/archives/coreaudio-api/2004/Oct/msg00041.html) describes filtering as deriving the true device rate while managing jitter. Removing a driver PLL does not require removing host filtering. Choose the algorithm from anchor accuracy/rate-tracking evidence. |
| Prewarming support is unresolved | [Graph.cpp:252](/Users/mrmidi/DEV/ASFireWire/ASFWDriver/Audio/DriverKit/ASFWAudioDriverGraph.cpp:252) passes `false` to `init`, which forwards that capability to the superclass. The draft can state that prewarming is not advertised. |
| The absence of an epoch API proves HAL cannot detect discontinuities, and only StopIO/reconfigure/StartIO is sanctioned | [Jeff Moore, 2009/Oct/msg00098](/Users/mrmidi/DEV/MailingList/wayback-machine-downloader/archives/coreaudio_archives/2009/archives/coreaudio-api/2009/Oct/msg00098.html) describes HAL detecting timestamp discrepancies and resynchronizing, including driver timestamps restarting at zero. That historical behavior does not establish the current ADK filter-reset contract, but contradicts the blanket historical claim. The coordinate defect in finding 5 is independent of this API uncertainty. |
| Publication-span instrumentation needs to be added | [ASFWAudioDriverIO.cpp:116](/Users/mrmidi/DEV/ASFireWire/ASFWDriver/Audio/DriverKit/ASFWAudioDriverIO.cpp:116) already records maximum publication frames and span histograms. Its 513–4096 bucket cannot distinguish 3072 from 4096; maximum/readback can. No observed maximum proves the permitted range. |
| A software-green A2 proves complete-image race resolution | The two DMA interleavings above reproduce missing coverage. Existing host tests prove specific fixed-pointer descriptor outcomes, not the live deadline or cross-queue acceptance protocol. |

The most useful archive principle for further work is that timestamps and latency properties must describe the same hardware path. [Jeff Moore, 2005/Aug/msg00269](/Users/mrmidi/DEV/MailingList/wayback-machine-downloader/archives/coreaudio_archives/2005/archives/coreaudio-api/2005/Aug/msg00269.html) ties IO to accurate hardware timestamps and explicitly allows filtering jittery transport observations. [2002/Dec/msg00140](/Users/mrmidi/DEV/MailingList/wayback-machine-downloader/archives/coreaudio_archives/2002/archives/coreaudio-api/2002/Dec/msg00140.html) distinguishes DMA-to-line latency from the DMA and timestamp-jitter requirements represented by safety.

One adjacent, pre-existing issue deserves a separate change: [Graph.cpp:649](/Users/mrmidi/DEV/ASFireWire/ASFWDriver/Audio/DriverKit/ASFWAudioDriverGraph.cpp:649) assigns clock domain 1 to every endpoint. There is no clock-source qualification there. This advertises a common hardware clock domain even for independently clocked devices. Both [the ADK contract](https://developer.apple.com/documentation/audiodriverkit/iouseraudioclockdevice/setclockdomain) and [Jeff Moore, 2009/Mar/msg00297](/Users/mrmidi/DEV/MailingList/wayback-machine-downloader/archives/coreaudio_archives/2009/archives/coreaudio-api/2009/Mar/msg00297.html) give the domain a hardware-synchronization meaning. It is not a generic “FireWire device” identifier. This line predates the reviewed V3 changes.

**Validation and reproducibility**

- Rebuilt HardwareSampleTimelineTests, AmdtpDirectTxTests, IsochTxDmaRingTests, RxDrivenTimingTests, AudioDeviceSessionManagerTests, and MAudioPresentationObserverTests.
- Focused CTest selection: 83 discovered tests, 82 passed, one deliberately skipped Debug benchmark. Separately ran AudioDeviceSessionManagerTests: all 17 passed.
- [timeline_repro.cpp](/Users/mrmidi/DEV/ASFireWire/documentation/reviews/audioengine-v3-2026-09-05/timeline_repro.cpp) invokes the real timeline, cadence, and geometry policy. It models the production newest-packet selection; it does not compile the ADK observer wrapper.
- [dma_repro.cpp](/Users/mrmidi/DEV/ASFireWire/documentation/reviews/audioengine-v3-2026-09-05/dma_repro.cpp) runs the real DMA ring through the existing host-test fixture and an IIsochDMAMemory decorator that injects deterministic interleavings. Both failure signatures were reproduced. These tests intentionally assert the existing bugs, so their passing is confirmation of the review, not acceptance of the driver.
- Run `python3 documentation/reviews/audioengine-v3-2026-09-05/run_repros.py` from the repository. It uses the configured `build/tests_build`, builds review binaries under `build/audioengine-v3-review`, and does not install a driver.
- SDK checks used Context7's official AudioDriverKit index plus the installed Xcode-beta DriverKit headers. Archive queries used the existing `coreaudio_archive.db` in read-only mode and linked original messages. No RAG-generated answer was treated as primary evidence.

Physical loopback RTL, real controller prefetch, low-buffer dispatch margins, current HAL filter recovery, and device-specific presentation conversions remain unmeasured in this review. The existing asfw_sim remains useful for its documented scenarios, but its old exposed-frame producer model and lack of live DMA/finality/HAL scheduling do not validate A2. No driver installation or hardware test was performed.

The next implementation batch should establish the HAL reference plane and enforce payload selection/finality, then i repair TX observation coverage and coordinate recovery. After host integration tests cover those boundaries, measure physical loopback RTL and actual deadline margins together at 32/64/128-frame buffers. Retain the distinction between an honest property model and measured signal propagation time.
