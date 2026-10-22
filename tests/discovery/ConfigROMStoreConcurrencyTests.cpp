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

                // Lookups race the erasing calls (SuspendAll on the driver
                // queue, EraseByGuid from the user client) and must only ever
                // see whole copies.
                (void)store.FindByNode(gen, nodeId, true);
                if (i % 50 == 0) {
                    store.SuspendAll(gen);
                }
                if (i % 70 == 0) {
                    store.EraseByGuid(guid);
                }
            }
        });
    }

    startGate.arrive_and_wait();

    for (auto& thread : threads) {
        thread.join();
    }
}

TEST(ConfigROMStoreConcurrencyTests, SameGenerationLongerInsertReplacesNodeCache) {
    ASFW::Discovery::ConfigROMStore store;

    constexpr ASFW::Discovery::Guid64 kGuid = 0x00130e0402004713ULL;
    const ASFW::Discovery::Generation kGeneration{2};
    constexpr uint8_t kNodeId = 2;

    auto truncated = MakeROM(kGeneration, kNodeId, kGuid);
    truncated.rawQuadlets = {0x04040B5Du, 0x31333934u, 0xE0FF8112u, 0x00130E04u, 0x02004713u};
    store.Insert(truncated);

    auto complete = truncated;
    complete.rawQuadlets.push_back(0x00000000u);
    store.Insert(complete);

    const auto byNode = store.FindByNode(kGeneration, kNodeId, false);
    ASSERT_TRUE(byNode.has_value());
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

    const auto byNode = store.FindByNode(kGeneration, kNodeId, true);
    ASSERT_TRUE(byNode.has_value());
    EXPECT_EQ(byNode->rawQuadlets.size(), 31u);
}

// TriggerROMRead erases a device's cached ROMs before a manual reread, so the
// reread is not refused in favour of the richer cached copy.
TEST(ConfigROMStoreConcurrencyTests, EraseByGuidRemovesEveryGenerationOfThatDeviceOnly) {
    ASFW::Discovery::ConfigROMStore store;

    constexpr ASFW::Discovery::Guid64 kGuid = 0x00130e0402004713ULL;
    constexpr ASFW::Discovery::Guid64 kOtherGuid = 0x0090b54001ffffffULL;
    const ASFW::Discovery::Generation kPrevious{2};
    const ASFW::Discovery::Generation kCurrent{3};

    store.Insert(MakeSBP2ROM(kPrevious, 2, kGuid));
    store.SuspendAll(kCurrent);
    store.Insert(MakeSBP2ROM(kCurrent, 2, kGuid));
    store.Insert(MakeROM(kCurrent, 1, kOtherGuid));

    store.EraseByGuid(kGuid);

    EXPECT_FALSE(store.FindByNode(kPrevious, 2, true).has_value());
    EXPECT_FALSE(store.FindByNode(kCurrent, 2, true).has_value());
    ASSERT_TRUE(store.FindByNode(kCurrent, 1, false).has_value());

    // The reread's shorter ROM is now accepted.
    store.Insert(MakeROM(kCurrent, 2, kGuid));
    const auto reread = store.FindByNode(kCurrent, 2, false);
    ASSERT_TRUE(reread.has_value());
    EXPECT_TRUE(reread->unitDirectories.empty());
}

// The store compares generations only for equality, so the 255 -> 0 rollover
// (OHCI 1.2 Table 11-2) is an ordinary reset for it.
TEST(ConfigROMStoreConcurrencyTests, ResetAcrossGenerationRolloverServesTheNewGeneration) {
    ASFW::Discovery::ConfigROMStore store;

    constexpr ASFW::Discovery::Guid64 kGuid = 0x00130e0402004713ULL;
    constexpr uint8_t kNodeId = 1;
    const ASFW::Discovery::Generation kLast{255};
    const ASFW::Discovery::Generation kWrapped{0};

    store.Insert(MakeSBP2ROM(kLast, kNodeId, kGuid));
    store.SuspendAll(kWrapped);
    store.Insert(MakeROM(kWrapped, kNodeId, kGuid));

    const auto current = store.FindByNode(kWrapped, kNodeId, false);
    ASSERT_TRUE(current.has_value());
    EXPECT_EQ(current->gen.value, 0u);
    EXPECT_FALSE(store.FindByNode(kLast, kNodeId, false).has_value());
    EXPECT_TRUE(store.FindByNode(kLast, kNodeId, true).has_value());
}
TEST(ConfigROMStoreConcurrencyTests, BusResetSuspensionRejectsExactGenerationExport) {
    ASFW::Discovery::ConfigROMStore store;

    constexpr ASFW::Discovery::Guid64 kGuid = 0x00130e0402004713ULL;
    const ASFW::Discovery::Generation kCachedGeneration{2};
    constexpr uint8_t kNodeId = 2;

    store.Insert(MakeROM(kCachedGeneration, kNodeId, kGuid));
    store.SuspendAll(ASFW::Discovery::Generation{3});

    // Lifecycle code can still inspect the suspended cache entry explicitly.
    ASSERT_TRUE(store.FindByNode(kCachedGeneration, kNodeId, true).has_value());

    // The live reset edge suspends this store before selector 14 can perform
    // its filtered exact-generation lookup. The existing not-cached path must
    // then export empty data instead of stale ROM bytes.
    EXPECT_FALSE(store.FindByNode(kCachedGeneration, kNodeId, false).has_value());
}

TEST(ConfigROMStoreConcurrencyTests, BusResetKeepsOnlyThePreviousGenerationSuspended) {
    ASFW::Discovery::ConfigROMStore store;

    constexpr ASFW::Discovery::Guid64 kGuid = 0x00130e0402004713ULL;
    constexpr uint8_t kNodeId = 1;
    const ASFW::Discovery::Generation kFirst{1};
    const ASFW::Discovery::Generation kSecond{2};

    store.Insert(MakeROM(kFirst, kNodeId, kGuid));
    store.SuspendAll(kSecond);
    store.Insert(MakeROM(kSecond, kNodeId, kGuid));
    store.SuspendAll(ASFW::Discovery::Generation{3});

    // The generation suspended by the earlier reset is gone; the one suspended
    // now is still visible to lifecycle code, but not to exports.
    EXPECT_FALSE(store.FindByNode(kFirst, kNodeId, true).has_value());
    ASSERT_TRUE(store.FindByNode(kSecond, kNodeId, true).has_value());
    EXPECT_FALSE(store.FindByNode(kSecond, kNodeId, false).has_value());
}

TEST(ConfigROMStoreConcurrencyTests, GenerationWrapDoesNotKeepAnEarlierDeviceAtTheSameKey) {
    ASFW::Discovery::ConfigROMStore store;

    constexpr ASFW::Discovery::Guid64 kEarlierGuid = 0x00130e0402004713ULL;
    constexpr ASFW::Discovery::Guid64 kLaterGuid = 0x0090b54001ffffffULL;
    constexpr uint8_t kNodeId = 0;
    const ASFW::Discovery::Generation kWrappedGeneration{5};

    // A full SBP-2 ROM for one device, then two resets with nothing rediscovered.
    store.Insert(MakeSBP2ROM(kWrappedGeneration, kNodeId, kEarlierGuid));
    store.SuspendAll(ASFW::Discovery::Generation{6});
    store.SuspendAll(ASFW::Discovery::Generation{7});

    // 256 resets later the 8-bit generation comes back to the same value
    // (OHCI 1.2 Table 11-2) and a different device sits at the same node with a
    // smaller ROM. Insert keeps a richer entry under the same key, so the
    // earlier device's entry must already be gone.
    store.Insert(MakeROM(kWrappedGeneration, kNodeId, kLaterGuid));

    const auto rom = store.FindByNode(kWrappedGeneration, kNodeId, false);
    ASSERT_TRUE(rom.has_value());
    EXPECT_EQ(rom->bib.guid, kLaterGuid);
}
