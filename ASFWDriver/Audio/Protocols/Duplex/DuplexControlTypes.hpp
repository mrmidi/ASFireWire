// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DuplexControlTypes.hpp - Neutral names for the pre-FW-73 restart vocabulary

#pragma once

#include "../DICE/Core/DICERestartSession.hpp"

namespace ASFW::Audio {

// FW-71 exposes these neutral names at the duplex seam. FW-73 will mechanically
// rename the underlying restart/session declarations and move their files.
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

} // namespace ASFW::Audio
