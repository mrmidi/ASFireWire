// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SessionRunResult.hpp"
#include "NativePeerCleanup.hpp"
#include <string>
#include <optional>
namespace Pro40Packets {class InputRecording;struct PlaybackClip;struct PlaybackPcmSource;class InputMonitor;}
namespace Pro40Audio {template<size_t> class BasicSustainedInputHandoff;}
namespace Pro40 {
struct NativeTimingEvidence;
struct StopQuery {
    bool (*poll)(void *) noexcept=nullptr;
    void *context=nullptr;
    bool requested() const noexcept {return poll && poll(context);}
};
struct PeerCleanupFinalizer {
    // Synchronous supervisor hook, invoked once AFTER native close, never from
    // DMA callbacks. Must finish sender join/control cleanup with bounded waits.
    // Context outlives run. No peer may be created before native entry: invalid,
    // busy, entry-cancel and file-preparation-failure returns do not invoke this hook.
    // It must also handle native-open failure, when no peer was created.
    int32_t (*finish)(void *) noexcept=nullptr;
    void *context=nullptr;
    int32_t status() const noexcept {return finish?finish(context):0;}
};
struct PeerPreparation {
    // Control-thread setup, after successful native open and before ownership,
    // DMA setup or timing/packet callbacks. May allocate/start bounded peer work.
    // Arm peerCleanup before acquiring resources. Failure still runs finalization.
    int32_t (*prepare)(void *) noexcept=nullptr;
    void *context=nullptr;
    int32_t status() const noexcept {return prepare?prepare(context):0;}
};
struct SourceCompletion {
    // Supervisor-only, after native close/capture-file close, BEFORE peer cleanup.
    // accepted is provisional: only a completed, error-free native source may
    // drain END. Otherwise cancel. Must honor stop and a finite wall-time budget.
    // Nonzero becomes evidence failure; peer cleanup always runs afterward.
    int32_t (*finish)(void *,bool,StopQuery) noexcept=nullptr;
    void *context=nullptr;
    int32_t status(bool accepted,StopQuery stop) const noexcept {return finish?finish(context,accepted,stop):0;}
};
struct NativeSessionOptions {
    uint64_t guid=0;
    std::string logPath;
    bool retainHistory=true;
    StopQuery stop;
    // Optional caller-owned content storage; must outlive this synchronous run.
    Pro40Packets::InputRecording *recording=nullptr;
    // Optional preloaded PCM for playback stream A; caller owns immutable
    // storage until return. Does not alter the interface's stored routing.
    const Pro40Packets::PlaybackClip *playback=nullptr;
    // Dedicated live-playback profile. Source is primed before entry, bounded
    // and nonblocking; caller joins its producer only AFTER native return.
    const Pro40Packets::PlaybackPcmSource *livePlayback=nullptr;
    // Dedicated sustained build. Fixed-size input evidence replaces finite PCM
    // storage. Zero stopAfterSeconds means cooperative stop only; nonzero is a
    // bounded acceptance check using the same running path (1..3600 seconds).
    bool sustained=false;
    unsigned stopAfterSeconds=0;
    Pro40Packets::InputMonitor *inputMonitor=nullptr;
    // Isolated sustained-input build only. Fixed owned queue; caller starts its
    // consumer before entry and joins it after native teardown. No peer hook.
    Pro40Audio::BasicSustainedInputHandoff<2048> *inputHandoff=nullptr;
    // An attached active timing consumer requires recording. Consumer callbacks
    // run on the native control owner and must never wait, allocate or perform IO.
    // Consumer failure stops transfer; peerCleanup still determines cleanup status.
    NativeTimingEvidence *timing=nullptr;
    PeerCleanupFinalizer peerCleanup{};
    PeerPreparation peerPreparation{}; // Requires peerCleanup when supplied.
    SourceCompletion sourceCompletion{}; // Requires peerCleanup when supplied.
    // Explicit finite recording diagnostic; never allowed with peer/source hooks.
    bool postReleasePeaks=false;
    bool preCloseQuietHold=false;
    bool ownedIdleObservation=false;
    bool assignedIdleObservation=false;
    bool verifyExistingClockOnly=false;
    // Experimental finite path; not exposed by the worker/app yet.
    bool enableFirstStartup=false;
    bool clockExcursion=false; // Diagnostic only; requires enable-first assigned-idle profile.
    // One finite enable-first initialization with the same owned idle recovery
    // window. Discard PCM; no recorder, timing consumer, peer or raw history.
    bool warmupOnly=false;
    // Experimental PR180 adaptation; one notification-triggered restart before
    // recording. Keeps ownership/clock, rebuilds all host programs. No retry on
    // faults and no compatibility with external consumers or raw history.
    bool startupRecovery=false;
    // Isolated cold-start comparison: initial lock only, then no recurring
    // device status/route reads. Requires its dedicated build and CLI mode.
    bool quietStartupControl=false;
    // Optional supervisor binding, checked before any owner/clock writes.
    std::optional<uint32_t> expectedGeneration{};
};
// Synchronous, one active invocation per process. Options and stop context must
// remain alive through return. Poll must not block/reenter; synchronize any
// cross-thread stop flag in the caller. Runner installs no signal handlers.
// 64=invalid options,75=busy or prior cleanup failure,1=preparation failure,
// 2=unaccepted diagnostic run,0=accepted. Outcome absent on pre-start failure,
// except admitted entry cancellation: interrupted/Aborted, evidence=cleanup=0,
// with no file/native IO. Invalid options and busy admission take precedence.
// Native transport remains singleton; do not mix with other native probes.
// Existing uncertain-finalization path terminates the process with _Exit(3).
NativeSessionResult runNativeSession(const NativeSessionOptions &options);
}
