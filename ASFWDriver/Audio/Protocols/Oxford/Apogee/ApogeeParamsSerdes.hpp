// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeParamsSerdes.hpp - The Duet half of the params control plane (FW-128).
//
// Pure, static, transport-free: parameter struct <-> vendor command list. The
// engine that drives these lives in Oxford/OxfwParamsEngine.hpp and knows none
// of the types below.
//
// The reference draws the same line - a per-device serdes trait supplying three
// operations, consumed by device-agnostic cache/update operations:
//
//   protocols/oxfw/src/apogee.rs:36-45   DuetFwParamsSerdes<T>
//     default_cmds_for_params -> BuildQuery
//     cmds_from_params        -> BuildControl
//     cmds_to_params          -> Parse
//
// (snd-firewire-ctl-services, read as a behavioural source; no code copied.)
//
// Each group also exposes a serdes struct binding those three functions to the
// engine's expected shape. Ordering inside BuildControl is part of the contract:
// the diff is positional, so a group must emit the same commands in the same
// order for every value of its params.

#pragma once

#include <vector>

#include "ApogeeTypes.hpp"
#include "ApogeeVendorCodec.hpp"

namespace ASFW::Audio::Oxford::Apogee::ParamsSerdes {

using Command = ApogeeVendorCommand;

//==============================================================================
// Knob - one packed hardware-state command rather than a command per field
//==============================================================================

[[nodiscard]] std::vector<Command> BuildKnobStateQuery();
[[nodiscard]] Command BuildKnobStateControl(const KnobState& state);
[[nodiscard]] KnobState ParseKnobState(const Command& command);

//==============================================================================
// Output
//==============================================================================

[[nodiscard]] std::vector<Command> BuildOutputParamsQuery();
[[nodiscard]] std::vector<Command> BuildOutputParamsControl(const OutputParams& params);
[[nodiscard]] OutputParams ParseOutputParams(const std::vector<Command>& commands);

//==============================================================================
// Input
//==============================================================================

[[nodiscard]] std::vector<Command> BuildInputParamsQuery();
[[nodiscard]] std::vector<Command> BuildInputParamsControl(const InputParams& params);
[[nodiscard]] InputParams ParseInputParams(const std::vector<Command>& commands);

//==============================================================================
// Mixer
//==============================================================================

[[nodiscard]] std::vector<Command> BuildMixerParamsQuery();
[[nodiscard]] std::vector<Command> BuildMixerParamsControl(const MixerParams& params);
[[nodiscard]] MixerParams ParseMixerParams(const std::vector<Command>& commands);

//==============================================================================
// Display
//==============================================================================

[[nodiscard]] std::vector<Command> BuildDisplayParamsQuery();
[[nodiscard]] std::vector<Command> BuildDisplayParamsControl(const DisplayParams& params);
[[nodiscard]] DisplayParams ParseDisplayParams(const std::vector<Command>& commands);

//==============================================================================
// Engine bindings - one struct per group, satisfying the OxfwParamsCache shape
//==============================================================================

struct KnobSerdes {
    using Params = KnobState;
    using Command = ParamsSerdes::Command;

    [[nodiscard]] static std::vector<Command> BuildQuery() { return BuildKnobStateQuery(); }

    /// The knob group is a single packed command, so the vector is length one.
    /// Exercising the engine against it proves the diff is not shaped around
    /// multi-command groups.
    [[nodiscard]] static std::vector<Command> BuildControl(const Params& params) {
        return {BuildKnobStateControl(params)};
    }

    [[nodiscard]] static Params Parse(const std::vector<Command>& commands) {
        return commands.empty() ? Params{} : ParseKnobState(commands.front());
    }
};

struct OutputSerdes {
    using Params = OutputParams;
    using Command = ParamsSerdes::Command;

    [[nodiscard]] static std::vector<Command> BuildQuery() { return BuildOutputParamsQuery(); }
    [[nodiscard]] static std::vector<Command> BuildControl(const Params& params) {
        return BuildOutputParamsControl(params);
    }
    [[nodiscard]] static Params Parse(const std::vector<Command>& commands) {
        return ParseOutputParams(commands);
    }
};

struct InputSerdes {
    using Params = InputParams;
    using Command = ParamsSerdes::Command;

    [[nodiscard]] static std::vector<Command> BuildQuery() { return BuildInputParamsQuery(); }
    [[nodiscard]] static std::vector<Command> BuildControl(const Params& params) {
        return BuildInputParamsControl(params);
    }
    [[nodiscard]] static Params Parse(const std::vector<Command>& commands) {
        return ParseInputParams(commands);
    }
};

struct MixerSerdes {
    using Params = MixerParams;
    using Command = ParamsSerdes::Command;

    [[nodiscard]] static std::vector<Command> BuildQuery() { return BuildMixerParamsQuery(); }
    [[nodiscard]] static std::vector<Command> BuildControl(const Params& params) {
        return BuildMixerParamsControl(params);
    }
    [[nodiscard]] static Params Parse(const std::vector<Command>& commands) {
        return ParseMixerParams(commands);
    }
};

struct DisplaySerdes {
    using Params = DisplayParams;
    using Command = ParamsSerdes::Command;

    [[nodiscard]] static std::vector<Command> BuildQuery() { return BuildDisplayParamsQuery(); }
    [[nodiscard]] static std::vector<Command> BuildControl(const Params& params) {
        return BuildDisplayParamsControl(params);
    }
    [[nodiscard]] static Params Parse(const std::vector<Command>& commands) {
        return ParseDisplayParams(commands);
    }
};

} // namespace ASFW::Audio::Oxford::Apogee::ParamsSerdes
