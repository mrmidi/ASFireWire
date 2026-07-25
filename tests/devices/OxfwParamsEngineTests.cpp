// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// OxfwParamsEngineTests.cpp - FW-128 coverage for the OXFW-common params cache
// and diff engine.
//
// The engine's whole job is choosing *which* commands go on the wire, so most of
// these assert on the recorded command list rather than on a status code. The
// property that matters is the one the ticket names: change one field of a group
// and exactly one command is sent.
//
// Genericity is proved the way the reference establishes it - by instantiating
// the same engine over a second, unrelated parameter type (the Tascam FireOne
// shape from tascam.rs:212-237) that shares no types with the Duet.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Audio/Protocols/Oxford/OxfwParamsEngine.hpp"
#include "Audio/Protocols/Oxford/Apogee/ApogeeParamsSerdes.hpp"
#include "Audio/Protocols/Oxford/Apogee/ApogeeTransport.hpp"

#include "AvcTestRig.hpp"

using ASFW::Audio::Oxford::OxfwParamsCache;
using namespace ASFW::Audio::Oxford::Apogee;
namespace Serdes = ASFW::Audio::Oxford::Apogee::ParamsSerdes;

namespace {

//==============================================================================
// A recording dispatch - the engine is transport-agnostic, so its selection
// logic is tested directly rather than through FCP framing.
//==============================================================================

template <typename Command>
struct RecordingDispatch {
    struct Call {
        std::vector<Command> commands;
        bool isStatus{false};
    };

    std::vector<Call> calls;
    IOReturn nextStatus{kIOReturnSuccess};
    std::vector<Command> statusResponses;

    [[nodiscard]] ASFW::Audio::Oxford::OxfwCommandDispatch<Command> Bind() {
        return [this](const std::vector<Command>& commands, bool isStatus,
                      ASFW::Audio::Oxford::OxfwSequenceCallback<Command> callback) {
            calls.push_back(Call{commands, isStatus});
            const IOReturn status = nextStatus;
            // A status query answers with the caller's own commands filled in;
            // a control sequence echoes nothing meaningful.
            const std::vector<Command>& responses =
                (isStatus && !statusResponses.empty()) ? statusResponses : commands;
            if (callback) {
                callback(status, responses);
            }
        };
    }

    [[nodiscard]] size_t CallCount() const noexcept { return calls.size(); }
    [[nodiscard]] const std::vector<Command>& LastCommands() const { return calls.back().commands; }
    [[nodiscard]] size_t LastCommandCount() const { return calls.back().commands.size(); }
    void Clear() noexcept { calls.clear(); }
};

using ApogeeDispatch = RecordingDispatch<ApogeeVendorCommand>;

//==============================================================================
// Establishing the cache
//==============================================================================

TEST(OxfwParamsEngine, CacheIssuesTheQueryAsStatusAndStoresTheResult) {
    ApogeeDispatch dispatch;
    OxfwParamsCache<Serdes::OutputSerdes> cache(dispatch.Bind());

    OutputParams reported{};
    reported.volume = 77;
    reported.mute = true;
    dispatch.statusResponses = Serdes::OutputSerdes::BuildControl(reported);

    IOReturn seenStatus = kIOReturnError;
    OutputParams seen{};
    cache.Cache([&](IOReturn status, const OutputParams& params) {
        seenStatus = status;
        seen = params;
    });

    ASSERT_EQ(dispatch.CallCount(), 1U);
    EXPECT_TRUE(dispatch.calls[0].isStatus);
    EXPECT_EQ(dispatch.LastCommandCount(), Serdes::OutputSerdes::BuildQuery().size());

    EXPECT_EQ(seenStatus, kIOReturnSuccess);
    EXPECT_EQ(seen.volume, 77);
    EXPECT_TRUE(seen.mute);
    EXPECT_TRUE(cache.IsEstablished());
    EXPECT_EQ(cache.Cached().volume, 77);
}

TEST(OxfwParamsEngine, FailedCacheLeavesTheCacheUnestablished) {
    ApogeeDispatch dispatch;
    dispatch.nextStatus = kIOReturnTimeout;
    OxfwParamsCache<Serdes::OutputSerdes> cache(dispatch.Bind());

    IOReturn seenStatus = kIOReturnSuccess;
    cache.Cache([&](IOReturn status, const OutputParams&) { seenStatus = status; });

    EXPECT_EQ(seenStatus, kIOReturnTimeout);
    EXPECT_FALSE(cache.IsEstablished());
}

TEST(OxfwParamsEngine, WithoutDispatchEveryOperationReportsNotReady) {
    OxfwParamsCache<Serdes::OutputSerdes> cache;

    IOReturn cacheStatus = kIOReturnSuccess;
    cache.Cache([&](IOReturn status, const OutputParams&) { cacheStatus = status; });
    EXPECT_EQ(cacheStatus, kIOReturnNotReady);

    IOReturn updateStatus = kIOReturnSuccess;
    cache.Update(OutputParams{}, [&](IOReturn status) { updateStatus = status; });
    EXPECT_EQ(updateStatus, kIOReturnNotReady);
}

//==============================================================================
// The diff - this is the property the ticket exists for
//==============================================================================

/// Establish a cache holding `params` without asserting on the traffic it took.
template <typename SerdesT, typename DispatchT>
void Establish(OxfwParamsCache<SerdesT>& cache,
               DispatchT& dispatch,
               const typename SerdesT::Params& params) {
    dispatch.statusResponses = SerdesT::BuildControl(params);
    cache.Cache([](IOReturn, const typename SerdesT::Params&) {});
    dispatch.statusResponses.clear();
    dispatch.Clear();
}

TEST(OxfwParamsEngine, ChangingOneFieldSendsExactlyOneCommand) {
    ApogeeDispatch dispatch;
    OxfwParamsCache<Serdes::OutputSerdes> cache(dispatch.Bind());

    OutputParams params{};
    params.volume = 40;
    Establish(cache, dispatch, params);

    params.volume = 41;
    IOReturn seenStatus = kIOReturnError;
    cache.Update(params, [&](IOReturn status) { seenStatus = status; });

    EXPECT_EQ(seenStatus, kIOReturnSuccess);
    ASSERT_EQ(dispatch.CallCount(), 1U);
    EXPECT_FALSE(dispatch.calls[0].isStatus);
    ASSERT_EQ(dispatch.LastCommandCount(), 1U);
    EXPECT_EQ(dispatch.LastCommands()[0].code, ApogeeVendorCommand::Code::OutVolume);
    EXPECT_EQ(dispatch.LastCommands()[0].u8Value, 41);
}

TEST(OxfwParamsEngine, NoChangeSendsNothingAtAll) {
    ApogeeDispatch dispatch;
    OxfwParamsCache<Serdes::OutputSerdes> cache(dispatch.Bind());

    OutputParams params{};
    params.volume = 40;
    params.mute = true;
    Establish(cache, dispatch, params);

    IOReturn seenStatus = kIOReturnError;
    cache.Update(params, [&](IOReturn status) { seenStatus = status; });

    EXPECT_EQ(seenStatus, kIOReturnSuccess);
    EXPECT_EQ(dispatch.CallCount(), 0U);
}

TEST(OxfwParamsEngine, ChangingEveryFieldSendsEveryCommandInSerdesOrder) {
    ApogeeDispatch dispatch;
    OxfwParamsCache<Serdes::DisplaySerdes> cache(dispatch.Bind());

    DisplayParams before{};
    before.target = DisplayTarget::Output;
    before.mode = DisplayMode::Independent;
    before.overhold = DisplayOverhold::Infinite;
    Establish(cache, dispatch, before);

    DisplayParams after{};
    after.target = DisplayTarget::Input;
    after.mode = DisplayMode::FollowingToKnobTarget;
    after.overhold = DisplayOverhold::TwoSeconds;
    cache.Update(after, [](IOReturn) {});

    ASSERT_EQ(dispatch.CallCount(), 1U);
    const auto expected = Serdes::DisplaySerdes::BuildControl(after);
    ASSERT_EQ(dispatch.LastCommandCount(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(dispatch.LastCommands()[i], expected[i]) << "position " << i;
    }
}

TEST(OxfwParamsEngine, UnestablishedUpdateWritesTheWholeGroup) {
    // Diffing against a default-constructed struct the hardware never reported
    // would drop every command that happens to match the default.
    ApogeeDispatch dispatch;
    OxfwParamsCache<Serdes::OutputSerdes> cache(dispatch.Bind());

    OutputParams params{};
    params.volume = 3;
    cache.Update(params, [](IOReturn) {});

    ASSERT_EQ(dispatch.CallCount(), 1U);
    EXPECT_EQ(dispatch.LastCommandCount(),
              Serdes::OutputSerdes::BuildControl(params).size());
}

TEST(OxfwParamsEngine, InvalidateForcesTheNextUpdateToWriteEverything) {
    ApogeeDispatch dispatch;
    OxfwParamsCache<Serdes::OutputSerdes> cache(dispatch.Bind());

    OutputParams params{};
    params.volume = 40;
    Establish(cache, dispatch, params);
    EXPECT_TRUE(cache.IsEstablished());

    cache.Invalidate();
    EXPECT_FALSE(cache.IsEstablished());

    params.volume = 41;
    cache.Update(params, [](IOReturn) {});
    ASSERT_EQ(dispatch.CallCount(), 1U);
    EXPECT_EQ(dispatch.LastCommandCount(),
              Serdes::OutputSerdes::BuildControl(params).size());
}

//==============================================================================
// Failure must not advance the cache
//==============================================================================

TEST(OxfwParamsEngine, FailedUpdateLeavesTheCacheUnadvancedSoRetryResends) {
    ApogeeDispatch dispatch;
    OxfwParamsCache<Serdes::OutputSerdes> cache(dispatch.Bind());

    OutputParams params{};
    params.volume = 40;
    Establish(cache, dispatch, params);

    params.volume = 41;
    dispatch.nextStatus = kIOReturnTimeout;

    IOReturn seenStatus = kIOReturnSuccess;
    cache.Update(params, [&](IOReturn status) { seenStatus = status; });
    EXPECT_EQ(seenStatus, kIOReturnTimeout);
    EXPECT_EQ(cache.Cached().volume, 40) << "a failed write must not be recorded as applied";

    // The retry must re-send the failed command rather than diff it away.
    dispatch.nextStatus = kIOReturnSuccess;
    dispatch.Clear();
    cache.Update(params, [&](IOReturn status) { seenStatus = status; });

    EXPECT_EQ(seenStatus, kIOReturnSuccess);
    ASSERT_EQ(dispatch.CallCount(), 1U);
    ASSERT_EQ(dispatch.LastCommandCount(), 1U);
    EXPECT_EQ(dispatch.LastCommands()[0].code, ApogeeVendorCommand::Code::OutVolume);
    EXPECT_EQ(dispatch.LastCommands()[0].u8Value, 41);
    EXPECT_EQ(cache.Cached().volume, 41);
}

//==============================================================================
// The knob group is a single packed command, not a command per field
//==============================================================================

TEST(OxfwParamsEngine, SingleCommandGroupStillDiffs) {
    ApogeeDispatch dispatch;
    OxfwParamsCache<Serdes::KnobSerdes> cache(dispatch.Bind());

    KnobState state{};
    state.outputVolume = 30;
    Establish(cache, dispatch, state);

    cache.Update(state, [](IOReturn) {});
    EXPECT_EQ(dispatch.CallCount(), 0U) << "identical packed state must send nothing";

    state.outputVolume = 31;
    cache.Update(state, [](IOReturn) {});
    ASSERT_EQ(dispatch.CallCount(), 1U);
    EXPECT_EQ(dispatch.LastCommandCount(), 1U);
}

//==============================================================================
// Genericity - a second parameter type sharing nothing with the Duet
//==============================================================================

/// The Tascam FireOne shape (tascam.rs:212-237): a different command type, a
/// different parameter struct, and one command per field.
struct FireOneCommand {
    enum class Code : uint8_t { DisplayMode, MessageMode, InputMode, FirmwareVersion };

    Code code{};
    uint8_t value{0};

    [[nodiscard]] bool operator==(const FireOneCommand&) const = default;
};

struct FireOneParams {
    uint8_t displayMode{0};
    uint8_t messageMode{0};
    uint8_t inputMode{0};
    uint8_t firmwareVersion{0};
};

struct FireOneSerdes {
    using Params = FireOneParams;
    using Command = FireOneCommand;

    [[nodiscard]] static std::vector<Command> BuildQuery() { return BuildControl(Params{}); }

    [[nodiscard]] static std::vector<Command> BuildControl(const Params& params) {
        return {
            Command{FireOneCommand::Code::DisplayMode, params.displayMode},
            Command{FireOneCommand::Code::MessageMode, params.messageMode},
            Command{FireOneCommand::Code::InputMode, params.inputMode},
            Command{FireOneCommand::Code::FirmwareVersion, params.firmwareVersion},
        };
    }

    [[nodiscard]] static Params Parse(const std::vector<Command>& commands) {
        Params params{};
        for (const auto& command : commands) {
            switch (command.code) {
                case FireOneCommand::Code::DisplayMode: params.displayMode = command.value; break;
                case FireOneCommand::Code::MessageMode: params.messageMode = command.value; break;
                case FireOneCommand::Code::InputMode: params.inputMode = command.value; break;
                case FireOneCommand::Code::FirmwareVersion:
                    params.firmwareVersion = command.value;
                    break;
            }
        }
        return params;
    }
};

TEST(OxfwParamsEngine, DiffsASecondUnrelatedParamsTypeIdentically) {
    RecordingDispatch<FireOneCommand> dispatch;
    OxfwParamsCache<FireOneSerdes> cache(dispatch.Bind());

    FireOneParams params{};
    params.displayMode = 1;
    params.messageMode = 2;
    Establish(cache, dispatch, params);

    params.inputMode = 9;
    cache.Update(params, [](IOReturn) {});

    ASSERT_EQ(dispatch.CallCount(), 1U);
    ASSERT_EQ(dispatch.LastCommandCount(), 1U);
    EXPECT_EQ(dispatch.LastCommands()[0].code, FireOneCommand::Code::InputMode);
    EXPECT_EQ(dispatch.LastCommands()[0].value, 9);
}

TEST(OxfwParamsEngine, SecondParamsTypeAlsoHonoursTheFailureContract) {
    RecordingDispatch<FireOneCommand> dispatch;
    OxfwParamsCache<FireOneSerdes> cache(dispatch.Bind());

    FireOneParams params{};
    Establish(cache, dispatch, params);

    params.firmwareVersion = 5;
    dispatch.nextStatus = kIOReturnError;
    cache.Update(params, [](IOReturn) {});
    EXPECT_EQ(cache.Cached().firmwareVersion, 0);

    dispatch.nextStatus = kIOReturnSuccess;
    dispatch.Clear();
    cache.Update(params, [](IOReturn) {});
    ASSERT_EQ(dispatch.CallCount(), 1U);
    EXPECT_EQ(dispatch.LastCommands()[0].code, FireOneCommand::Code::FirmwareVersion);
}

//==============================================================================
// Over the real FCP transport, via the shared AV/C rig (FW-138)
//==============================================================================

TEST(OxfwParamsEngine, DiffReachesTheWireThroughTheRealFcpTransport) {
    ASFW::Testing::AvcTestRig rig;
    ASSERT_TRUE(rig.IsReady());

    OxfwParamsCache<Serdes::OutputSerdes> cache(
        [&rig](const std::vector<ApogeeVendorCommand>& commands, bool isStatus,
               ASFW::Audio::Oxford::OxfwSequenceCallback<ApogeeVendorCommand> callback) {
            VendorFcp::ExecuteSequence(rig.Transport(), commands, isStatus, std::move(callback));
        });

    // No cache yet, so this writes the whole group and establishes it.
    OutputParams params{};
    params.volume = 20;
    IOReturn seeded = kIOReturnError;
    cache.Update(params, [&](IOReturn status) { seeded = status; });
    rig.Drain();

    ASSERT_EQ(seeded, kIOReturnSuccess);
    const size_t fullGroup = Serdes::OutputSerdes::BuildControl(params).size();
    EXPECT_EQ(rig.Target().CommandCount(), fullGroup);

    // One field changes, so exactly one FCP command must reach the target.
    rig.Target().ClearCommands();
    params.volume = 21;
    IOReturn updated = kIOReturnError;
    cache.Update(params, [&](IOReturn status) { updated = status; });
    rig.Drain();

    EXPECT_EQ(updated, kIOReturnSuccess);
    EXPECT_EQ(rig.Target().CommandCount(), 1U);
    EXPECT_EQ(cache.Cached().volume, 21);
}

} // namespace
