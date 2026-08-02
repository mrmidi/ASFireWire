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

ASFW::Discovery::ConfigROM MakeSBP2ROM(ASFW::Discovery::Generation gen,
                                       uint8_t nodeId,
                                       ASFW::Discovery::Guid64 guid) {
    auto rom = MakeROM(gen, nodeId, guid);
    ASFW::Discovery::UnitDirectory unit{};
    unit.unitSpecId = 0x00609E;
    unit.unitSwVersion = 0x010483;
    rom.unitDirectories.push_back(unit);
    rom.rawQuadlets.resize(34, 0);
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

TEST(ConfigROMStoreConcurrencyTests, SameGenerationShorterInsertDoesNotRegressNodeCache) {
    ASFW::Discovery::ConfigROMStore store;
    constexpr ASFW::Discovery::Guid64 kGuid = 0x0003DB0A0000D112ULL;
    const ASFW::Discovery::Generation kGeneration{4};
    constexpr uint8_t kNodeId = 2;

    auto rich = MakeROM(kGeneration, kNodeId, kGuid);
    rich.rawQuadlets.resize(31, 0);
    store.Insert(rich);

    auto partialRetry = rich;
    partialRetry.rawQuadlets.resize(19);
    store.Insert(partialRetry);

    const auto* byNode = store.FindByNode(kGeneration, kNodeId);
    ASSERT_NE(byNode, nullptr);
    EXPECT_EQ(byNode->rawQuadlets.size(), 31u);

    const auto* byGuid = store.FindByGuid(kGuid);
    ASSERT_NE(byGuid, nullptr);
    EXPECT_EQ(byGuid->rawQuadlets.size(), 31u);
}

TEST(ConfigROMStoreConcurrencyTests, LatestLookupPrefersPreviousProfileOverNewerPartialROM) {
    ASFW::Discovery::ConfigROMStore store;
    constexpr ASFW::Discovery::Guid64 kGuid = 0x0090b54001ffffffULL;

    store.Insert(MakeSBP2ROM(ASFW::Discovery::Generation{2}, 0, kGuid));
    store.Insert(MakeROM(ASFW::Discovery::Generation{3}, 0, kGuid));

    const auto* latest = store.FindLatestForNode(0);
    ASSERT_NE(latest, nullptr);
    EXPECT_EQ(latest->gen.value, 2u);
    EXPECT_EQ(latest->unitDirectories.size(), 1u);

    const auto* byGuid = store.FindByGuid(kGuid);
    ASSERT_NE(byGuid, nullptr);
    EXPECT_EQ(byGuid->gen.value, 2u);
    EXPECT_EQ(byGuid->unitDirectories.size(), 1u);
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

TEST(ConfigROMStoreConcurrencyTests, LatestLookupDoesNotReuseProfileAcrossDifferentGuid) {
    ASFW::Discovery::ConfigROMStore store;
    constexpr ASFW::Discovery::Guid64 kOldGuid = 0x0090b54001ffffffULL;
    constexpr ASFW::Discovery::Guid64 kNewGuid = 0x00a0c91002000000ULL;

    store.Insert(MakeSBP2ROM(ASFW::Discovery::Generation{2}, 0, kOldGuid));
    store.Insert(MakeROM(ASFW::Discovery::Generation{3}, 0, kNewGuid));

    const auto* latest = store.FindLatestForNode(0);
    ASSERT_NE(latest, nullptr);
    EXPECT_EQ(latest->gen.value, 3u);
    EXPECT_EQ(latest->bib.guid, kNewGuid);
    EXPECT_TRUE(latest->unitDirectories.empty());
}

TEST(ConfigROMStoreConcurrencyTests, LatestLookupUsesNewerProfileWhenItCompletes) {
    ASFW::Discovery::ConfigROMStore store;
    constexpr ASFW::Discovery::Guid64 kGuid = 0x0090b54001ffffffULL;

    store.Insert(MakeSBP2ROM(ASFW::Discovery::Generation{2}, 0, kGuid));
    store.Insert(MakeROM(ASFW::Discovery::Generation{3}, 0, kGuid));
    store.Insert(MakeSBP2ROM(ASFW::Discovery::Generation{4}, 0, kGuid));

    const auto* latest = store.FindLatestForNode(0);
    ASSERT_NE(latest, nullptr);
    EXPECT_EQ(latest->gen.value, 4u);

    const auto* byGuid = store.FindByGuid(kGuid);
    ASSERT_NE(byGuid, nullptr);
    EXPECT_EQ(byGuid->gen.value, 4u);
}
