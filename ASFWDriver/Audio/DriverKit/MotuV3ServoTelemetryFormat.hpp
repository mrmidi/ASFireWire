// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuV3ServoTelemetryFormat.hpp - Ring formats of the MOTU v3 SPH servo lines.
//
// A LogRecord message holds 231 characters, and ASFW_LOG_RING_ONLY_RL prefixes
// "[<category>][<key>] " to the format. A single servo line outgrew that: in
// steady state the cut landed at or before `clamped=`, so the flags, the mute
// and the event counters never reached the ring.
// The line is therefore split in two, and tests/audio/MotuV3ServoTelemetry-
// FormatTests.cpp pins both, at a week of streaming, inside one record.
//
// Kept as macros because the logging macro pastes the format as a literal.
//
// [MotuSphServo] keeps its prefix through `phaseTicks=` stable: offline
// analysers match the prefix up to `stepQ32=` and read `locked=` and
// `phaseTicks=` from it.
// Everything that used to follow moved to [MotuSphState], which carries the
// same `gen=` so the two lines pair up per stream.

#pragma once

#define ASFW_MOTU_SPH_SERVO_KEY "motu-sph-servo"
#define ASFW_MOTU_SPH_SERVO_FORMAT                                                \
    "[MotuSphServo] gen=%llu updates=%llu decisions=%llu "                        \
    "frames=%llu rxTicks=%lld txCorrQ32=%lld stepQ32=%lld "                       \
    "locked=%u phaseTicks=%lld"

// `mutedFrames` is MotuV3PayloadWriter::FramesIntentionallyMuted(): host frames
// the acquisition mute replaced with silence, cumulative for the stream. Zero
// on a clean start (U3 checklist §6).
#define ASFW_MOTU_SPH_STATE_KEY "motu-sph-state"
#define ASFW_MOTU_SPH_STATE_FORMAT                                                \
    "[MotuSphState] gen=%llu measuredQ32=%lld feedback=%u rebase=%u "             \
    "clamped=%u hard=%u repair=%u muted=%u "                                      \
    "counts=%llu/%llu/%llu/%llu mutedFrames=%llu"
