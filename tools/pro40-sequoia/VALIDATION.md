# Validation through 2026-10-13

Hardware results below apply to one original Saffire Pro 40 and one M1 Mac
Studio on Sequoia 15.7.9. Software simulations are listed separately. No raw
audio, device identifier, host path or development signing identity is published.

## Observed hardware/application results

| Check | Result and limit |
| --- | --- |
| CoreAudio device | 8 inputs, 6 outputs, internal 48 kHz; visible to Nuendo and VLC |
| VLC speaker test | User heard all six outputs in L, R, C, LFE, LS, RS order |
| Nuendo input 1 | Recorded microphone take replayed cleanly through left/right; direct monitoring used |
| Shared output | Owner reports simultaneous Nuendo and browser audio working |
| Earlier endurance | Ten-minute CoreAudio run passed native timing/cleanup; meters remained responsive |
| Updated installed device | Silent 180-second and 24-second sessions passed native evidence, stopped prefixes and independent release receipts |
| Updated native starts | 12/12 short sessions passed after output-device switches; Start 2925–2991 ms, clean joined shutdown after each |
| Shared persistent stream | Four clients joined the same native worker at 11, 11, 10, 10 ms; gaps and output switches did not restart FireWire |
| Persistent-stream stop | All four published tails drained, zero native work overruns, independent cleanup confirmed |
| Rebootless recovery | Completed after those checks; a new persistent stream and warm client passed, keep-active left enabled |
| Physical meters | Owner confirms responsive throughout updated installed/restart tests and afterward |

The new restart run did not contain a malformed first data payload. A retained
real failure was replayed offline, and generated data reproduces its displaced
labels. Previous failed runs remain failures; their existence is not hidden by
the later success. The stop-race repair and pre-roll change have different causes.

## Software checks

- ASan/UBSan: packet/layout/label validation, timestamp and DBC wraps, independent
  stream replay, first-payload quarantine, unavailable frame reads, exact later
  samples, receive-buffer reuse, input queues, pending publication and teardown.
- TSan: concurrent producer/consumer input handoff and window publication.
- 600-second packet/clock simulation: two rate changes, retained exact PCM,
  immutable timestamps, expiry, input/output mapping and native presentation.
- Generated managed-worker checks under ASan/UBSan and TSan: normal startup,
  rejection, cancellation, wrong release, parent Stop, underrun and unclean exit.
- Static native package and CoreAudio bundle: compiled, development-signed,
  verified for expected imports, exact peer identities and discovery properties.

The focused offline suite is reproducible from this clean source export.
The private retained capture is optional and not needed to run it.

## Not qualified

There is no three-hour hardware result, production reliability claim, or promise
for other hardware/OS versions. Physical round-trip latency remains about
225.8 ms from an earlier loopback measurement, distinct from app Start time.
Cold input mute still relies on a login warm-up workaround on the working host.
Sleep/wake, hot unplug, power-cycle recovery, the new login combination, portable
deployment, other rates/clocks/channels, MIDI and QuickTime surround remain work
in progress. The owner's practical Nuendo use does not replace controlled
endurance testing.
