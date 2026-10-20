// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MidiRunBracket.hpp - How an audio stream start and stop touch the MIDI seam.
//
// The whole of MIDI's relationship to the audio stream lifecycle is these two
// calls, and they are deliberately thin:
//
//   * the start runs UNCHANGED, whatever MIDI is doing. MIDI holds no lease and
//     no reference on the session, so a session that stopped is a session that
//     starts again from scratch -- the same transport arm, the same IT start --
//     with a MIDI client open or not. (On the midi branch a MIDI lease kept the
//     shared session "streaming" across a CoreAudio stop/start, so its start
//     path never re-ran; with the MIDI nub published, audio did not recover.)
//
//   * a start that brings streams up publishes a new epoch BEFORE the session
//     call, so the first received packet is delivered into an armed seam; the
//     epoch is withdrawn if that start fails. A start against streams that are
//     already running (a second client joining) changes nothing: moving the
//     epoch there would make every consumer drop bytes mid-stream.
//
//   * the seam is quiesced only after a stop that succeeded. A stop that failed
//     may have left the wire up, so MIDI stays armed with it.
//
// Header-only and parameterised on the session call so the host suite drives
// the real session scheduler through exactly the code the coordinator uses.

#pragma once

#include "AudioEndpointRuntime.hpp"

#include <DriverKit/IOReturn.h>

#include <utility>

namespace ASFW::Audio {

/// Run `attach` (the session's start) inside the endpoint's MIDI run.
/// `endpoint` may be null (no runtime yet) or have no MIDI; both are no-ops.
/// `alreadyStreaming` is the session's state before the call.
template <typename Attach>
[[nodiscard]] IOReturn StartWithMidiRun(AudioEndpointRuntime* endpoint, bool alreadyStreaming,
                                        Attach&& attach) {
    if (endpoint == nullptr || alreadyStreaming) return std::forward<Attach>(attach)();
    (void)endpoint->BeginMidiRun();
    const IOReturn status = std::forward<Attach>(attach)();
    if (status != kIOReturnSuccess) endpoint->EndMidiRun();
    return status;
}

/// Run `detach` (the session's stop), then withdraw the MIDI run if it worked.
template <typename Detach>
[[nodiscard]] IOReturn StopWithMidiRun(AudioEndpointRuntime* endpoint, Detach&& detach) {
    const IOReturn status = std::forward<Detach>(detach)();
    if (status == kIOReturnSuccess && endpoint != nullptr) endpoint->EndMidiRun();
    return status;
}

} // namespace ASFW::Audio
