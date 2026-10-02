// SPDX-License-Identifier: Apache-2.0
#include "AudioSubunitDescriptor.hpp"
#include "AVCInfoBlock.hpp"

namespace ASFW::Protocols::AVC::Descriptors {
namespace {
/// A list descriptor (type 0x86): its entries, after header and specific info.
Parsed<ParseReader> ListEntries(std::span<const uint8_t> data) {
    return DescriptorBody(data).and_then([](ParseReader reader) -> Parsed<ParseReader> {
        uint8_t type{}, attributes{};
        return reader.Fields(type, attributes)
            .and_then([&]() -> Parsed<void> {
                if (type != 0x86) return std::unexpected(ParseError{2, ParseErrorKind::InvalidValue});
                return {};
            })
            .and_then([&] { return reader.Section(); })
            .transform([&](ParseReader) { return reader; });
    });
}
std::string Text(std::span<const uint8_t> bytes) {
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    while (!text.empty() && (text.back() == '\0' || text.back() == '\r' || text.back() == '\n')) text.pop_back();
    return text;
}
/// One text entry (type 0x93): its info blocks, keeping the name (0x000A) one.
Parsed<void> TextEntry(ParseReader entry, uint16_t position, TextDatabase& database) {
    return entry.Take(5).and_then([&](auto) -> Parsed<void> {
        while (entry.Remaining()) {
            const auto base = entry.Offset();
            auto bytes = entry.Take(entry.Remaining());
            if (!bytes) return std::unexpected(bytes.error());
            size_t used = 0;
            auto block = AVCInfoBlock::Parse(*bytes, used, base);
            if (!block) return std::unexpected(block.error());
            if (block->GetType() == 0x000A) database[position] = Text(block->GetPrimaryData());
            entry = ParseReader(bytes->subspan(used), std::add_sat(base, used));
        }
        return {};
    });
}
} // namespace

TextDatabase AudioSubunitDescriptorParser::ParseTextDatabaseList(std::span<const uint8_t> data) noexcept {
    return ParseTextDatabaseListChecked(data).value_or(TextDatabase{});
}
Parsed<TextDatabase> AudioSubunitDescriptorParser::ParseTextDatabaseListChecked(std::span<const uint8_t> data) noexcept {
    return ListEntries(data).and_then([](ParseReader entries) -> Parsed<TextDatabase> {
        return entries.BE16().and_then([&](uint16_t count) -> Parsed<TextDatabase> {
            TextDatabase database;
            for (uint16_t i = 0; i < count; ++i) {
                auto entry = entries.Section();
                if (!entry) return std::unexpected(entry.error());
                uint8_t type{}, attributes{};
                if (auto read = entry->Fields(type, attributes); !read) return std::unexpected(read.error());
                if (type != 0x93) continue;
                if (auto text = TextEntry(*entry, i, database); !text) return std::unexpected(text.error());
            }
            return entries.End().transform([&] { return std::move(database); });
        });
    });
}
Parsed<std::vector<uint16_t>> AudioSubunitDescriptorParser::ParseChildListIds(
    std::span<const uint8_t> data, uint8_t listIdSize, uint8_t objectIdSize) noexcept {
    if (listIdSize != 2) return std::unexpected(ParseError{0, ParseErrorKind::InvalidValue});
    return ListEntries(data).and_then([&](ParseReader entries) -> Parsed<std::vector<uint16_t>> {
        // ListEntries checked the header, so the list attributes byte exists.
        const bool hasObjectIds = (data[3] & 0x10) != 0;
        return entries.BE16().and_then([&](uint16_t count) -> Parsed<std::vector<uint16_t>> {
            std::vector<uint16_t> ids;
            for (uint16_t i = 0; i < count; ++i) {
                auto entry = entries.Section();
                if (!entry) return std::unexpected(entry.error());
                uint8_t type{}, attributes{};
                auto read = entry->Fields(type, attributes)
                    .and_then([&]() -> Parsed<void> {
                        if (!(attributes & 0x20)) return {};
                        return entry->BE16().transform([&](uint16_t id) { ids.push_back(id); });
                    })
                    .and_then([&]() -> Parsed<void> {
                        if (!hasObjectIds) return {};
                        if (!objectIdSize) return std::unexpected(ParseError{entry->Offset(), ParseErrorKind::InvalidValue});
                        return entry->Take(objectIdSize).transform([](auto) {});
                    })
                    .and_then([&] { return entry->Section().transform([](ParseReader) {}); });
                if (!read) return std::unexpected(read.error());
            }
            return entries.End().transform([&] { return std::move(ids); });
        });
    });
}
void AudioSubunitDescriptorParser::ResolveNames(AudioSubunitIdentifier& identifier, const TextDatabase& database) noexcept {
    for (auto& block : identifier.functionBlocks)
        if (auto found = database.find(block.nameIndex); found != database.end()) block.name = found->second;
}
} // namespace ASFW::Protocols::AVC::Descriptors
