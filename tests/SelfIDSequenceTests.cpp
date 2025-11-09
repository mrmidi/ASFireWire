#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ASFWDriver/Core/TopologyTypes.hpp"
#include "TestDataUtils.hpp"

using ASFW::Driver::HasMorePackets;
using ASFW::Driver::IsExtended;
using ASFW::Driver::SelfIDSequenceEnumerator;

namespace {

std::vector<uint32_t> LoadSequenceArray(std::string_view arrayName) {
    std::vector<uint32_t> words;
    std::string error;
    bool ok = ASFW::Tests::LoadHexArrayFromRepoFile(
        "firewire/self-id-sequence-helper-test.c", arrayName, words, &error);
    if (!ok) {
        ADD_FAILURE() << "Failed to load array '" << arrayName << "': " << error;
    }
    return words;
}

} // namespace

TEST(SelfIDSequenceEnumeratorTests, EnumeratesValidSequencesFromLinuxFixtures) {
    auto valid = LoadSequenceArray("valid_sequences");
    ASSERT_FALSE(valid.empty());

    SelfIDSequenceEnumerator enumerator;
    enumerator.cursor = valid.data();
    enumerator.quadlet_count = static_cast<unsigned int>(valid.size());

    std::vector<std::pair<size_t, unsigned int>> sequences;
    while (enumerator.quadlet_count > 0) {
        auto next = enumerator.next();
        ASSERT_TRUE(next.has_value());
        const auto [ptr, count] = *next;
        const size_t index = static_cast<size_t>(ptr - valid.data());
        sequences.emplace_back(index, count);
    }

    const std::vector<std::pair<size_t, unsigned int>> expected = {
        {0, 1},
        {1, 2},
        {3, 3},
        {6, 1},
    };
    EXPECT_EQ(sequences, expected);
}

TEST(SelfIDSequenceEnumeratorTests, FlagsInvalidSequenceFromLinuxFixtures) {
    auto invalid = LoadSequenceArray("invalid_sequences");
    ASSERT_FALSE(invalid.empty());

    SelfIDSequenceEnumerator enumerator;
    enumerator.cursor = invalid.data();
    enumerator.quadlet_count = static_cast<unsigned int>(invalid.size());

    auto result = enumerator.next();
    EXPECT_FALSE(result.has_value());
}

TEST(SelfIDSequenceEnumeratorTests, RecognisesChainedPacketsAndExtendedQuads) {
    auto valid = LoadSequenceArray("valid_sequences");
    ASSERT_GE(valid.size(), static_cast<size_t>(5));

    // Sequence starting at index 1 should contain two quadlets with more-bit chaining
    const uint32_t first = valid[1];
    const uint32_t second = valid[2];
    ASSERT_TRUE(HasMorePackets(first));
    EXPECT_TRUE(IsExtended(second));

    // Sequence starting at index 3 contains three quadlets; verify chaining bits.
    const uint32_t chain0 = valid[3];
    const uint32_t chain1 = valid[4];
    const uint32_t chain2 = valid[5];
    ASSERT_TRUE(HasMorePackets(chain0));
    ASSERT_TRUE(HasMorePackets(chain1));
    EXPECT_TRUE(IsExtended(chain1));
    EXPECT_TRUE(IsExtended(chain2));
    EXPECT_FALSE(HasMorePackets(chain2));
}
