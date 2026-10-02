// SPDX-License-Identifier: Apache-2.0
#include "AudioSubunitDescriptor.hpp"
#include "AVCInfoBlock.hpp"

namespace ASFW::Protocols::AVC::Descriptors {
// Local propagation shorthand: every field access is checked at its source.
#define AVC_FIELD(name, expression) \
    auto name##Result = (expression); \
    if (!name##Result) return std::unexpected(name##Result.error()); \
    auto name = std::move(*name##Result)

namespace {
Parsed<AudioSourceId> Source(ParseReader& reader) {
    AVC_FIELD(type, reader.U8()); AVC_FIELD(id, reader.U8());
    return AudioSourceId{type, id};
}
Parsed<void> Feature(ParseReader reader, AudioFunctionBlockInfo& block) {
    // Audio Subunit Table 8.3 (local spec text:2304-2340). The Duet capture
    // uses one-byte length/width; only its exact 08 02 00 layout is accepted.
    AVC_FIELD(bytes, reader.Take(reader.Remaining()));
    ParseReader fields(bytes, reader.Offset() - bytes.size());
    if (bytes.size() == 9 && bytes[0] == 8 && bytes[1] == 2 && bytes[2] == 0) {
        AVC_FIELD(shortLength, fields.U8());
        (void)shortLength;
        AVC_FIELD(width, fields.U8());
        (void)width;
    } else {
        AVC_FIELD(specific, fields.Section());
        auto end = fields.End(); if (!end) return std::unexpected(end.error());
        fields = specific;
        AVC_FIELD(width, fields.BE16());
        if (width != 1 && width != 2)
            return std::unexpected(ParseError{fields.Offset() - 2, ParseErrorKind::InvalidValue});
        AVC_FIELD(tag, fields.U8()); block.generalTag = tag;
        const auto readControl = [&fields, width]() -> Parsed<uint16_t> {
            if (width == 1) return fields.U8().transform([](uint8_t value) { return static_cast<uint16_t>(value); });
            return fields.BE16();
        };
        AVC_FIELD(master, readControl()); block.masterControls = master;
        while (fields.Remaining()) { AVC_FIELD(channel, readControl()); block.channelControls.push_back(channel); }
        return {};
    }
    AVC_FIELD(tag, fields.U8()); block.generalTag = tag;
    AVC_FIELD(master, fields.BE16()); block.masterControls = master;
    while (fields.Remaining()) { AVC_FIELD(channel, fields.BE16()); block.channelControls.push_back(channel); }
    return {};
}
Parsed<AudioFunctionBlockInfo> FunctionBlock(ParseReader& configuration) {
    AVC_FIELD(reader, configuration.Section());
    AudioFunctionBlockInfo block;
    AVC_FIELD(type, reader.U8()); block.type = static_cast<AudioFunctionBlockType>(type);
    AVC_FIELD(id, reader.U8()); block.id = id;
    AVC_FIELD(name, reader.BE16()); block.nameIndex = name;
    AVC_FIELD(inputs, reader.U8());
    for (size_t i = 0; i < inputs; ++i) { AVC_FIELD(source, Source(reader)); block.inputSources.push_back(source); }
    AVC_FIELD(cluster, reader.Section());
    if (cluster.Remaining()) { AVC_FIELD(channels, cluster.U8()); block.clusterChannels = channels; }
    AVC_FIELD(dependent, reader.Section());
    if (block.type == AudioFunctionBlockType::kFeature && dependent.Remaining()) {
        auto result = Feature(dependent, block); if (!result) return std::unexpected(result.error());
    } else if (block.type == AudioFunctionBlockType::kProcessing && dependent.Remaining()) {
        AVC_FIELD(process, dependent.U8()); block.processType = process;
    }
    // Additional dependent bytes are reserved/device-specific, not reparsed as
    // another block. Their bounds have already been validated by Section().
    return block;
}
Parsed<ParseReader> ListEntries(std::span<const uint8_t> data) {
    AVC_FIELD(reader, DescriptorBody(data));
    AVC_FIELD(type, reader.U8());
    if (type != 0x86) return std::unexpected(ParseError{2, ParseErrorKind::InvalidValue});
    AVC_FIELD(attributes, reader.U8()); (void)attributes;
    AVC_FIELD(specific, reader.Section()); (void)specific;
    return reader;
}
std::string Text(std::span<const uint8_t> bytes) {
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    while (!text.empty() && (text.back() == '\0' || text.back() == '\r' || text.back() == '\n')) text.pop_back();
    return text;
}
} // namespace

Parsed<AudioSubunitIdentifier> AudioSubunitDescriptorParser::ParseIdentifierDescriptor(std::span<const uint8_t> data) noexcept {
    AVC_FIELD(reader, DescriptorBody(data));
    AudioSubunitIdentifier identifier;
    AVC_FIELD(generation, reader.U8()); identifier.generationId = generation;
    AVC_FIELD(listWidth, reader.U8()); identifier.sizeOfListId = listWidth;
    AVC_FIELD(objectWidth, reader.U8()); identifier.sizeOfObjectId = objectWidth;
    AVC_FIELD(positionWidth, reader.U8()); identifier.sizeOfObjectPosition = positionWidth;
    AVC_FIELD(rootCount, reader.BE16());
    if (rootCount && listWidth != 2) return std::unexpected(ParseError{3, ParseErrorKind::InvalidValue});
    for (size_t i = 0; i < rootCount; ++i) { AVC_FIELD(id, reader.BE16()); identifier.rootListIds.push_back(id); }
    AVC_FIELD(dependent, reader.Section());
    AVC_FIELD(configuration, dependent.Section());
    AVC_FIELD(configurationId, configuration.BE16()); (void)configurationId;
    AVC_FIELD(info, configuration.Section());
    AVC_FIELD(name, info.BE16()); (void)name;
    AVC_FIELD(cluster, info.Section()); (void)cluster;
    AVC_FIELD(sourceCount, info.U8());
    for (size_t i = 0; i < sourceCount; ++i) { AVC_FIELD(source, Source(info)); identifier.sourcePlugLinks.push_back(source); }
    AVC_FIELD(blockCount, info.U8());
    for (size_t i = 0; i < blockCount; ++i) { AVC_FIELD(block, FunctionBlock(info)); identifier.functionBlocks.push_back(std::move(block)); }
    return identifier;
}

TextDatabase AudioSubunitDescriptorParser::ParseTextDatabaseList(std::span<const uint8_t> data) noexcept {
    auto result = ParseTextDatabaseListChecked(data); return result ? std::move(*result) : TextDatabase{};
}
Parsed<TextDatabase> AudioSubunitDescriptorParser::ParseTextDatabaseListChecked(std::span<const uint8_t> data) noexcept {
    AVC_FIELD(entries, ListEntries(data));
    AVC_FIELD(count, entries.BE16());
    TextDatabase database;
    for (size_t i = 0; i < count; ++i) {
        AVC_FIELD(entry, entries.Section());
        AVC_FIELD(type, entry.U8());
        AVC_FIELD(attributes, entry.U8()); (void)attributes;
        if (type != 0x93) continue;
        AVC_FIELD(header, entry.Take(5)); (void)header;
        while (entry.Remaining()) {
            const auto base = entry.Offset();
            AVC_FIELD(bytes, entry.Take(entry.Remaining()));
            size_t used = 0;
            AVC_FIELD(block, AVCInfoBlock::Parse(bytes, used, base));
            if (block.GetType() == 0x000A) database[static_cast<uint16_t>(i)] = Text(block.GetPrimaryData());
            entry = ParseReader(bytes.subspan(used), base + used);
        }
    }
    auto end = entries.End(); if (!end) return std::unexpected(end.error());
    return database;
}
Parsed<std::vector<uint16_t>> AudioSubunitDescriptorParser::ParseChildListIds(
    std::span<const uint8_t> data, uint8_t listIdSize, uint8_t objectIdSize) noexcept {
    if (listIdSize != 2) return std::unexpected(ParseError{0, ParseErrorKind::InvalidValue});
    AVC_FIELD(entries, ListEntries(data));
    AVC_FIELD(count, entries.BE16());
    std::vector<uint16_t> ids;
    for (size_t i = 0; i < count; ++i) {
        AVC_FIELD(entry, entries.Section());
        AVC_FIELD(type, entry.U8()); (void)type;
        AVC_FIELD(attributes, entry.U8());
        if (attributes & 0x20) { AVC_FIELD(id, entry.BE16()); ids.push_back(id); }
        if (data[3] & 0x10) {
            if (!objectIdSize) return std::unexpected(ParseError{entry.Offset(), ParseErrorKind::InvalidValue});
            AVC_FIELD(object, entry.Take(objectIdSize)); (void)object;
        }
        AVC_FIELD(specific, entry.Section()); (void)specific;
    }
    auto end = entries.End(); if (!end) return std::unexpected(end.error());
    return ids;
}
void AudioSubunitDescriptorParser::ResolveNames(AudioSubunitIdentifier& identifier, const TextDatabase& database) noexcept {
    for (auto& block : identifier.functionBlocks)
        if (auto found = database.find(block.nameIndex); found != database.end()) block.name = found->second;
}
#undef AVC_FIELD
} // namespace ASFW::Protocols::AVC::Descriptors
