// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AT completion scan against the controller behaviour the OHCI 1.2 draft
// specifies, cross-checked with Apple and Linux:
//
// - Status is written only into the packet's OUTPUT_LAST* descriptor
//   (OHCI §7.1.5.2; Table 7-2 gives OUTPUT_MORE-Immediate no xferStatus).
//   Apple AppleFWOHCI_AsyncTransmit::checkForCompletedElements reads block
//   count-1; Linux handle_at_packet reads `last` (ohci.c:1550).
// - An idle running context leaves CommandPtr on the block holding the Z=0
//   that stopped it, i.e. the packet that just finished (OHCI Table 3-4).
// - A pipelining context may retire packets out of order and leaves
//   CommandPtr on the furthest fetched block (OHCI §7.7). A zero-status head
//   is still in flight; Linux stops and waits there (ohci.c:1550-1552).

#include <cstring>
#include <vector>
#include <gtest/gtest.h>

#include "ASFWDriver/Async/Contexts/ATRequestContext.hpp"
#include "ASFWDriver/Async/Engine/ATManager.hpp"
#include "ASFWDriver/Async/Tx/DescriptorBuilder.hpp"
#include "ASFWDriver/Hardware/HardwareInterface.hpp"
#include "ASFWDriver/Hardware/OHCIConstants.hpp"
#include "ASFWDriver/Shared/Memory/DMAMemoryManager.hpp"
#include "ASFWDriver/Shared/Rings/DescriptorRing.hpp"
#include "ASFWDriver/Testing/FakeDMAMemory.hpp"

namespace ASFW::Testing {
namespace {

using ASFW::Async::ATRequestContext;
using ASFW::Async::ATRequestTag;
using ASFW::Async::DescriptorBuilder;
using ASFW::Async::OHCIEventCode;
using ASFW::Async::TxCompletion;
using ASFW::Async::Engine::ATManager;
using ASFW::Driver::HardwareInterface;
using ASFW::Driver::kContextControlActiveBit;
using ASFW::Driver::kContextControlRunBit;
using ASFW::Shared::DescriptorRing;
using ASFW::Shared::DMAMemoryManager;
namespace HW = ASFW::Async::HW;

// ContextControl[15:0] as the controller writes it: run and active are set
// (OHCI §7.2.2.1: "When Command.xferStatus is written to memory, the active bit
// is always one"), event = ack_complete.
constexpr uint16_t kAckCompleteStatus = 0x8000u | 0x0400u | 0x11u;
constexpr uint16_t kTimeStamp = 0x1234u;

class ATCompletionScanTest : public ::testing::Test {
protected:
    FakeDMAMemory dma_{64 * 1024};
    DescriptorRing ring_{};
    DMAMemoryManager dmaManager_{};
    HardwareInterface hw_{};
    ATRequestContext ctx_{};
    DescriptorBuilder builder_{ring_, dmaManager_};
    ATManager<ATRequestContext, DescriptorRing, ATRequestTag> manager_{ctx_, ring_, builder_};

    struct Packet {
        size_t startIndex{0};
        HW::OHCIDescriptor* start{nullptr};
        HW::OHCIDescriptor* last{nullptr};  // the OUTPUT_LAST* that receives status
        uint32_t startIOVA{0};
        uint8_t blocks{0};
    };

    void SetUp() override {
        constexpr size_t kNumDescriptors = 64;
        auto region = dma_.AllocateRegion(kNumDescriptors * sizeof(HW::OHCIDescriptor));
        ASSERT_TRUE(region.has_value());
        auto* descriptors = reinterpret_cast<HW::OHCIDescriptor*>(region->virtualBase);
        ASSERT_TRUE(ring_.Initialize(std::span<HW::OHCIDescriptor>{descriptors, kNumDescriptors}));
        ASSERT_TRUE(ring_.Finalize(region->deviceBase));
        ASSERT_EQ(ctx_.Initialize(hw_, ring_, dmaManager_), kIOReturnSuccess);
    }

    static void SetHeaderTLabel(HW::OHCIDescriptor* header, uint8_t tLabel, uint8_t tCode) {
        auto* imm = reinterpret_cast<HW::OHCIDescriptorImmediate*>(header);
        imm->immediateData[0] = (static_cast<uint32_t>(tLabel) << 10) | (static_cast<uint32_t>(tCode) << 4);
    }

    // Block write request: OUTPUT_MORE-Immediate header (2 blocks) + OUTPUT_LAST payload.
    Packet SubmitBlockWrite(uint8_t tLabel) {
        const size_t cap = ring_.Capacity();
        const size_t idx = ring_.Tail();
        auto* first = ring_.At(idx);
        auto* last = ring_.At((idx + 2) % cap);
        std::memset(first, 0, 2 * sizeof(HW::OHCIDescriptor));
        std::memset(last, 0, sizeof(HW::OHCIDescriptor));
        first->control = 0x02000010u;  // OUTPUT_MORE, key=immediate, reqCount=16
        last->control = 0x103c0020u;   // OUTPUT_LAST, i=always, b=always, reqCount=32
        SetHeaderTLabel(first, tLabel, 0x1);
        DescriptorBuilder::DescriptorChain chain{};
        chain.first = first;
        chain.last = last;
        chain.firstIOVA32 = ring_.CommandPtrWordTo(first, 0) & 0xFFFFFFF0u;
        chain.lastIOVA32 = ring_.CommandPtrWordTo(last, 0) & 0xFFFFFFF0u;
        chain.firstBlocks = 2;
        chain.lastBlocks = 1;
        chain.firstRingIndex = idx;
        chain.lastRingIndex = (idx + 2) % cap;
        chain.txid = ++txid_;
        EXPECT_EQ(manager_.Submit(std::move(chain), {}), kIOReturnSuccess);
        return {idx, first, last, ring_.CommandPtrWordTo(first, 0) & 0xFFFFFFF0u, 3};
    }

    // Quadlet write request: one OUTPUT_LAST-Immediate (2 blocks).
    Packet SubmitQuadletWrite(uint8_t tLabel) {
        const size_t idx = ring_.Tail();
        auto* desc = ring_.At(idx);
        std::memset(desc, 0, 2 * sizeof(HW::OHCIDescriptor));
        desc->control = 0x123c0010u;  // OUTPUT_LAST, key=immediate, i=always, b=always, reqCount=16
        SetHeaderTLabel(desc, tLabel, 0x0);
        DescriptorBuilder::DescriptorChain chain{};
        chain.first = desc;
        chain.last = desc;
        chain.firstIOVA32 = ring_.CommandPtrWordTo(desc, 0) & 0xFFFFFFF0u;
        chain.lastIOVA32 = chain.firstIOVA32;
        chain.firstBlocks = 2;
        chain.lastBlocks = 2;
        chain.firstRingIndex = idx;
        chain.lastRingIndex = (idx + 1) % ring_.Capacity();  // as DescriptorBuilder.cpp:353
        chain.txid = ++txid_;
        EXPECT_EQ(manager_.Submit(std::move(chain), {}), kIOReturnSuccess);
        return {idx, desc, desc, chain.firstIOVA32, 2};
    }

    // The controller retires a packet: status and timeStamp land in its
    // OUTPUT_LAST* only (OHCI §7.1.5.2, §7.1.5.3.1).
    static void Retire(const Packet& p) {
        p.last->xferStatus = kAckCompleteStatus;
        p.last->timeStamp = kTimeStamp;
    }

    void SetContext(uint32_t control, const Packet& commandPtrBlock) {
        hw_.SetTestRegister(ATRequestTag::kControlSetReg, control);
        hw_.SetTestRegister(ATRequestTag::kCommandPtrReg,
                            commandPtrBlock.startIOVA | commandPtrBlock.blocks);
    }

    std::vector<TxCompletion> Drain() {
        std::vector<TxCompletion> out;
        while (auto completion = ctx_.ScanCompletion()) out.push_back(*completion);
        return out;
    }

    uint32_t txid_{0};
};

TEST_F(ATCompletionScanTest, BlockWriteCompletesFromItsOutputLastStatus) {
    const Packet write = SubmitBlockWrite(5);
    Retire(write);
    SetContext(kContextControlRunBit, write);  // idle: CommandPtr on the finished block (Table 3-4)

    const auto completion = ctx_.ScanCompletion();
    ASSERT_TRUE(completion.has_value()) << "a retired block write must complete without a later packet";
    EXPECT_EQ(completion->tLabel, 5u);
    EXPECT_EQ(completion->eventCode, OHCIEventCode::kAckComplete);
    EXPECT_EQ(completion->timeStamp, kTimeStamp);
    EXPECT_EQ(ring_.Head(), ring_.Tail()) << "all three blocks are consumed together";
}

TEST_F(ATCompletionScanTest, QuadletWriteCompletesFromItsImmediateStatus) {
    const Packet write = SubmitQuadletWrite(7);
    Retire(write);
    SetContext(kContextControlRunBit, write);

    const auto completion = ctx_.ScanCompletion();
    ASSERT_TRUE(completion.has_value());
    EXPECT_EQ(completion->tLabel, 7u);
    EXPECT_EQ(completion->eventCode, OHCIEventCode::kAckComplete);
    EXPECT_EQ(ring_.Head(), ring_.Tail());
}

TEST_F(ATCompletionScanTest, DrainReturnsEveryRetiredPacketInOrder) {
    const Packet a = SubmitQuadletWrite(1);
    const Packet b = SubmitBlockWrite(2);
    const Packet c = SubmitQuadletWrite(3);
    Retire(a);
    Retire(b);
    Retire(c);
    SetContext(kContextControlRunBit, c);

    const auto completions = Drain();
    ASSERT_EQ(completions.size(), 3u) << "one interrupt drains every retired packet";
    EXPECT_EQ(completions[0].tLabel, 1u);
    EXPECT_EQ(completions[1].tLabel, 2u);
    EXPECT_EQ(completions[2].tLabel, 3u);
    EXPECT_EQ(ring_.Head(), ring_.Tail());
}

TEST_F(ATCompletionScanTest, InFlightHeadIsKeptWhileAPipelinedLaterPacketRetires) {
    const Packet a = SubmitBlockWrite(1);
    const Packet b = SubmitQuadletWrite(2);
    Retire(b);  // out-of-order retirement is allowed (OHCI §7.7)
    SetContext(kContextControlRunBit | kContextControlActiveBit, b);  // furthest fetched block

    EXPECT_TRUE(Drain().empty()) << "the head is in flight, not orphaned";
    EXPECT_EQ(ring_.Head(), a.startIndex) << "no descriptor of an in-flight packet may be discarded";

    Retire(a);
    SetContext(kContextControlRunBit, b);
    const auto completions = Drain();
    ASSERT_EQ(completions.size(), 2u);
    EXPECT_EQ(completions[0].tLabel, 1u);
    EXPECT_EQ(completions[1].tLabel, 2u);
    EXPECT_EQ(ring_.Head(), ring_.Tail());
}

TEST_F(ATCompletionScanTest, StoppedContextSkipsAWholeUnsentPacketAndKeepsScanning) {
    const Packet unsent = SubmitBlockWrite(1);
    const Packet sent = SubmitQuadletWrite(2);
    Retire(sent);
    SetContext(0, unsent);  // run and active clear: the unsent packet will never get status

    const auto completions = Drain();
    ASSERT_EQ(completions.size(), 1u) << "the skip must not end the scan";
    EXPECT_EQ(completions[0].tLabel, 2u);
    EXPECT_EQ(ring_.Head(), ring_.Tail()) << "header and payload of the unsent packet are both released";
}

} // namespace
} // namespace ASFW::Testing
