// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../../../Duplex/DuplexControlTypes.hpp"

namespace ASFW::Audio::DICE {

using DiceRestartReason = ::ASFW::Audio::DuplexRestartReason;
using DiceRestartPhase = ::ASFW::Audio::DuplexRestartPhase;
using DiceRestartState = ::ASFW::Audio::DuplexRestartState;
using DiceClockRequestOutcome = ::ASFW::Audio::DuplexClockRequestOutcome;
using DiceRestartErrorClass = ::ASFW::Audio::DuplexRestartErrorClass;
using DiceRestartFailureCause = ::ASFW::Audio::DuplexRestartFailureCause;
using DiceClockRequestCompletion = ::ASFW::Audio::DuplexClockRequestCompletion;
using DiceRestartIssueInfo = ::ASFW::Audio::DuplexRestartIssueInfo;
using DiceRestartSession = ::ASFW::Audio::DuplexRestartSession;
using DiceDuplexPrepareResult = ::ASFW::Audio::DuplexPrepareResult;
using DiceDuplexStageResult = ::ASFW::Audio::DuplexStageResult;
using DiceDuplexConfirmResult = ::ASFW::Audio::DuplexConfirmResult;
using DiceClockApplyResult = ::ASFW::Audio::ClockApplyResult;
using DiceDuplexHealthResult = ::ASFW::Audio::DuplexHealthResult;

using ::ASFW::Audio::ClassifyRestartReason;
using ::ASFW::Audio::ClearRestartProgress;
using ::ASFW::Audio::HasAnyRestartState;
using ::ASFW::Audio::HasDeviceRestartState;
using ::ASFW::Audio::HasHostRestartState;
using ::ASFW::Audio::HasRestartIntent;

} // namespace ASFW::Audio::DICE
