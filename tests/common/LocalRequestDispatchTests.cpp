// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// LocalRequestDispatchTests.cpp — FW-19 central inbound-request routing.
// Validates participant priority, NotMine fallthrough, and that one protocol no
// longer clobbers another (the bug the central dispatch fixes).

#include "Async/Rx/LocalRequestDispatch.hpp"
#include "Async/PacketHelpers.hpp"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace {

using ASFW::Async::ILocalAddressHandler;
using ASFW::Async::LocalRequestContext;
using ASFW::Async::LocalRequestDispatch;
using ASFW::Async::LocalRequestResult;
using ASFW::Async::ResponseCode;

// Records calls and claims a fixed address; declines everything else.
struct FakeHandler : ILocalAddressHandler {
    std::string name;
    uint64_t ownedOffset{0};
    bool claim{true};
    std::vector<std::string>* log{nullptr};

    [[nodiscard]] const char* Name() const noexcept override { return name.c_str(); }

    [[nodiscard]] LocalRequestResult HandleLocalRequest(const LocalRequestContext& ctx) override {
        if (log) log->push_back(name);
        if (claim && ctx.destOffset == ownedOffset) {
            return LocalRequestResult::Quadlet(ResponseCode::Complete, 0xD00D0000u | ctx.tCode);
        }
        return LocalRequestResult::NotMine();
    }
};

std::unique_ptr<FakeHandler> Make(const std::string& n, uint64_t off, std::vector<std::string>* log) {
    auto h = std::make_unique<FakeHandler>();
    h->name = n;
    h->ownedOffset = off;
    h->log = log;
    return h;
}

LocalRequestContext Ctx(uint64_t off, uint8_t tcode = 0x4) {
    LocalRequestContext c{};
    c.destOffset = off;
    c.tCode = tcode;
    return c;
}

TEST(LocalRequestDispatch, NoHandlers_NotMine) {
    LocalRequestDispatch d;
    EXPECT_FALSE(d.Route(Ctx(0x1234)).claimed);
}

TEST(LocalRequestDispatch, FirstMatchingHandlerWins) {
    std::vector<std::string> log;
    LocalRequestDispatch d;
    d.AddHandler(Make("CSR", 0xAAAA, &log));
    d.AddHandler(Make("SBP2", 0xBBBB, &log));

    const auto res = d.Route(Ctx(0xBBBB));
    EXPECT_TRUE(res.claimed);
    // Both consulted in order; CSR first (declines), then SBP2 (claims).
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0], "CSR");
    EXPECT_EQ(log[1], "SBP2");
}

TEST(LocalRequestDispatch, NonMatchingAllDecline_NotMine) {
    LocalRequestDispatch d;
    d.AddHandler(Make("CSR", 0xAAAA, nullptr));
    d.AddHandler(Make("SBP2", 0xBBBB, nullptr));
    EXPECT_FALSE(d.Route(Ctx(0xCCCC)).claimed);
}

TEST(LocalRequestDispatch, EarlierHandlerClaims_LaterNotConsulted) {
    std::vector<std::string> log;
    LocalRequestDispatch d;
    d.AddHandler(Make("CSR", 0xAAAA, &log));
    d.AddHandler(Make("SBP2", 0xAAAA, &log)); // same offset, lower priority

    const auto res = d.Route(Ctx(0xAAAA));
    EXPECT_TRUE(res.claimed);
    ASSERT_EQ(log.size(), 1u); // SBP2 never consulted
    EXPECT_EQ(log[0], "CSR");
}

// Regression: DICE and SBP-2 used to collide on tCode 0x0 (SBP-2 clobbered DICE).
// With the central dispatch both coexist; each claims only its own address.
TEST(LocalRequestDispatch, DiceAndSbp2Coexist_NoClobber) {
    constexpr uint64_t kDiceAddr = 0xD1CE0000ull;
    constexpr uint64_t kSbp2Addr = 0x5B920000ull;

    LocalRequestDispatch d;
    d.AddHandler(Make("DICE", kDiceAddr, nullptr));
    d.AddHandler(Make("SBP2", kSbp2Addr, nullptr));

    // A quadlet write to the DICE address is claimed by DICE, not swallowed by SBP-2.
    EXPECT_TRUE(d.Route(Ctx(kDiceAddr, 0x0)).claimed);
    // A quadlet write to the SBP-2 address is still claimed by SBP-2.
    EXPECT_TRUE(d.Route(Ctx(kSbp2Addr, 0x0)).claimed);
}

TEST(LocalRequestDispatch, HandlerCountTracksRegistrations) {
    LocalRequestDispatch d;
    EXPECT_EQ(d.HandlerCount(), 0u);
    d.AddHandler(Make("A", 1, nullptr));
    d.AddHandler(Make("B", 2, nullptr));
    EXPECT_EQ(d.HandlerCount(), 2u);
    d.AddHandler(nullptr); // ignored
    EXPECT_EQ(d.HandlerCount(), 2u);
}

TEST(LocalRequestDispatch, LockHeaderFieldsUseQ3LittleEndianLayout) {
    std::array<uint8_t, 16> header{};
    header[12] = 0x02; // extended_tcode low byte
    header[13] = 0x00;
    header[14] = 0x08; // data_length low byte
    header[15] = 0x00;

    EXPECT_EQ(ASFW::Async::ExtractExtendedTCode(header), 0x0002u);
    EXPECT_EQ(ASFW::Async::ExtractDataLength(header), 8u);
}

} // namespace
