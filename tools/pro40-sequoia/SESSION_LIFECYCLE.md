# Session ownership, recovery and keeping the stream active

The installed development version starts audio through ordinary CoreAudio
clients. A last-client Stop closes admission and drives the native session
through bounded joined shutdown. A new session gets fresh process, generation,
clock and ownership observations. It must not inherit a failed session's state.

## Rebootless recovery

The original disappearing-device report exposed a stop race in a read-only TX
preparation gate. Stop arrived after `ReceiveBankHandoffs::begin()` but before
any packet/range/branch publication. The old `finish(false)` error path marked
the already published terminal graph unusable, preventing its normal drain.

`stopBeforePublication()` cancels only a provably unpublished, read-only pending
preparation. It permanently prevents another handoff while preserving the known
terminal watermark. Previous faults or uncertain/failed publication remain
terminal. Ordinary Stop then drains all four published null tails before native
Stop, release and buffer destruction. The five cancellation guard positions and
sticky-fault behavior are covered by `NativeStopDrainChecks.cpp`.

`session/manage.py` contains the development recovery sequence. It verifies
installed hashes and exact service definitions, joins the silent client's stop,
requires successful native cleanup, audits the independent ownership journal,
checks that no native worker/verifier or active lease remains, closes admission,
then starts a fresh controller and restarts CoreAudio. Fresh readiness belongs
to the current PID. Original failed evidence is preserved; recovery never
clears a dirty lease or unresolved journal to manufacture success. Restarting
CoreAudio affects other audio applications, so they must first be closed.

The exposed operational policy remains development-host specific. The public
source snapshot does not supply a portable installer or permission to infer
release from stale status files. See README before adapting these tools.

## Persistent stream

`session/KeepActive.cpp` is an ordinary CoreAudio output client. It writes
bounded silence into six output channels, allowing CoreAudio to mix other
applications over it. It does not access FireWire, select the default audio
device, record microphone samples or log/allocate inside the callback.

Input stream usage is disabled and read back. On the tested Sequoia host,
CoreAudio nevertheless supplied a non-null input-buffer pointer. The callback
ignores the complete input list. Output geometry, liveness and device identity
remain checked, and control-thread faults stop the client without a retry loop.

The development login job waits for the existing warm-up's success in the
current boot. Missing/stale/failed warm-up evidence blocks it. RunAtLoad is true;
KeepAlive is false. A failed hardware stream is not repeatedly restarted.
The host's warm-up workaround is not included as a portable installation package.

Keeping this client active removes repeated native start delays between apps:
measured app Start calls fell from roughly 2.9–3.1 seconds to 10–11 ms. It does
not reduce physical audio buffering latency. Recovery starts a fresh native
stream on the already powered/warmed interface; power cycling or waking from
sleep is a separate, unqualified lifecycle.
