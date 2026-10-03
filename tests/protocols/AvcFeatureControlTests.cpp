// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "ASFWDriver/Audio/Protocols/AVC/AvcFeatureControl.hpp"
#include <limits>
using namespace ASFW;
namespace {
using Audio::Model::AvcVolumeRange;
using Protocols::AVC::Graph::FeatureChannelState;
class FeatureUnit : public AVC::IAvcUnit {
public:
    FW::NodeId NodeId() const noexcept override { return FW::NodeId{1}; }
    FW::Generation CurrentGeneration() const noexcept override { return FW::Generation{generation}; }
    uint64_t Guid() const noexcept override { return 42; }
    uint32_t generation{1};
    bool reject{}, badReadback{}, resetAfterControl{}, defer{}, transition{};
    std::vector<AVC::CommandFrame> commands;
    int16_t volume{};
    bool mute{};
    std::function<void()> pending;
    void Submit(const AVC::CommandFrame& frame, FW::Generation, ResponseCallback completion) override {
        commands.push_back(frame);
        const auto op = frame.Operands();
        std::vector<uint8_t> operands(op.begin(), op.end());
        auto code = AVC::ResponseCode::kImplementedStable;
        if (frame.Type() == AVC::CommandType::kControl) {
            if (reject) code = AVC::ResponseCode::kRejected;
            else {
                code = AVC::ResponseCode::kAccepted;
                if (op[5] == 1) mute = op[7] == AVC::Cmd::kBooleanTrue;
                else volume = static_cast<int16_t>((op[7] << 8) | op[8]);
            }
            if (resetAfterControl) ++generation;
        } else {
            if (op[5] == 1) operands[7] = mute ? AVC::Cmd::kBooleanTrue : AVC::Cmd::kBooleanFalse;
            else { operands[7] = static_cast<uint8_t>(static_cast<uint16_t>(volume) >> 8); operands[8] = static_cast<uint8_t>(volume); }
            if (badReadback) operands[4] = 9;
            if (transition) code = AVC::ResponseCode::kInTransition;
        }
        auto deliver = [frame, operands, code, completion] {
            completion(AVC::Response{code, frame.Address(), frame.OpcodeValue(), operands});
        };
        if (defer) pending = std::move(deliver); else deliver();
    }
};
FeatureChannelState Channel() {
    return {.subunit = 0, .block = 1, .channel = 0, .mute = false,
            .volume = 0, .minimum = -16384, .maximum = 0, .resolution = 256};
}
TEST(AvcVolumeMapping, ClampsAndRoundsOnTheDeviceGrid) {
    const AvcVolumeRange range{-16384, 0, 256};
    EXPECT_EQ(range.Quantize(-10.4f), -2560);
    EXPECT_EQ(range.Quantize(-10.6f), -2816);
    EXPECT_EQ(range.Quantize(-1000), -16384);
    EXPECT_EQ(range.Quantize(10), 0);
    EXPECT_EQ(AvcVolumeRange::Decibels(-16384), -64);
    EXPECT_FALSE(range.Quantize(std::numeric_limits<float>::quiet_NaN()));
    EXPECT_FALSE(range.Quantize(std::numeric_limits<float>::infinity()));
}
TEST(AvcVolumeMapping, FractionalStepsAndNonDivisibleMaximum) {
    const AvcVolumeRange fine{-16384, 0, 1};
    EXPECT_EQ(fine.Quantize(-0.00390625f), -1);
    const AvcVolumeRange grid{-100, 100, 64};
    EXPECT_EQ(grid.Quantize(100), 92); // highest valid grid point, not an invented endpoint
    for (int raw = -100; raw <= 92; raw += 64)
        EXPECT_EQ(grid.Quantize(AvcVolumeRange::Decibels(static_cast<int16_t>(raw))), raw);
}
TEST(AvcVolumeMapping, RejectsUnknownOrInvalidRanges) {
    EXPECT_FALSE((AvcVolumeRange{0, -1, 1}).Valid());
    EXPECT_FALSE((AvcVolumeRange{0, 0, 1}).Valid());
    EXPECT_FALSE((AvcVolumeRange{-1, 0, 2}).Valid());
    EXPECT_FALSE((AvcVolumeRange{-1, 0, 0}).Valid());
    EXPECT_FALSE((AvcVolumeRange{INT16_MIN, 0, 1}).Valid());
    EXPECT_FALSE((AvcVolumeRange{-1, INT16_MAX, 1}).Valid());
}
TEST(AvcFeatureControl, WritesQuantizedDbThenReadsCurrent) {
    auto unit = std::make_shared<FeatureUnit>();
    bool completed{};
    Audio::SetAvcFeature(unit, *unit->CurrentRoute(), Channel(), false, -2680, [&](auto reply) {
        ASSERT_TRUE(reply); EXPECT_EQ(reply->AsVolume().Raw(), -2560); completed = true;
    });
    EXPECT_TRUE(completed); ASSERT_EQ(unit->commands.size(), 2);
    EXPECT_EQ(unit->commands[0].Type(), AVC::CommandType::kControl);
    EXPECT_EQ(unit->commands[1].Type(), AVC::CommandType::kStatus);
}
TEST(AvcFeatureControl, MuteUsesTheBooleanWireEncoding) {
    auto unit = std::make_shared<FeatureUnit>();
    Audio::SetAvcFeature(unit, *unit->CurrentRoute(), Channel(), true, 1, [](auto reply) {
        ASSERT_TRUE(reply); EXPECT_TRUE(reply->AsMute());
    });
    ASSERT_EQ(unit->commands.size(), 2); EXPECT_EQ(unit->commands[0].Operands()[7], 0x70);
}
TEST(AvcFeatureControl, RejectedControlDoesNotReadOrReportSuccess) {
    auto unit = std::make_shared<FeatureUnit>(); unit->reject = true;
    Audio::SetAvcFeature(unit, *unit->CurrentRoute(), Channel(), false, -256, [](auto reply) { EXPECT_FALSE(reply); });
    EXPECT_EQ(unit->commands.size(), 1);
}
TEST(AvcFeatureControl, ResetAndMalformedOrTransitionReadbackAreNotConfirmed) {
    for (int fault = 0; fault < 3; ++fault) {
        auto unit = std::make_shared<FeatureUnit>();
        unit->resetAfterControl = fault == 0; unit->badReadback = fault == 1; unit->transition = fault == 2;
        Audio::SetAvcFeature(unit, *unit->CurrentRoute(), Channel(), false, -256, [](auto reply) { EXPECT_FALSE(reply); });
    }
}
TEST(AvcFeatureControl, MissingLimitsAndExpiredOperationsSendNoFurtherTraffic) {
    auto unit = std::make_shared<FeatureUnit>(); auto channel = Channel(); channel.resolution.reset();
    Audio::SetAvcFeature(unit, *unit->CurrentRoute(), channel, false, -256, [](auto reply) { EXPECT_FALSE(reply); });
    EXPECT_TRUE(unit->commands.empty());
    unit->defer = true; bool active = true;
    Audio::SetAvcFeature(unit, *unit->CurrentRoute(), Channel(), false, -256,
        [](auto reply) { EXPECT_FALSE(reply); }, [&] { return active; });
    active = false; unit->pending(); EXPECT_EQ(unit->commands.size(), 1);
}
} // namespace
