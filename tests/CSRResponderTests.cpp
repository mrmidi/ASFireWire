// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// CSRResponderTests.cpp — FW-19 software CSR responder surface.

#include "Bus/CSR/CSRResponder.hpp"
#include "Common/CSRSpace.hpp"

#include <gtest/gtest.h>

namespace {

using ASFW::Async::ResponseCode;
using ASFW::Bus::CSRResponder;
using ASFW::Bus::ICycleMasterControl;
using ASFW::Bus::IRootStatus;
using ASFW::Bus::ITopologyMapProvider;
namespace FW = ASFW::FW;

struct FakeRoot : IRootStatus {
    bool root{false};
    [[nodiscard]] bool IsLocalRoot() const noexcept override { return root; }
};

struct FakeCycleMaster : ICycleMasterControl {
    bool enabled{false};
    int setCalls{0};
    void SetCycleMaster(bool enable) noexcept override {
        enabled = enable;
        ++setCalls;
    }
    [[nodiscard]] bool IsCycleMasterEnabled() const noexcept override { return enabled; }
};

struct FakeTopologyMap : ITopologyMapProvider {
    uint32_t value{0xABCDEF01};
    bool valid{true};
    [[nodiscard]] bool ReadQuadlet(uint32_t, uint32_t& out) const noexcept override {
        if (!valid) return false;
        out = value;
        return true;
    }
};

CSRResponder Make(FakeRoot& r, FakeCycleMaster& cm) {
    return CSRResponder(CSRResponder::Deps{.root = &r, .cycleMaster = &cm, .topologyMap = nullptr});
}

// ---- STATE_SET cycle master ---------------------------------------------------

TEST(CSRResponder, StateSetCmstr_AsRoot_EnablesCycleMaster) {
    FakeRoot r;
    r.root = true;
    FakeCycleMaster cm;
    auto rsp = Make(r, cm);

    const auto res = rsp.WriteQuadlet(FW::kCSR_StateSet, FW::kCSRStateBitCMSTR);
    EXPECT_TRUE(res.mine);
    EXPECT_EQ(res.rcode, ResponseCode::Complete);
    EXPECT_TRUE(cm.enabled);
    EXPECT_EQ(cm.setCalls, 1);
}

TEST(CSRResponder, StateSetCmstr_NotRoot_DiscardsButCompletes) {
    FakeRoot r;
    r.root = false;
    FakeCycleMaster cm;
    auto rsp = Make(r, cm);

    const auto res = rsp.WriteQuadlet(FW::kCSR_StateSet, FW::kCSRStateBitCMSTR);
    EXPECT_TRUE(res.mine);
    EXPECT_EQ(res.rcode, ResponseCode::Complete); // inert success, not an error
    EXPECT_FALSE(cm.enabled);
    EXPECT_EQ(cm.setCalls, 0);
}

TEST(CSRResponder, StateClearCmstr_AsRoot_DisablesCycleMaster) {
    FakeRoot r;
    r.root = true;
    FakeCycleMaster cm;
    cm.enabled = true;
    auto rsp = Make(r, cm);

    const auto res = rsp.WriteQuadlet(FW::kCSR_StateClear, FW::kCSRStateBitCMSTR);
    EXPECT_EQ(res.rcode, ResponseCode::Complete);
    EXPECT_FALSE(cm.enabled);
}

// ---- Abdicate -----------------------------------------------------------------

TEST(CSRResponder, AbdicateSetViaStateSet_ClearViaStateClear) {
    FakeRoot r;
    FakeCycleMaster cm;
    auto rsp = Make(r, cm);

    rsp.WriteQuadlet(FW::kCSR_StateSet, FW::kCSRStateBitABDICATE);
    EXPECT_TRUE(rsp.Abdicate());

    rsp.WriteQuadlet(FW::kCSR_StateClear, FW::kCSRStateBitABDICATE);
    EXPECT_FALSE(rsp.Abdicate());
}

TEST(CSRResponder, AbdicateConsumeIsOneShot) {
    FakeRoot r;
    FakeCycleMaster cm;
    auto rsp = Make(r, cm);

    rsp.WriteQuadlet(FW::kCSR_StateSet, FW::kCSRStateBitABDICATE);
    EXPECT_TRUE(rsp.ConsumeAbdicate());  // returns prior value
    EXPECT_FALSE(rsp.Abdicate());        // cleared
    EXPECT_FALSE(rsp.ConsumeAbdicate()); // already consumed
}

TEST(CSRResponder, ResetStartWrite_ClearsAbdicate_ReadIsTypeError) {
    FakeRoot r;
    FakeCycleMaster cm;
    auto rsp = Make(r, cm);

    rsp.WriteQuadlet(FW::kCSR_StateSet, FW::kCSRStateBitABDICATE);
    const auto w = rsp.WriteQuadlet(FW::kCSR_ResetStart, 0);
    EXPECT_EQ(w.rcode, ResponseCode::Complete);
    EXPECT_FALSE(rsp.Abdicate());

    const auto rd = rsp.ReadQuadlet(FW::kCSR_ResetStart);
    EXPECT_TRUE(rd.mine);
    EXPECT_EQ(rd.rcode, ResponseCode::TypeError);
}

// ---- STATE read ---------------------------------------------------------------

TEST(CSRResponder, StateRead_ReflectsCycleMasterAndAbdicate) {
    FakeRoot r;
    r.root = true;
    FakeCycleMaster cm;
    cm.enabled = true;
    auto rsp = Make(r, cm);
    rsp.WriteQuadlet(FW::kCSR_StateSet, FW::kCSRStateBitABDICATE);

    const auto res = rsp.ReadQuadlet(FW::kCSR_StateSet);
    EXPECT_TRUE(res.mine);
    EXPECT_EQ(res.rcode, ResponseCode::Complete);
    EXPECT_EQ(res.readValue, FW::kCSRStateBitCMSTR | FW::kCSRStateBitABDICATE);
}

TEST(CSRResponder, StateRead_NotRoot_NoCmstrBit) {
    FakeRoot r;
    r.root = false;
    FakeCycleMaster cm;
    cm.enabled = true;
    auto rsp = Make(r, cm);

    const auto res = rsp.ReadQuadlet(FW::kCSR_StateClear);
    EXPECT_EQ(res.readValue, 0u); // cmstr only reported when root
}

// ---- Broadcast channel --------------------------------------------------------

TEST(CSRResponder, BroadcastChannel_InitialValue) {
    FakeRoot r;
    FakeCycleMaster cm;
    auto rsp = Make(r, cm);

    const auto res = rsp.ReadQuadlet(FW::kCSR_BroadcastChannel);
    EXPECT_EQ(res.rcode, ResponseCode::Complete);
    EXPECT_EQ(res.readValue, FW::kBroadcastChannelInitial); // 0x8000001F
}

TEST(CSRResponder, BroadcastChannel_WriteMasksToValidOrInitial) {
    FakeRoot r;
    FakeCycleMaster cm;
    auto rsp = Make(r, cm);

    // Writing all-ones should keep only the VALID bit plus the pinned INITIAL.
    rsp.WriteQuadlet(FW::kCSR_BroadcastChannel, 0xFFFFFFFFu);
    EXPECT_EQ(rsp.BroadcastChannel(), FW::kBroadcastChannelValid | FW::kBroadcastChannelInitial);
    EXPECT_EQ(rsp.BroadcastChannel(), 0xC000001Fu);

    // Writing zero drops VALID, keeps INITIAL.
    rsp.WriteQuadlet(FW::kCSR_BroadcastChannel, 0u);
    EXPECT_EQ(rsp.BroadcastChannel(), FW::kBroadcastChannelInitial);
}

// ---- IRM resource CSRs are OHCI-served (never ours) ----------------------------

TEST(CSRResponder, IrmResourceCsrs_AreNotMine) {
    FakeRoot r;
    FakeCycleMaster cm;
    auto rsp = Make(r, cm);

    for (uint32_t off : {FW::kCSR_BusManagerID, FW::kCSR_BandwidthAvailable,
                         FW::kCSR_ChannelsAvailableHi, FW::kCSR_ChannelsAvailableLo}) {
        EXPECT_FALSE(rsp.ReadQuadlet(off).mine) << "read off=" << std::hex << off;
        EXPECT_FALSE(rsp.WriteQuadlet(off, 0x3F).mine) << "write off=" << std::hex << off;
    }
}

// ---- Topology map region ------------------------------------------------------

TEST(CSRResponder, TopologyMap_NoProvider_DeclinesReads) {
    FakeRoot r;
    FakeCycleMaster cm;
    auto rsp = Make(r, cm); // topologyMap == nullptr
    EXPECT_FALSE(rsp.ReadQuadlet(FW::kCSR_TopologyMapBase).mine);
    EXPECT_FALSE(rsp.BlockReadClaim(FW::kCSR_TopologyMapBase, 64).mine);
}

TEST(CSRResponder, TopologyMap_WithProvider_BlockReadClaimsAddressError) {
    FakeRoot r;
    FakeCycleMaster cm;
    FakeTopologyMap topo;
    CSRResponder rsp(CSRResponder::Deps{.root = &r, .cycleMaster = &cm, .topologyMap = &topo});

    const auto res = rsp.BlockReadClaim(FW::kCSR_TopologyMapBase, 64);
    EXPECT_TRUE(res.mine);
    EXPECT_EQ(res.rcode, ResponseCode::AddressError);
}

TEST(CSRResponder, TopologyMap_WithProvider_ServesQuadlet) {
    FakeRoot r;
    FakeCycleMaster cm;
    FakeTopologyMap topo;
    CSRResponder rsp(CSRResponder::Deps{.root = &r, .cycleMaster = &cm, .topologyMap = &topo});

    const auto res = rsp.ReadQuadlet(FW::kCSR_TopologyMapBase + 8);
    EXPECT_TRUE(res.mine);
    EXPECT_EQ(res.rcode, ResponseCode::Complete);
    EXPECT_EQ(res.readValue, 0xABCDEF01u);
}

TEST(CSRResponder, TopologyMap_IsReadOnly) {
    FakeRoot r;
    FakeCycleMaster cm;
    FakeTopologyMap topo;
    CSRResponder rsp(CSRResponder::Deps{.root = &r, .cycleMaster = &cm, .topologyMap = &topo});

    const auto res = rsp.WriteQuadlet(FW::kCSR_TopologyMapBase, 0x1234);
    EXPECT_TRUE(res.mine);
    EXPECT_EQ(res.rcode, ResponseCode::TypeError);
}

TEST(CSRResponder, TopologyMap_MisalignedRead_AddressError) {
    FakeRoot r;
    FakeCycleMaster cm;
    FakeTopologyMap topo;
    CSRResponder rsp(CSRResponder::Deps{.root = &r, .cycleMaster = &cm, .topologyMap = &topo});

    const auto res = rsp.ReadQuadlet(FW::kCSR_TopologyMapBase + 2); // not quad-aligned
    EXPECT_TRUE(res.mine);
    EXPECT_EQ(res.rcode, ResponseCode::AddressError);
}

// ---- Unknown offsets fall through --------------------------------------------

TEST(CSRResponder, UnknownCsrOffset_IsNotMine) {
    FakeRoot r;
    FakeCycleMaster cm;
    auto rsp = Make(r, cm);
    EXPECT_FALSE(rsp.ReadQuadlet(FW::kCSR_NodeIDs).mine);  // not handled here
    EXPECT_FALSE(rsp.WriteQuadlet(0xF0009999u, 0).mine);
}

} // namespace
