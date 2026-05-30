// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// CapabilityModeTests.cpp — FW-22 role-mode capability advertisement.

#include "Common/CSRSpace.hpp"

#include <gtest/gtest.h>

namespace {

using ASFW::FW::DecodeBusOptions;
using ASFW::FW::IsLegalCapabilityCombo;
using ASFW::FW::NormalizeLocalBusOptions;
using ASFW::FW::RoleMode;
namespace BO = ASFW::FW::BusOptionsFields;

// A realistic OHCI hardware BusOptions value: bmc=1, irmc=1, cmc=1, isc=1,
// plus numeric fields and a reserved bit set, to prove preservation.
constexpr uint32_t kHwBusOptions =
    BO::kIRMCMask | BO::kCMCMask | BO::kISCMask | BO::kBMCMask |
    (0x05u << BO::kCycClkAccShift) | (0x08u << BO::kMaxRecShift) |
    (0x02u << BO::kMaxROMShift) | BO::kReserved3Mask | 0x02u /*link_spd*/;

TEST(CapabilityMode, LegacyBmcCleared_ForcesBmcZero_PreservesRest) {
    const uint32_t out = NormalizeLocalBusOptions(kHwBusOptions, RoleMode::LegacyBmcCleared);
    const auto d = DecodeBusOptions(out);
    EXPECT_FALSE(d.bmc);
    EXPECT_TRUE(d.irmc); // preserved from hardware
    EXPECT_TRUE(d.cmc);
    EXPECT_TRUE(d.isc);
    // Everything except the bmc bit is byte-for-byte preserved.
    EXPECT_EQ(out, kHwBusOptions & ~BO::kBMCMask);
}

TEST(CapabilityMode, AppleAvoidManager_ForcesBmcAndIrmcZero_PreservesRest) {
    const uint32_t out = NormalizeLocalBusOptions(kHwBusOptions, RoleMode::AppleAvoidManager);
    const auto d = DecodeBusOptions(out);
    EXPECT_FALSE(d.bmc);
    EXPECT_FALSE(d.irmc); // cleared to avoid being manager/IRM
    EXPECT_TRUE(d.cmc);
    EXPECT_TRUE(d.isc);
    // Both bmc and irmc bits are cleared.
    EXPECT_EQ(out, kHwBusOptions & ~(BO::kBMCMask | BO::kIRMCMask));
}

TEST(CapabilityMode, LegacyBmcCleared_MatchesLegacyOneArgOverload) {
    EXPECT_EQ(NormalizeLocalBusOptions(kHwBusOptions),
              NormalizeLocalBusOptions(kHwBusOptions, RoleMode::LegacyBmcCleared));
}

TEST(CapabilityMode, IRMServerOnly_SetsIrmcClearsBmc) {
    const uint32_t out = NormalizeLocalBusOptions(0u, RoleMode::IRMServerOnly);
    const auto d = DecodeBusOptions(out);
    EXPECT_TRUE(d.irmc);
    EXPECT_FALSE(d.bmc);
    EXPECT_TRUE(IsLegalCapabilityCombo(out));
}

TEST(CapabilityMode, FullBusManager_SetsBmcAndIrmc) {
    // bmc=1 now requires explicitly opting in to ElectionOnly-or-higher; the
    // 2-arg default is ObserveOnly (covered by FullBusManagerObserveOnly_*).
    const uint32_t out = NormalizeLocalBusOptions(0u, RoleMode::FullBusManager,
                                                  ASFW::FW::FullBMActivityLevel::ElectionOnly);
    const auto d = DecodeBusOptions(out);
    EXPECT_TRUE(d.bmc);
    EXPECT_TRUE(d.irmc); // bmc implies irmc
    EXPECT_TRUE(IsLegalCapabilityCombo(out));
}

TEST(CapabilityMode, FullBusManagerObserveOnly_ClearsBmcSetsIrmc) {
    const uint32_t out = NormalizeLocalBusOptions(0u, RoleMode::FullBusManager, ASFW::FW::FullBMActivityLevel::ObserveOnly);
    const auto d = DecodeBusOptions(out);
    EXPECT_FALSE(d.bmc);
    EXPECT_TRUE(d.irmc);
    EXPECT_TRUE(IsLegalCapabilityCombo(out));
}

TEST(CapabilityMode, ReservedAndNumericBitsPreservedInEveryMode) {
    for (auto mode : {RoleMode::LegacyBmcCleared, RoleMode::AppleAvoidManager,
                      RoleMode::IRMServerOnly, RoleMode::FullBusManager}) {
        const uint32_t out = NormalizeLocalBusOptions(kHwBusOptions, mode);
        const auto d = DecodeBusOptions(out);
        EXPECT_EQ(d.cycClkAcc, 0x05u);
        EXPECT_EQ(d.maxRec, 0x08u);
        EXPECT_EQ(d.maxRom, 0x02u);
        EXPECT_EQ(d.linkSpd, 0x02u);
        EXPECT_EQ(out & BO::kReserved3Mask, BO::kReserved3Mask); // reserved bit kept
    }
}

TEST(CapabilityMode, IllegalCombo_BmcWithoutIrmc_IsDetected) {
    EXPECT_FALSE(IsLegalCapabilityCombo(BO::kBMCMask));            // bmc=1, irmc=0
    EXPECT_TRUE(IsLegalCapabilityCombo(BO::kBMCMask | BO::kIRMCMask));
    EXPECT_TRUE(IsLegalCapabilityCombo(0u));
    EXPECT_TRUE(IsLegalCapabilityCombo(BO::kIRMCMask));            // irmc alone is fine
}

TEST(CapabilityMode, GenerationUpdatePreservesCapabilityBits) {
    const uint32_t advertised = NormalizeLocalBusOptions(kHwBusOptions, RoleMode::IRMServerOnly);
    const uint32_t regen = ASFW::FW::SetGeneration({.busOptionsHost = advertised, .gen4 = 0xA});
    const auto d = DecodeBusOptions(regen);
    EXPECT_EQ(d.generation, 0xAu);
    EXPECT_TRUE(d.irmc);
    EXPECT_FALSE(d.bmc);
    // Only the generation nibble changed.
    EXPECT_EQ(regen & ~BO::kGenerationMask, advertised & ~BO::kGenerationMask);
}

} // namespace
