#include "SelfIDStreamParser.hpp"

#include <algorithm>
#include <map>

namespace ASFW::Driver {

std::expected<std::vector<SelfIDNodeRecord>, TopologyBuildError>
SelfIDStreamParser::Parse(const SelfIDCapture::Result& result) {
    if (!result.valid) {
        return std::unexpected(TopologyBuildError{
            TopologyBuildErrorCode::InvalidSelfID,
            "Self-ID capture result is invalid"});
    }

    if (result.quads.empty()) {
        return std::unexpected(TopologyBuildError{
            TopologyBuildErrorCode::EmptySequenceSet,
            "Self-ID quadlet buffer is empty"});
    }

    // IEEE 1394-2008 Annex P.2:
    // The first phase copies each node's self-ID port connection status into a
    // per-node data structure.
    
    std::map<uint8_t, SelfIDNodeRecord> nodeMap;

    for (const auto& seq : result.sequences) {
        if (seq.first >= result.quads.size() || seq.first + seq.second > result.quads.size()) {
            return std::unexpected(TopologyBuildError{
                TopologyBuildErrorCode::InvalidSelfID,
                "Self-ID sequence bounds exceed buffer"});
        }

        const uint32_t* quadlets = &result.quads[seq.first];
        const unsigned int count = seq.second;

        if (count == 0) continue;

        const uint32_t baseQuad = quadlets[0];
        const uint8_t phyId = ExtractPhyID(baseQuad);

        if (phyId >= kMaxFireWireNodes) {
            return std::unexpected(TopologyBuildError{
                TopologyBuildErrorCode::InvalidSelfID,
                "Self-ID packet contains out-of-range physical ID: " + std::to_string(phyId)});
        }

        if (nodeMap.contains(phyId)) {
            return std::unexpected(TopologyBuildError{
                TopologyBuildErrorCode::DuplicatePhysicalId,
                "Duplicate physical ID in Self-ID stream: " + std::to_string(phyId)});
        }

        SelfIDNodeRecord record{};
        record.physicalId = phyId;
        record.linkActive = IsLinkActive(baseQuad);
        record.contender = IsContender(baseQuad);
        record.initiatedReset = IsInitiatedReset(baseQuad);
        record.gapCount = ExtractGapCount(baseQuad);
        record.powerClass = static_cast<uint8_t>(ExtractPowerClass(baseQuad));
        record.speedCode = ExtractSpeedCode(baseQuad);
        record.maxSpeedMbps = DecodeSpeed(record.speedCode);
        record.baseRaw = baseQuad;
        record.hasBasePacket = true;

        // Port states p0, p1, p2 are in the base quadlet.
        record.ports[0] = ExtractPortState(baseQuad, 0);
        record.ports[1] = ExtractPortState(baseQuad, 1);
        record.ports[2] = ExtractPortState(baseQuad, 2);
        record.portCount = 3;

        // Extended quadlets contain p3..p15.
        for (unsigned int i = 1; i < count; ++i) {
            const uint32_t extQuad = quadlets[i];
            const uint8_t seq = ExtractSeq(extQuad);
            
            // IEEE 1394-2008 Figure 16-11:
            // Extended packet 0 (n=0) contains p3..p10.
            // Extended packet 1 (n=1) contains p11..p15.
            if (seq == 0) {
                for (unsigned int p = 0; p < 8; ++p) {
                    // Figure 16-11: pa (bits 23:22) to ph (bits 9:8)
                    const unsigned int shift = 22 - (p * 2);
                    record.ports[3 + p] = DecodePort((extQuad >> shift) & 0x3u);
                }
                record.portCount = std::max<uint8_t>(record.portCount, 11);
            } else if (seq == 1) {
                for (unsigned int p = 0; p < 5; ++p) {
                    // Figure 16-11: pi (bits 23:22) to pm (bits 15:14)
                    const unsigned int shift = 22 - (p * 2);
                    record.ports[11 + p] = DecodePort((extQuad >> shift) & 0x3u);
                }
                record.portCount = std::max<uint8_t>(record.portCount, 16);
            }
        }

        // Clamp portCount to highest active/parent/child port observed,
        // or stay at 3 if only base packet was present.
        uint8_t actualCount = 0;
        for (uint8_t p = 0; p < kMaxPhyPorts; ++p) {
            if (record.ports[p] != PortState::NotPresent) {
                actualCount = p + 1;
            }
        }
        record.portCount = actualCount;

        nodeMap[phyId] = record;
    }

    if (nodeMap.empty()) {
        return std::unexpected(TopologyBuildError{
            TopologyBuildErrorCode::EmptySequenceSet,
            "No valid Self-ID sequences found in stream"});
    }

    std::vector<SelfIDNodeRecord> records;
    records.reserve(nodeMap.size());
    for (auto& [id, record] : nodeMap) {
        records.push_back(std::move(record));
    }

    if (!ValidateContiguousPhysicalIds(records)) {
        return std::unexpected(TopologyBuildError{
            TopologyBuildErrorCode::NonContiguousPhysicalIds,
            "Self-ID physical IDs are not contiguous from 0 to root"});
    }

    return records;
}

bool SelfIDStreamParser::ValidateContiguousPhysicalIds(const std::vector<SelfIDNodeRecord>& records) noexcept {
    if (records.empty()) return false;
    for (size_t i = 0; i < records.size(); ++i) {
        if (records[i].physicalId != static_cast<uint8_t>(i)) {
            return false;
        }
    }
    return true;
}

} // namespace ASFW::Driver
