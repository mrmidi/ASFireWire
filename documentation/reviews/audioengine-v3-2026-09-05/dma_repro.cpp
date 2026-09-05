#include "tests/audio/IsochTxDmaRingTests.cpp"
#include <functional>

class InterleavedDma final : public ASFW::Isoch::Memory::IIsochDMAMemory {
public:
    ASFW::Isoch::Memory::IIsochDMAMemory& inner;
    mutable std::function<void(const std::byte*, size_t)> onPublish;
    explicit InterleavedDma(ASFW::Isoch::Memory::IIsochDMAMemory& m):inner(m){}
    std::optional<ASFW::Shared::DMARegion> AllocateDescriptor(size_t n) override { return inner.AllocateDescriptor(n); }
    std::optional<ASFW::Shared::DMARegion> AllocatePayloadBuffer(size_t n) override { return inner.AllocatePayloadBuffer(n); }
    std::optional<ASFW::Shared::DMARegion> AllocateRegion(size_t n,size_t a) override { return inner.AllocateRegion(n,a); }
    uint64_t VirtToIOVA(const std::byte* p) const noexcept override { return inner.VirtToIOVA(p); }
    std::byte* IOVAToVirt(uint64_t p) const noexcept override { return inner.IOVAToVirt(p); }
    void PublishToDevice(const std::byte* p,size_t n) const noexcept override {
        if(onPublish) onPublish(p,n);
        inner.PublishToDevice(p,n);
    }
    void FetchFromDevice(const std::byte* p,size_t n) const noexcept override { inner.FetchFromDevice(p,n); }
    size_t TotalSize() const noexcept override { return inner.TotalSize(); }
    size_t AvailableSize() const noexcept override { return inner.AvailableSize(); }
};

class V3ReviewDmaTest: public IsochTxDmaRingTest {
protected:
    std::unique_ptr<InterleavedDma> interleaved;
    void SetUp() override {
        IsochMemoryConfig c;
        c.numDescriptors=Layout::kRingBlocks;
        c.packetSizeBytes=0;
        c.descriptorAlignment=Layout::kOHCIPageSize;
        c.payloadPageAlignment=16384;
        c.allocatePayloadSlab=false;
        dmaMemory_=IsochDMAMemoryManager::Create(c);
        ASSERT_TRUE(dmaMemory_->Initialize(hardware_));
        interleaved=std::make_unique<InterleavedDma>(*dmaMemory_);
        ASSERT_EQ(ring_.SetupRings(*interleaved),kIOReturnSuccess);
        TxPayloadDmaSegment s{kSharedPayloadIOVA,sharedPayload_.size()};
        ASSERT_TRUE(payloadDmaMap_.Configure(std::span(&s,1),sharedPayload_.size()));
    }
    void PointAt(unsigned packet) {
        hardware_.SetTestRegister(static_cast<Register32>(DMAContextHelpers::IsoXmitCommandPtr(0)),
            ring_.Slab().GetDescriptorIOVA(packet*Layout::kBlocksPerPacket)|Layout::kBlocksPerPacket);
    }
    void PrimeForReview(std::vector<IsochTxPacketMeta>& meta) {
        for(auto& m:meta) { m.payloadLength=72; m.payloadPrefixBytes=8; }
        primeControl_.numSlots=kSharedPayloadSlots;
        primeControl_.slotStrideBytes=kSharedPayloadStride;
        primeControl_.maxPacketBytes=kSharedPayloadStride;
        primeControl_.committedEnd.store(120);
        ASSERT_EQ(ring_.Prime(payloadDmaMap_,kSharedPayloadSlots,kSharedPayloadStride,
            meta.data(),&primeControl_,sharedPayload_.data(),120).packetsAssembled,Layout::kNumPackets);
        hardware_.SetTestRegister(static_cast<Register32>(DMAContextHelpers::IsoXmitContextControl(0)),0);
    }
    auto Refill(std::vector<IsochTxPacketMeta>& meta) {
        return ring_.Refill(hardware_,0,meta.data(),&primeControl_,kSharedPayloadSlots,
            sharedPayload_.data(),payloadDmaMap_);
    }
};

TEST_F(V3ReviewDmaTest, PointerAdvancesInsidePublicationButRebindStillSucceeds) {
    auto meta=MakeMetadataRing(); PrimeForReview(meta);
    PointAt(6);
    meta[8].pcmGeneration.store(1,std::memory_order_release);
    bool advanced=false;
    interleaved->onPublish=[&](const std::byte* p,size_t) {
        if(p==reinterpret_cast<const std::byte*>(ImageBytes(8,1))) {
            PointAt(8); // Hardware proceeds during a delay after the MMIO snapshot.
            advanced=true;
        }
    };
    auto out=Refill(meta);
    ASSERT_TRUE(advanced);
    ASSERT_TRUE(out.ok);
    EXPECT_EQ(meta[8].selectedPayloadImage,1);
    EXPECT_EQ(primeControl_.minimumLatePayloadRebindDistance.load(),2);
    std::puts("REPRO: live command advanced to packet 8 before its address store; rebind accepted and reports distance 2");
}

TEST_F(V3ReviewDmaTest, PublicationCanWinProducerCheckAfterSelectorHasSkippedPacket) {
    auto meta=MakeMetadataRing(); PrimeForReview(meta);
    PointAt(6);
    // Scan skips packet 8, then reaches packet 9. Interleave the producer
    // publication while the transport is publishing 9. This uses the exact
    // release-marker/frontier test of DextTxSlotProvider::PublishLatePayload.
    meta[9].pcmGeneration.store(1,std::memory_order_release);
    bool producerAccepted=false;
    interleaved->onPublish=[&](const std::byte* p,size_t) {
        if(p==reinterpret_cast<const std::byte*>(ImageBytes(9,1))) {
            meta[8].pcmGeneration.store(1,std::memory_order_release);
            producerAccepted=8>=primeControl_.finalizedEnd.load(std::memory_order_acquire);
        }
    };
    auto first=Refill(meta);
    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(producerAccepted);
    EXPECT_EQ(primeControl_.finalizedEnd.load(),14);
    EXPECT_EQ(meta[8].selectedPayloadImage,0);
    interleaved->onPublish={};
    PointAt(12);
    auto second=Refill(meta);
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(meta[8].selectedPayloadImage,0);
    std::puts("REPRO: producer reports packet 8 accepted; finality advances to 14; next completion retires packet 8 as silence");
}
