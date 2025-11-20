#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "ASFWDriver/ConfigROM/ConfigROMBuilder.hpp"
#include "ASFWDriver/ConfigROM/ConfigROMTypes.hpp"
#include "TestDataUtils.hpp"

using ASFW::Driver::ConfigROMBuilder;
using ASFW::FW::MakeDirectoryEntry;
using ASFW::FW::EntryType;
using ASFW::FW::ConfigKey;
using ASFW::FW::kBusNameQuadlet;

namespace {

constexpr uint32_t kGenerationShift = 4;
constexpr uint32_t kGenerationMask = 0xFu << kGenerationShift;
constexpr uint32_t kMaxRomShift = 8;
constexpr uint32_t kMaxRomMask = 0xFu << kMaxRomShift;
constexpr uint32_t kMaxRecShift = 12;
constexpr uint32_t kMaxRecMask = 0xFu << kMaxRecShift;

constexpr uint16_t kPolynomial = ASFW::FW::kConfigROMCRCPolynomial;

constexpr uint32_t Swap32(uint32_t value) noexcept {
#if defined(__clang__) || defined(__GNUC__)
    return __builtin_bswap32(value);
#else
    return (value >> 24) |
           ((value >> 8) & 0x0000FF00u) |
           ((value << 8) & 0x00FF0000u) |
           (value << 24);
#endif
}

constexpr uint32_t HostToBig(uint32_t value) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return Swap32(value);
    }
    return value;
}

uint16_t StepCRC(uint16_t crc, uint16_t data) {
    crc = static_cast<uint16_t>(crc ^ data);
    for (int bit = 0; bit < 16; ++bit) {
        if (crc & 0x8000u) {
            crc = static_cast<uint16_t>((crc << 1) ^ kPolynomial);
        } else {
            crc = static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

uint16_t ComputeCRC(std::span<const uint32_t> words, size_t start, size_t count) {
    uint16_t crc = 0;
    const size_t end = std::min(start + count, words.size());
    for (size_t idx = start; idx < end; ++idx) {
        const uint32_t word = words[idx];
        crc = StepCRC(crc, static_cast<uint16_t>((word >> 16) & 0xFFFFu));
        crc = StepCRC(crc, static_cast<uint16_t>(word & 0xFFFFu));
    }
    return crc;
}

std::string MakePatternString(size_t length) {
    std::string text;
    text.reserve(length);
    for (size_t idx = 0; idx < length; ++idx) {
        text.push_back(static_cast<char>('A' + (idx % 26)));
    }
    return text;
}

void ValidateDirectory(std::span<const uint32_t> words,
                       size_t headerIndex,
                       std::unordered_set<size_t>& visited) {
    ASSERT_LT(headerIndex, words.size());
    if (!visited.insert(headerIndex).second) {
        return; // Already validated
    }

    const uint32_t header = words[headerIndex];
    const size_t entryCount = static_cast<size_t>(header >> 16);
    ASSERT_LE(headerIndex + 1 + entryCount, words.size());
    EXPECT_EQ(static_cast<uint16_t>(header & 0xFFFFu), ComputeCRC(words, headerIndex + 1, entryCount));

    for (size_t entry = 0; entry < entryCount; ++entry) {
        const size_t entryIndex = headerIndex + 1 + entry;
        const uint32_t value = words[entryIndex];
        const uint32_t type = value >> 30;
        const uint32_t offset = value & 0x00FFFFFFu;

        if (type == static_cast<uint32_t>(ASFW::FW::EntryType::kLeaf)) {
            EXPECT_NE(offset, 0u) << "Leaf entry at index " << entryIndex << " has zero offset";
            const size_t leafHeaderIndex = entryIndex + static_cast<size_t>(offset);
            ASSERT_LT(leafHeaderIndex, words.size());
            const uint32_t leafHeader = words[leafHeaderIndex];
            const size_t payloadQuadlets = static_cast<size_t>(leafHeader >> 16);
            ASSERT_LE(leafHeaderIndex + 1 + payloadQuadlets, words.size());
            EXPECT_EQ(static_cast<uint16_t>(leafHeader & 0xFFFFu),
                      ComputeCRC(words, leafHeaderIndex + 1, payloadQuadlets));
        } else if (type == static_cast<uint32_t>(ASFW::FW::EntryType::kDirectory)) {
            EXPECT_NE(offset, 0u) << "Directory entry at index " << entryIndex << " has zero offset";
            const size_t directoryHeaderIndex = entryIndex + static_cast<size_t>(offset);
            ValidateDirectory(words, directoryHeaderIndex, visited);
        }
    }
}

} // namespace

TEST(ConfigROMBuilderTests, BuildProducesExpectedLayout) {
    ConfigROMBuilder builder;
    constexpr uint32_t busOptions = 0x00008000u; // MaxRec=8 (0x8 << 12), MaxROM=0 -> should mirror MaxRec
    constexpr uint64_t guid = 0x1122334455667788ULL;
    constexpr uint32_t nodeCapabilities = 0x00ABCDEFu;
    constexpr std::string_view vendorName = "Acme";

    builder.Build(busOptions, guid, nodeCapabilities, vendorName);

    ASSERT_EQ(builder.QuadletCount(), 11u);

    auto native = builder.ImageNative();
    ASSERT_EQ(native.size(), builder.QuadletCount());

    const uint32_t header = builder.HeaderQuad();
    EXPECT_EQ(header >> 24, 4u);
    EXPECT_EQ((header >> 16) & 0xFFu, 4u);
    EXPECT_EQ(header & 0xFFFFu, ComputeCRC(native, 1, 4));

    EXPECT_EQ(native[1], kBusNameQuadlet);
    EXPECT_EQ(native[3], static_cast<uint32_t>(guid >> 32));
    EXPECT_EQ(native[4], static_cast<uint32_t>(guid & 0xFFFFFFFFu));

    const uint32_t busInfo = builder.BusInfoQuad();
    EXPECT_EQ((busInfo & kGenerationMask) >> kGenerationShift, 0u);
    const uint32_t maxRec = (busInfo & kMaxRecMask) >> kMaxRecShift;
    const uint32_t maxRom = (busInfo & kMaxRomMask) >> kMaxRomShift;
    EXPECT_EQ(maxRec, maxRom);

    const uint32_t expectedVendorIdEntry = MakeDirectoryEntry(
        ASFW::FW::ConfigKey::kModuleVendorId, ASFW::FW::EntryType::kImmediate,
        static_cast<uint32_t>((guid >> 40) & 0xFFFFFFu));
    EXPECT_EQ(native[6], expectedVendorIdEntry);

    const uint32_t expectedNodeCapsEntry = MakeDirectoryEntry(
        ASFW::FW::ConfigKey::kNodeCapabilities, ASFW::FW::EntryType::kImmediate, nodeCapabilities);
    EXPECT_EQ(native[7], expectedNodeCapsEntry);

    const uint32_t leafEntry = native[8];
    const uint32_t expectedLeafEntry = MakeDirectoryEntry(
        ASFW::FW::ConfigKey::kTextualDescriptor, ASFW::FW::EntryType::kLeaf, 9u);
    EXPECT_EQ(leafEntry, expectedLeafEntry);

    const uint32_t rootHeader = native[5];
    EXPECT_EQ(rootHeader >> 16, 5u);
    EXPECT_EQ(rootHeader & 0xFFFFu, ComputeCRC(native, 6, 5));

    const uint32_t leafHeader = native[9];
    EXPECT_EQ(leafHeader >> 16, 1u);
    EXPECT_EQ(leafHeader & 0xFFFFu, ComputeCRC(native, 10, 1));
    EXPECT_EQ(native[10], 0x41636D65u);

    auto be = builder.ImageBE();
    ASSERT_EQ(be.size(), native.size());
    for (size_t idx = 0; idx < be.size(); ++idx) {
        EXPECT_EQ(be[idx], HostToBig(native[idx])) << "Mismatch at quadlet " << idx;
    }
}

TEST(ConfigROMBuilderTests, UpdateGenerationRefreshesBusInfoAndHeaderCrc) {
    ConfigROMBuilder builder;
    constexpr uint32_t busOptions = 0x00008000u;
    constexpr uint64_t guid = 0x0000000000000000ULL;

    builder.Begin(busOptions, guid, 0);
    builder.UpdateGeneration(9);

    ASSERT_EQ(builder.QuadletCount(), 5u);

    auto native = builder.ImageNative();
    ASSERT_EQ(native.size(), builder.QuadletCount());

    const uint32_t busInfo = builder.BusInfoQuad();
    EXPECT_EQ((busInfo & kGenerationMask) >> kGenerationShift, 9u);
    const uint32_t maxRec = (busInfo & kMaxRecMask) >> kMaxRecShift;
    const uint32_t maxRom = (busInfo & kMaxRomMask) >> kMaxRomShift;
    EXPECT_EQ(maxRom, maxRec);

    const uint32_t header = builder.HeaderQuad();
    EXPECT_EQ(header >> 24, 4u);
    EXPECT_EQ((header >> 16) & 0xFFu, 4u);
    EXPECT_EQ(header & 0xFFFFu, ComputeCRC(native, 1, 4));
}

class ConfigROMBuilderLeafCrcTests : public ::testing::TestWithParam<size_t> {};

TEST_P(ConfigROMBuilderLeafCrcTests, LeafHeaderCrcMatchesPolynomial) {
    const size_t textLength = GetParam();

    ConfigROMBuilder builder;
    constexpr uint32_t busOptions = 0x00008000u;
    constexpr uint64_t guid = 0x1122334455667788ULL;
    constexpr uint32_t nodeCapabilities = 0x0055AAFFu;

    builder.Begin(busOptions, guid, nodeCapabilities);
    ASSERT_TRUE(builder.AddImmediateEntry(ASFW::FW::ConfigKey::kModuleVendorId, static_cast<uint32_t>((guid >> 40) & 0xFFFFFFu)));
    ASSERT_TRUE(builder.AddImmediateEntry(ASFW::FW::ConfigKey::kNodeCapabilities, nodeCapabilities));

    const std::string vendorText = MakePatternString(textLength);
    const auto leafHandle = builder.AddTextLeaf(ASFW::FW::ConfigKey::kTextualDescriptor, vendorText);
    ASSERT_TRUE(leafHandle.valid());
    builder.Finalize();

    const auto native = builder.ImageNative();
    ASSERT_GT(native.size(), leafHandle.offsetQuadlets);

    const uint32_t leafHeader = native[leafHandle.offsetQuadlets];
    const size_t payloadQuadlets = static_cast<size_t>(leafHeader >> 16);
    EXPECT_EQ(payloadQuadlets, (vendorText.size() + 3) / 4);

    const uint16_t expectedLeafCrc = ComputeCRC(native, leafHandle.offsetQuadlets + 1, payloadQuadlets);
    EXPECT_EQ(static_cast<uint16_t>(leafHeader & 0xFFFFu), expectedLeafCrc);

    constexpr size_t kRootDirHeaderIndex = 5; // Begin() writes 5 quadlets before root directory header placeholder
    ASSERT_GT(native.size(), kRootDirHeaderIndex);
    const uint32_t rootHeader = native[kRootDirHeaderIndex];
    const size_t rootEntries = native.size() - (kRootDirHeaderIndex + 1);
    EXPECT_EQ(rootHeader >> 16, rootEntries);

    const uint16_t expectedRootCrc = ComputeCRC(native, kRootDirHeaderIndex + 1, rootEntries);
    EXPECT_EQ(static_cast<uint16_t>(rootHeader & 0xFFFFu), expectedRootCrc);
}

INSTANTIATE_TEST_SUITE_P(
    VariousLengths,
    ConfigROMBuilderLeafCrcTests,
    ::testing::Values(size_t{0}, size_t{1}, size_t{2}, size_t{3}, size_t{4},
                      size_t{5}, size_t{7}, size_t{8}, size_t{9}, size_t{15}, size_t{16}));

struct ReferenceRomCase {
    const char* arrayName;
    const char* description;
};

class ConfigROMReferenceCrcTests : public ::testing::TestWithParam<ReferenceRomCase> {};

TEST_P(ConfigROMReferenceCrcTests, ReferenceDataHasValidCrcs) {
    const auto& testCase = GetParam();
    SCOPED_TRACE(testCase.description);

    std::vector<uint32_t> words;
    std::string errorMessage;
    ASSERT_TRUE(ASFW::Tests::LoadHexArrayFromRepoFile("firewire/device-attribute-test.c",
                                                      testCase.arrayName,
                                                      words,
                                                      &errorMessage))
        << errorMessage;
    ASSERT_FALSE(words.empty());

    std::span<const uint32_t> span(words.data(), words.size());

    ASSERT_GE(words.size(), static_cast<size_t>(5));
    const uint32_t bibHeader = words[0];
    const size_t bibLength = static_cast<size_t>(bibHeader >> 24);
    const size_t bibCoverage = static_cast<size_t>((bibHeader >> 16) & 0xFFu);
    ASSERT_LE(1 + bibLength, words.size());
    ASSERT_LE(1 + bibCoverage, words.size());
    EXPECT_EQ(static_cast<uint16_t>(bibHeader & 0xFFFFu), ComputeCRC(span, 1, bibCoverage));

    std::unordered_set<size_t> visited;
    constexpr size_t kRootDirectoryIndex = 5; // Bus info block occupies quadlets 0..4
    ASSERT_LT(kRootDirectoryIndex, words.size());
    ValidateDirectory(span, kRootDirectoryIndex, visited);
    EXPECT_FALSE(visited.empty());
}

INSTANTIATE_TEST_SUITE_P(
    LinuxReferenceData,
    ConfigROMReferenceCrcTests,
    ::testing::Values(
        ReferenceRomCase{"simple_avc_config_rom", "Simple AV/C device (Annex C)"},
        ReferenceRomCase{"legacy_avc_config_rom", "Legacy AV/C device (Annex A)"}));
