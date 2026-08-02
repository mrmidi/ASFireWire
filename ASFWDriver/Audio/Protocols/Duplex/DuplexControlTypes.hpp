// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DuplexControlTypes.hpp - Protocol-neutral duplex restart vocabulary

#pragma once

#include "../DICE/Core/DICERestartSession.hpp"

namespace ASFW::Audio {

// The state machine is shared by DICE and AV/C backends. Keep the seam's
// vocabulary neutral even while the owning header retains its historical DICE
// location; device-specific DICE protocol results remain explicitly DICE-named.
using DuplexRestartReason = DICE::DiceRestartReason;
using DuplexRestartPhase = DICE::DiceRestartPhase;
using DuplexRestartState = DICE::DiceRestartState;
using DuplexClockRequestOutcome = DICE::DiceClockRequestOutcome;
using DuplexRestartErrorClass = DICE::DiceRestartErrorClass;
using DuplexRestartFailureCause = DICE::DiceRestartFailureCause;
using DuplexClockRequestCompletion = DICE::DiceClockRequestCompletion;
using DuplexRestartIssueInfo = DICE::DiceRestartIssueInfo;
using DuplexRestartSession = DICE::DiceRestartSession;
using DuplexPrepareResult = DICE::DiceDuplexPrepareResult;
using DuplexStageResult = DICE::DiceDuplexStageResult;
using DuplexConfirmResult = DICE::DiceDuplexConfirmResult;
using ClockApplyResult = DICE::DiceClockApplyResult;
using DuplexHealthResult = DICE::DiceDuplexHealthResult;

using DICE::ClassifyRestartReason;
using DICE::ClearRestartProgress;
using DICE::HasAnyRestartState;
using DICE::HasDeviceRestartState;
using DICE::HasHostRestartState;
using DICE::HasRestartIntent;

} // namespace ASFW::Audio
