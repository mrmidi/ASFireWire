#include <gtest/gtest.h>

#include "ASFWDriver/ConfigROM/ConfigROMStore.hpp"

#include <barrier>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

ASFW::Discovery::ConfigROM MakeROM(ASFW::Discovery::Generation gen,
                                  uint8_t nodeId,
                                  ASFW::Discovery::Guid64 guid) {
    ASFW::Discovery::ConfigROM rom{};
    rom.gen = gen;
    rom.nodeId = nodeId;
    rom.bib.guid = guid;
    rom.rawQuadlets = {0x31333934u};
    return rom;
}

} // namespace

TEST(ConfigROMStoreConcurrencyTests, ConcurrentInsertAndLookupDoesNotCrash) {
    ASFW::Discovery::ConfigROMStore store;

    constexpr int kThreads = 8;
    constexpr int kIterationsPerThread = 500;

    std::barrier startGate(kThreads + 1);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            startGate.arrive_and_wait();

            for (int i = 0; i < kIterationsPerThread; ++i) {
                const auto gen = static_cast<ASFW::Discovery::Generation>((i % 4) + 1);
                const auto nodeId = static_cast<uint8_t>((t + i) % 64);
                const auto guid = (static_cast<uint64_t>(t + 1) << 32) | static_cast<uint32_t>(i);

                store.Insert(MakeROM(gen, nodeId, guid));

                (void)store.FindByNode(gen, nodeId);
                (void)store.FindLatestForNode(nodeId);
                EXPECT_NE(store.FindByGuid(guid), nullptr);
            }
        });
    }

    startGate.arrive_and_wait();

    for (auto& thread : threads) {
        thread.join();
    }
}

TEST(ConfigROMStoreConcurrencyTests, SameGenerationInsertRefreshesGuidCache) {
    ASFW::Discovery::ConfigROMStore store;

    constexpr ASFW::Discovery::Guid64 kGuid = 0x00130e0402004713ULL;

    auto truncated = MakeROM(ASFW::Discovery::Generation{2}, 2, kGuid);
    truncated.rawQuadlets = {0x04040B5Du, 0x31333934u, 0xE0FF8112u, 0x00130E04u, 0x02004713u};
    store.Insert(truncated);

    auto complete = truncated;
    complete.rawQuadlets.push_back(0x00000000u);
    store.Insert(complete);

    const auto* byGuid = store.FindByGuid(kGuid);
    ASSERT_NE(byGuid, nullptr);
    EXPECT_EQ(byGuid->gen.value, 2u);
    EXPECT_EQ(byGuid->rawQuadlets.size(), 6u);
}

TEST(ConfigROMStoreConcurrencyTests, InvalidateRemovesGenerationNodeReachability) {
    ASFW::Discovery::ConfigROMStore store;

    constexpr ASFW::Discovery::Guid64 kGuid = 0x00130e0402004713ULL;
    store.Insert(MakeROM(ASFW::Discovery::Generation{2}, 2, kGuid));

    ASSERT_NE(store.FindByNode(ASFW::Discovery::Generation{2}, 2), nullptr);
    ASSERT_NE(store.FindByGuid(kGuid), nullptr);

    store.InvalidateROM(kGuid);

    EXPECT_EQ(store.FindByNode(ASFW::Discovery::Generation{2}, 2), nullptr);
    ASSERT_NE(store.FindByGuid(kGuid), nullptr);
    EXPECT_EQ(store.FindByGuid(kGuid)->state, ASFW::Discovery::ROMState::Invalid);
}
