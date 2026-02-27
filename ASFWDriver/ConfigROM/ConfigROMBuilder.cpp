#include "ConfigROMBuilder.hpp"
#include "ConfigROMTypes.hpp"
#include "../Common/FWCommon.hpp"

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOLib.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string_view>

namespace ASFW::Driver {

ConfigROMBuilder::ConfigROMBuilder() {
    Reset();
}

void ConfigROMBuilder::Build(uint32_t busOptions,
                             uint64_t guid,
                             uint32_t nodeCapabilities,
                             std::string_view vendorName) {
    Begin(busOptions, guid, nodeCapabilities);
    const auto vendorId = static_cast<uint32_t>((guid >> 40) & 0xFFFFFFu);
    AddImmediateEntry(ASFW::FW::ConfigKey::kModuleVendorId, vendorId);
    AddImmediateEntry(ASFW::FW::ConfigKey::kNodeCapabilities, nodeCapabilities);
    if (!vendorName.empty()) {
        AddTextLeaf(ASFW::FW::ConfigKey::kTextualDescriptor, vendorName);
    }
    Finalize();
}

void ConfigROMBuilder::Begin(uint32_t busOptions, uint64_t guid, uint32_t nodeCapabilities) {
    (void)nodeCapabilities; // value provided later via AddImmediateEntry
    Reset();
    begun_ = true;
    finalized_ = false;
    lastBusOptions_ = busOptions;

    const auto guidHi = static_cast<uint32_t>(guid >> 32);
    const auto guidLo = static_cast<uint32_t>(guid & 0xFFFFFFFFu);

    // Bus information block (5 quadlets)
    Append(0); // header placeholder
    Append(ASFW::FW::kBusNameQuadlet);
    Append(ASFW::FW::SetGeneration(busOptions, 0));
    Append(guidHi);
    Append(guidLo);
    FinaliseBIB();
}

bool ConfigROMBuilder::EnsureRootDirectory() {
    if (!begun_) return false;
    if (rootDirHeaderIndex_ == static_cast<size_t>(-1)) {
        rootDirHeaderIndex_ = quadCount_;
        Append(0); // placeholder for header
    }
    return true;
}

bool ConfigROMBuilder::AddImmediateEntry(uint8_t key, uint32_t value24) {
    if (!begun_ || finalized_) return false;
    if (!EnsureRootDirectory()) return false;
    if (quadCount_ >= kMaxQuadlets) return false;
    Append(ASFW::FW::MakeDirectoryEntry(key, ASFW::FW::EntryType::kImmediate, value24));
    return true;
}

LeafHandle ConfigROMBuilder::WriteTextLeaf(std::string_view text) {
    LeafHandle handle{};
    const size_t leafOffset = quadCount_;
    size_t payloadBytes = text.size();
    size_t payloadQuadlets = (payloadBytes + 3) / 4;
    if (leafOffset + 1 + payloadQuadlets > kMaxQuadlets) {
        return handle; // invalid
    }
    const size_t headerIndex = quadCount_;
    Append(0); // header placeholder
    for (size_t i = 0; i < payloadQuadlets; ++i) {
        uint32_t word = 0;
        for (size_t byte = 0; byte < 4; ++byte) {
            size_t idx = i * 4 + byte;
            uint8_t ch = idx < payloadBytes ? static_cast<uint8_t>(text[idx]) : 0;
            word |= static_cast<uint32_t>(ch) << (24 - static_cast<uint32_t>(byte) * 8);
        }
        Append(word);
    }
    const uint16_t crc = ComputeCRC(headerIndex + 1, payloadQuadlets);
    words_[headerIndex] = (static_cast<uint32_t>(payloadQuadlets) << 16) | crc;
    handle.offsetQuadlets = static_cast<uint16_t>(leafOffset);
    return handle;
}

LeafHandle ConfigROMBuilder::AddTextLeaf(uint8_t key, std::string_view text) {
    LeafHandle invalid{};
    if (!begun_ || finalized_) return invalid;
    if (!EnsureRootDirectory()) return invalid;
    // Reserve space for directory entry referencing leaf; we'll fill value after writing leaf.
    if (quadCount_ >= kMaxQuadlets) return invalid;
    const auto entryIndex = quadCount_;
    Append(0); // placeholder entry
    auto leafHandle = WriteTextLeaf(text);
    if (!leafHandle.valid()) return invalid; // if failed we leave placeholder (harmless)
    words_[entryIndex] = ASFW::FW::MakeDirectoryEntry(key, ASFW::FW::EntryType::kLeaf, leafHandle.offsetQuadlets);
    return leafHandle;
}

void ConfigROMBuilder::Finalize() {
    if (!begun_ || finalized_) return;
    FinaliseRootDirectory();
    finalized_ = true;
}

void ConfigROMBuilder::UpdateGeneration(uint8_t generation) {
    if (quadCount_ < 3) {
        return;
    }
    words_[2] = ASFW::FW::SetGeneration(lastBusOptions_, generation);
    FinaliseBIB();
}

std::span<const uint32_t> ConfigROMBuilder::ImageBE() const {
    std::ranges::fill(beImage_, 0u);
    for (size_t i = 0; i < quadCount_; ++i) {
        beImage_[i] = OSSwapHostToBigInt32(words_[i]);
    }
    return std::span<const uint32_t>(beImage_.data(), quadCount_);
}

std::span<const uint32_t> ConfigROMBuilder::ImageNative() const {
    // Return words_ as-is - already in host byte order
    // This is what hardware expects when reading from DMA buffer during bus reset
    return std::span<const uint32_t>(words_.data(), quadCount_);
}

uint32_t ConfigROMBuilder::HeaderQuad() const {
    return quadCount_ > 0 ? words_[0] : 0;
}

uint32_t ConfigROMBuilder::BusInfoQuad() const {
    return quadCount_ > 2 ? words_[2] : 0;
}

uint32_t ConfigROMBuilder::GuidHiQuad() const {
    return quadCount_ > 3 ? words_[3] : 0;
}

uint32_t ConfigROMBuilder::GuidLoQuad() const {
    return quadCount_ > 4 ? words_[4] : 0;
}

void ConfigROMBuilder::Reset() {
    std::ranges::fill(words_, 0u);
    std::ranges::fill(beImage_, 0u);
    quadCount_ = 0;
    rootDirHeaderIndex_ = static_cast<size_t>(-1);
    lastBusOptions_ = 0;
}

void ConfigROMBuilder::Append(uint32_t value) {
    if (quadCount_ < kMaxQuadlets) {
        words_[quadCount_] = value;
        ++quadCount_;
    }
}

uint16_t ConfigROMBuilder::ComputeCRC(size_t start, size_t count) const {
    uint16_t crc = 0;
    const size_t end = std::min(start + count, quadCount_);
    for (size_t i = start; i < end; ++i) {
        const auto word = words_[i];
        const auto hi = static_cast<uint16_t>((word >> 16) & 0xFFFFu);
        const auto lo = static_cast<uint16_t>(word & 0xFFFFu);
        crc = CRCStep(crc, hi);
        crc = CRCStep(crc, lo);
    }
    return crc;
}

uint16_t ConfigROMBuilder::CRCStep(uint16_t crc, uint16_t data) {
    crc ^= data;
    for (int bit = 0; bit < 16; ++bit) {
        if (crc & 0x8000) {
            crc = static_cast<uint16_t>((crc << 1) ^ ASFW::FW::kConfigROMCRCPolynomial);
        } else {
            crc <<= 1;
        }
    }
    return crc;
}

uint32_t ConfigROMBuilder::MakeDirectoryEntry(uint8_t key, uint8_t type, uint32_t value) {
    return ASFW::FW::MakeDirectoryEntry(key, type, value);
}

void ConfigROMBuilder::FinaliseBIB() {
    if (quadCount_ < 5) {
        return;
    }
    constexpr uint32_t kBusInfoLength = 4; // quadlets following header
    constexpr uint32_t kCrcCoverage = 4;   // quadlets covered by CRC (1..4)
    const uint16_t crc = ComputeCRC(1, kCrcCoverage);
    words_[0] = (kBusInfoLength << 24) | (kCrcCoverage << 16) | crc;
}

void ConfigROMBuilder::FinaliseRootDirectory() {
    if (rootDirHeaderIndex_ >= quadCount_) {
        return;
    }
    const size_t entries = quadCount_ - rootDirHeaderIndex_ - 1;
    const uint16_t crc = ComputeCRC(rootDirHeaderIndex_ + 1, entries);
    const uint32_t header = (static_cast<uint32_t>(entries) << 16) | crc;
    words_[rootDirHeaderIndex_] = header;
}

} // namespace ASFW::Driver
