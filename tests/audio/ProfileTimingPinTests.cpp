// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Pins every device profile's timing declarations at every AMDTP rate, as they
// were before the FW-177 timing-geometry migration (commit d5d65c8). The
// migration moves where these values are derived (one ladder helper, one
// resolver) and must not change a single one of them -- except where a row
// below is deliberately updated with its reason.
//
// Regenerate the table after an intentional change:
//   ASFW_DUMP_PROFILE_TIMING=1 ./ProfileTimingPinTests --gtest_filter='*Pinned*'

#include "Audio/DriverKit/Config/AudioProfileRegistry.hpp"
#include "DeviceProfiles/Audio/AudioDeviceCatalog.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using ASFW::DeviceProfiles::Audio::ProfileBuilderId;
using ASFW::Isoch::Audio::AudioProfileRegistry;

// Per-GUID BeBoB profiles (RegisterBeBoBProfile) need a discovery model and
// are not pinned here; BeBoBProfile's constants are 64/64/128/128 at all rates.
constexpr std::array<uint32_t, 7> kRates = {32000, 44100, 48000, 88200, 96000, 176400, 192000};

struct PinnedTiming {
    uint32_t builder;
    uint32_t rate;
    uint32_t txLatency;
    uint32_t rxLatency;
    uint32_t txSafety;
    uint32_t rxSafety;
};

// clang-format off
constexpr PinnedTiming kPinned[] = {
#include "ProfileTimingPinTable.inc"
};
// clang-format on

TEST(ProfileTimingPinTests, PinnedDeclarationsAtEveryRate) {
    const bool dump = std::getenv("ASFW_DUMP_PROFILE_TIMING") != nullptr;
    size_t checked = 0;
    // Builder 0 is the generic DICE fallback FindProfile returns for an
    // unidentified device; every other row is a catalog builder.
    for (uint32_t builder = 0; builder <= static_cast<uint32_t>(ProfileBuilderId::kLastValid);
         ++builder) {
        const auto* profile = builder == 0
                                  ? AudioProfileRegistry::FindProfile(0, 0, 0, 0)
                                  : AudioProfileRegistry::ProfileForBuilderId(builder);
        if (profile == nullptr) {
            continue;
        }
        for (const uint32_t rate : kRates) {
            const double r = rate;
            const PinnedTiming now{builder,
                                   rate,
                                   profile->TxReportedLatencyFrames(r),
                                   profile->RxReportedLatencyFrames(r),
                                   profile->TxSafetyOffsetFrames(r),
                                   profile->RxSafetyOffsetFrames(r)};
            if (dump) {
                std::printf("    {%u, %u, %u, %u, %u, %u},  // %s\n", now.builder, now.rate,
                            now.txLatency, now.rxLatency, now.txSafety, now.rxSafety,
                            profile->Name());
                continue;
            }
            const PinnedTiming* pinned = nullptr;
            for (const auto& row : kPinned) {
                if (row.builder == builder && row.rate == rate) {
                    pinned = &row;
                }
            }
            ASSERT_NE(pinned, nullptr) << "no pinned row for builder " << builder << " @" << rate;
            SCOPED_TRACE(std::string(profile->Name()) + " @" + std::to_string(rate));
            EXPECT_EQ(now.txLatency, pinned->txLatency);
            EXPECT_EQ(now.rxLatency, pinned->rxLatency);
            EXPECT_EQ(now.txSafety, pinned->txSafety);
            EXPECT_EQ(now.rxSafety, pinned->rxSafety);
            ++checked;
        }
    }
    if (!dump) {
        EXPECT_EQ(checked, sizeof(kPinned) / sizeof(kPinned[0])) << "a profile disappeared or a row is stale";
    }
}

} // namespace
