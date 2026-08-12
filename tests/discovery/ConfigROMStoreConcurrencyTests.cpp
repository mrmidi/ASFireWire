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
                (void)store.FindByObservedGuid(guid);
            }
        });
    }

    startGate.arrive_and_wait();

    for (auto& thread : threads) {
        thread.join();
    }
}

TEST(ConfigROMStoreConcurrencyTests, SameGenerationInsertRefreshesRouteCache) {
    ASFW::Discovery::ConfigROMStore store;

    constexpr ASFW::Discovery::Guid64 kGuid = 0x00130e0402004713ULL;

    auto truncated = MakeROM(ASFW::Discovery::Generation{2}, 2, kGuid);
    truncated.rawQuadlets = {0x04040B5Du, 0x31333934u, 0xE0FF8112u, 0x00130E04u, 0x02004713u};
    store.Insert(truncated);

    auto complete = truncated;
    complete.rawQuadlets.push_back(0x00000000u);
    store.Insert(complete);

    const auto byNode = store.FindByNode(ASFW::Discovery::Generation{2}, 2);
    ASSERT_TRUE(byNode.has_value());
    EXPECT_EQ(byNode->gen.value, 2u);
    EXPECT_EQ(byNode->rawQuadlets.size(), 6u);
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

    const auto byNode = store.FindByNode(kGeneration, kNodeId);
    ASSERT_TRUE(byNode.has_value());
    EXPECT_EQ(byNode->rawQuadlets.size(), 31u);

    const auto matches = store.FindByObservedGuid(kGuid);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front().rawQuadlets.size(), 31u);
}

TEST(ConfigROMStoreConcurrencyTests, LatestLookupNeverSubstitutesOlderGuidMatch) {
    ASFW::Discovery::ConfigROMStore store;
    constexpr ASFW::Discovery::Guid64 kGuid = 0x0090b54001ffffffULL;

    store.Insert(MakeSBP2ROM(ASFW::Discovery::Generation{2}, 0, kGuid));
    store.Insert(MakeROM(ASFW::Discovery::Generation{3}, 0, kGuid));

    const auto latest = store.FindLatestForNode(0);
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->gen.value, 3u);
    EXPECT_TRUE(latest->unitDirectories.empty());

    const auto matches = store.FindByObservedGuid(kGuid);
    ASSERT_EQ(matches.size(), 2u);
}

TEST(ConfigROMStoreConcurrencyTests, InvalidateRemovesGenerationNodeReachability) {
    ASFW::Discovery::ConfigROMStore store;

    constexpr ASFW::Discovery::Guid64 kGuid = 0x00130e0402004713ULL;
    store.Insert(MakeROM(ASFW::Discovery::Generation{2}, 2, kGuid));

    ASSERT_TRUE(store.FindByNode(ASFW::Discovery::Generation{2}, 2).has_value());
    ASSERT_EQ(store.FindByObservedGuid(kGuid).size(), 1u);

    store.InvalidateNode(ASFW::Discovery::Generation{2}, 2);

    const auto invalid = store.FindByNode(ASFW::Discovery::Generation{2}, 2);
    ASSERT_TRUE(invalid.has_value());
    EXPECT_EQ(invalid->state, ASFW::Discovery::ROMState::Invalid);

    store.PruneInvalid();
    EXPECT_FALSE(store.FindByNode(ASFW::Discovery::Generation{2}, 2).has_value());
    EXPECT_TRUE(store.FindByObservedGuid(kGuid).empty());
}

TEST(ConfigROMStoreConcurrencyTests, LatestLookupDoesNotReuseProfileAcrossDifferentGuid) {
    ASFW::Discovery::ConfigROMStore store;
    constexpr ASFW::Discovery::Guid64 kOldGuid = 0x0090b54001ffffffULL;
    constexpr ASFW::Discovery::Guid64 kNewGuid = 0x00a0c91002000000ULL;

    store.Insert(MakeSBP2ROM(ASFW::Discovery::Generation{2}, 0, kOldGuid));
    store.Insert(MakeROM(ASFW::Discovery::Generation{3}, 0, kNewGuid));

    const auto latest = store.FindLatestForNode(0);
    ASSERT_TRUE(latest.has_value());
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

    const auto latest = store.FindLatestForNode(0);
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->gen.value, 4u);

    const auto matches = store.FindByObservedGuid(kGuid);
    ASSERT_EQ(matches.size(), 3u);
    EXPECT_EQ(matches.back().gen.value, 4u);
}

TEST(ConfigROMStoreConcurrencyTests, DuplicateAndZeroObservedGuidsRemainDistinctRoutes) {
    ASFW::Discovery::ConfigROMStore store;
    constexpr ASFW::Discovery::Guid64 kDuplicateGuid = 0x000d6c0404000002ULL;
    const ASFW::Discovery::Generation generation{8};

    store.Insert(MakeROM(generation, 1, kDuplicateGuid));
    store.Insert(MakeROM(generation, 2, kDuplicateGuid));
    store.Insert(MakeROM(generation, 3, 0));

    EXPECT_TRUE(store.FindByNode(generation, 1).has_value());
    EXPECT_TRUE(store.FindByNode(generation, 2).has_value());
    EXPECT_TRUE(store.FindByNode(generation, 3).has_value());
    EXPECT_EQ(store.FindByObservedGuid(kDuplicateGuid).size(), 2u);
    EXPECT_EQ(store.FindByObservedGuid(0).size(), 1u);
}
