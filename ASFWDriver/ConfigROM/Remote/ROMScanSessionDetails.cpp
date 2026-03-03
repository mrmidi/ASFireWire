#include "ROMScanSession.hpp"

#include "../../Common/FWCommon.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"
#include "../Common/ConfigROMConstants.hpp"
#include "../ConfigROMParser.hpp"

#include <DriverKit/IOLib.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ASFW::Discovery {

struct ROMScanSession::DetailsDiscovery : std::enable_shared_from_this<DetailsDiscovery> {
    using DirectoryEntry = ConfigROMParser::DirectoryEntry;

    struct DescriptorRef {
        uint8_t keyType{0};
        ASFW::ConfigROM::QuadletCount targetRel{0};
    };

    struct TextDescriptorFetcher : std::enable_shared_from_this<TextDescriptorFetcher> {
        ROMScanSession* session{nullptr};
        uint8_t nodeId{0xFF};
        ASFW::ConfigROM::QuadletOffset absOffset{0};
        uint8_t keyType{0};
        std::function<void(std::string)> completion;

        uint16_t leafLen{0};
        uint16_t dirLen{0};
        std::vector<ASFW::ConfigROM::QuadletOffset> candidates;
        size_t candidateIndex{0};

        enum class Mode : uint8_t { Invalid, Leaf, DescriptorDirectory } mode{Mode::Invalid};

        static void Start(ROMScanSession& session, uint8_t nodeId,
                          ASFW::ConfigROM::QuadletOffset absOffset, uint8_t keyType,
                          std::function<void(std::string)> completion);

      private:
        void StartImpl();
        void EnsureHeader();
        void OnHeaderReady(bool ok);

        void EnsureLeafPayload();
        void OnLeafPayloadReady(bool ok);
        void ParseLeafAndFinish();

        void EnsureDirectoryData();
        void OnDirectoryDataReady(bool ok);
        void ParseDirectoryAndFetchCandidates();
        void FetchNextCandidate();

        [[nodiscard]] ROMScanNodeStateMachine* FindNode() const;
        void Finish(std::string text);
    };

    static void Start(ROMScanSession& session, uint8_t nodeId,
                      ASFW::ConfigROM::QuadletOffset rootDirStart, std::vector<uint32_t> rootDirBE);

    void StartImpl();
    void EnsureRootDirPrefix();
    void OnRootDirPrefixReady(bool ok);
    void AppendRootDirAndParse();

    void StartVendorFetch();
    void OnVendorFetched(std::string text);
    void StartModelFetch();
    void OnModelFetched(std::string text);

    void StartNextUnitDir();
    void EnsureUnitDirHeader();
    void OnUnitDirHeaderReady(bool ok);
    void EnsureUnitDirData();
    void OnUnitDirDataReady(bool ok);
    void MaybeFetchUnitModelName();
    void OnUnitModelNameFetched(std::string text);

    void FinalizeNodeDiscovery();

  private:
    enum class Step : uint8_t {
        Start,
        EnsureRootPrefix,
        ParseRoot,
        FetchVendor,
        FetchModel,
        UnitHeader,
        UnitData,
        UnitModelName,
        Finalize,
    };

    [[nodiscard]] ROMScanNodeStateMachine* FindNode() const;

    [[nodiscard]] static std::vector<DirectoryEntry>
    ParseDirectoryBestEffort(std::span<const uint32_t> dirBE, uint32_t entryCap);
    [[nodiscard]] static std::optional<DescriptorRef>
    FindDescriptorRef(std::span<const DirectoryEntry> entries, uint8_t ownerKeyId);

    ROMScanSession* session{nullptr};
    uint8_t nodeId{0xFF};
    ASFW::ConfigROM::QuadletOffset rootDirStart{0};
    std::vector<uint32_t> rootDirBE;
    Step step{Step::Start};

    std::vector<DirectoryEntry> rootEntries;
    std::optional<DescriptorRef> vendorRef;
    std::optional<DescriptorRef> modelRef;
    std::vector<ASFW::ConfigROM::QuadletCount> unitDirRelOffsets;
    size_t unitIndex{0};

    ASFW::ConfigROM::QuadletOffset absUnitDir{0};
    ASFW::ConfigROM::QuadletCount unitRel{0};
    uint16_t unitDirLen{0};
    UnitDirectory parsedUnit{};
    std::optional<DescriptorRef> unitModelRef;
};

// =============================================================================
// TextDescriptorFetcher
// =============================================================================

void ROMScanSession::DetailsDiscovery::TextDescriptorFetcher::Start(
    ROMScanSession& session, uint8_t nodeId, ASFW::ConfigROM::QuadletOffset absOffset,
    uint8_t keyType, std::function<void(std::string)> completion) {
    auto state = std::make_shared<TextDescriptorFetcher>();
    state->session = &session;
    state->nodeId = nodeId;
    state->absOffset = absOffset;
    state->keyType = keyType;
    state->completion = std::move(completion);
    state->StartImpl();
}

void ROMScanSession::DetailsDiscovery::TextDescriptorFetcher::StartImpl() {
    if (session == nullptr) {
        Finish("");
        return;
    }

    ASFW_LOG_V3(ConfigROM, "TextDescriptorFetcher: start node=%u keyType=%u abs=%u", nodeId,
                keyType, absOffset.value);

    if (keyType == ASFW::FW::EntryType::kLeaf) {
        mode = Mode::Leaf;
        EnsureHeader();
        return;
    }

    if (keyType == ASFW::FW::EntryType::kDirectory) {
        mode = Mode::DescriptorDirectory;
        EnsureHeader();
        return;
    }

    ASFW_LOG_V3(ConfigROM, "TextDescriptorFetcher: unsupported keyType=%u (node=%u)", keyType,
                nodeId);
    Finish("");
}

ROMScanNodeStateMachine* ROMScanSession::DetailsDiscovery::TextDescriptorFetcher::FindNode() const {
    if (session == nullptr) {
        return nullptr;
    }
    return session->FindNode(nodeId);
}

void ROMScanSession::DetailsDiscovery::TextDescriptorFetcher::EnsureHeader() {
    if (session == nullptr) {
        Finish("");
        return;
    }

    session->EnsurePrefix(nodeId, ASFW::ConfigROM::QuadletCount{absOffset.value + 1U},
                          [self = shared_from_this()](bool ok) { self->OnHeaderReady(ok); });
}

void ROMScanSession::DetailsDiscovery::TextDescriptorFetcher::OnHeaderReady(bool ok) {
    if (!ok || session == nullptr) {
        ASFW_LOG_V3(ConfigROM, "TextDescriptorFetcher: header EnsurePrefix failed (node=%u abs=%u)",
                    nodeId, absOffset.value);
        Finish("");
        return;
    }

    auto* node = FindNode();
    if (node == nullptr || absOffset.value >= node->ROM().rawQuadlets.size()) {
        Finish("");
        return;
    }

    const uint32_t hdr = OSSwapBigToHostInt32(node->ROM().rawQuadlets[absOffset.value]);

    if (mode == Mode::Leaf) {
        leafLen = static_cast<uint16_t>((hdr >> 16) & 0xFFFFU);
        ASFW_LOG_V3(ConfigROM, "TextDescriptorFetcher: leaf header node=%u abs=%u len=%u", nodeId,
                    absOffset.value, leafLen);
        if (leafLen == 0) {
            Finish("");
            return;
        }
        EnsureLeafPayload();
        return;
    }

    if (mode == Mode::DescriptorDirectory) {
        dirLen = static_cast<uint16_t>((hdr >> 16) & 0xFFFFU);
        ASFW_LOG_V3(ConfigROM, "TextDescriptorFetcher: dir header node=%u abs=%u len=%u", nodeId,
                    absOffset.value, dirLen);
        if (dirLen == 0) {
            Finish("");
            return;
        }
        dirLen = std::min(dirLen, static_cast<uint16_t>(32U));
        EnsureDirectoryData();
        return;
    }

    Finish("");
}

void ROMScanSession::DetailsDiscovery::TextDescriptorFetcher::EnsureLeafPayload() {
    if (session == nullptr) {
        Finish("");
        return;
    }

    const ASFW::ConfigROM::QuadletCount leafEndExclusive{absOffset.value + 1U +
                                                         static_cast<uint32_t>(leafLen)};
    session->EnsurePrefix(nodeId, leafEndExclusive,
                          [self = shared_from_this()](bool ok) { self->OnLeafPayloadReady(ok); });
}

void ROMScanSession::DetailsDiscovery::TextDescriptorFetcher::OnLeafPayloadReady(bool ok) {
    if (!ok || session == nullptr) {
        ASFW_LOG_V3(ConfigROM,
                    "TextDescriptorFetcher: payload EnsurePrefix failed (node=%u abs=%u)", nodeId,
                    absOffset.value);
        Finish("");
        return;
    }

    ParseLeafAndFinish();
}

void ROMScanSession::DetailsDiscovery::TextDescriptorFetcher::ParseLeafAndFinish() {
    auto* node = FindNode();
    if (node == nullptr) {
        Finish("");
        return;
    }

    const auto span =
        std::span<const uint32_t>(node->ROM().rawQuadlets.data(), node->ROM().rawQuadlets.size());
    auto textRes = ConfigROMParser::ParseTextDescriptorLeaf(span, absOffset.value);
    if (!textRes) {
        ASFW_LOG_V3(ConfigROM,
                    "TextDescriptorFetcher: ParseTextDescriptorLeaf failed (node=%u abs=%u "
                    "code=%u offset=%u)",
                    nodeId, absOffset.value, static_cast<uint8_t>(textRes.error().code),
                    textRes.error().offsetQuadlets);
        Finish("");
        return;
    }

    Finish(std::move(*textRes));
}

void ROMScanSession::DetailsDiscovery::TextDescriptorFetcher::EnsureDirectoryData() {
    if (session == nullptr) {
        Finish("");
        return;
    }

    const ASFW::ConfigROM::QuadletCount dirEndExclusive{absOffset.value + 1U +
                                                        static_cast<uint32_t>(dirLen)};
    session->EnsurePrefix(nodeId, dirEndExclusive,
                          [self = shared_from_this()](bool ok) { self->OnDirectoryDataReady(ok); });
}

void ROMScanSession::DetailsDiscovery::TextDescriptorFetcher::OnDirectoryDataReady(bool ok) {
    if (!ok || session == nullptr) {
        ASFW_LOG_V3(ConfigROM,
                    "TextDescriptorFetcher: dir EnsurePrefix failed (node=%u abs=%u len=%u)",
                    nodeId, absOffset.value, dirLen);
        Finish("");
        return;
    }

    ParseDirectoryAndFetchCandidates();
}

void ROMScanSession::DetailsDiscovery::TextDescriptorFetcher::ParseDirectoryAndFetchCandidates() {
    auto* node = FindNode();
    if (node == nullptr) {
        Finish("");
        return;
    }

    const auto* base = node->ROM().rawQuadlets.data();
    const size_t available = node->ROM().rawQuadlets.size();
    const size_t required = static_cast<size_t>(absOffset.value) + 1U + static_cast<size_t>(dirLen);
    if (required > available) {
        Finish("");
        return;
    }

    const auto dirSpan =
        std::span<const uint32_t>(base + absOffset.value, 1U + static_cast<size_t>(dirLen));
    auto entriesRes = ConfigROMParser::ParseDirectory(dirSpan, 32);
    if (!entriesRes) {
        ASFW_LOG_V3(
            ConfigROM,
            "TextDescriptorFetcher: ParseDirectory failed (node=%u abs=%u code=%u offset=%u)",
            nodeId, absOffset.value, static_cast<uint8_t>(entriesRes.error().code),
            entriesRes.error().offsetQuadlets);
        Finish("");
        return;
    }

    candidates.clear();
    candidates.reserve(entriesRes->size());
    for (const auto& entry : *entriesRes) {
        if (!entry.hasTarget || entry.targetRel == 0) {
            continue;
        }
        if (entry.keyId != ASFW::FW::ConfigKey::kTextualDescriptor ||
            entry.keyType != ASFW::FW::EntryType::kLeaf) {
            continue;
        }

        candidates.push_back(absOffset + ASFW::ConfigROM::QuadletCount{entry.targetRel});
    }

    ASFW_LOG_V3(ConfigROM, "TextDescriptorFetcher: dir candidates=%zu (node=%u abs=%u)",
                candidates.size(), nodeId, absOffset.value);

    candidateIndex = 0;
    FetchNextCandidate();
}

void ROMScanSession::DetailsDiscovery::TextDescriptorFetcher::FetchNextCandidate() {
    if (session == nullptr) {
        Finish("");
        return;
    }

    if (candidateIndex >= candidates.size()) {
        Finish("");
        return;
    }

    const auto leafAbs = candidates[candidateIndex];
    ASFW_LOG_V3(ConfigROM, "TextDescriptorFetcher: candidate %zu/%zu leafAbs=%u (node=%u)",
                candidateIndex + 1, candidates.size(), leafAbs.value, nodeId);

    TextDescriptorFetcher::Start(
        *session, nodeId, leafAbs, ASFW::FW::EntryType::kLeaf,
        [self = shared_from_this()](std::string text) mutable {
            if (!text.empty()) {
                ASFW_LOG_V3(ConfigROM, "TextDescriptorFetcher: candidate ok (node=%u len=%zu)",
                            self->nodeId, text.size());
                self->Finish(std::move(text));
                return;
            }

            ASFW_LOG_V3(ConfigROM, "TextDescriptorFetcher: candidate empty (node=%u)",
                        self->nodeId);
            ++self->candidateIndex;
            self->FetchNextCandidate();
        });
}

void ROMScanSession::DetailsDiscovery::TextDescriptorFetcher::Finish(std::string text) {
    if (completion) {
        completion(std::move(text));
    }
    completion = nullptr;
}

// =============================================================================
// DetailsDiscovery helpers
// =============================================================================

ROMScanNodeStateMachine* ROMScanSession::DetailsDiscovery::FindNode() const {
    if (session == nullptr) {
        return nullptr;
    }
    return session->FindNode(nodeId);
}

std::vector<ROMScanSession::DetailsDiscovery::DirectoryEntry>
ROMScanSession::DetailsDiscovery::ParseDirectoryBestEffort(std::span<const uint32_t> dirBE,
                                                           uint32_t entryCap) {
    auto entries = ConfigROMParser::ParseDirectory(dirBE, entryCap);
    if (!entries) {
        return {};
    }
    return std::move(*entries);
}

std::optional<ROMScanSession::DetailsDiscovery::DescriptorRef>
ROMScanSession::DetailsDiscovery::FindDescriptorRef(std::span<const DirectoryEntry> entries,
                                                    uint8_t ownerKeyId) {
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& ownerEntry = entries[i];
        if (ownerEntry.keyType != ASFW::FW::EntryType::kImmediate ||
            ownerEntry.keyId != ownerKeyId) {
            continue;
        }
        if (i + 1 >= entries.size()) {
            return std::nullopt;
        }

        const auto& descriptorEntry = entries[i + 1];
        if (descriptorEntry.keyId != ASFW::FW::ConfigKey::kTextualDescriptor) {
            return std::nullopt;
        }
        if (descriptorEntry.keyType != ASFW::FW::EntryType::kLeaf &&
            descriptorEntry.keyType != ASFW::FW::EntryType::kDirectory) {
            return std::nullopt;
        }
        if (!descriptorEntry.hasTarget || descriptorEntry.targetRel == 0) {
            return std::nullopt;
        }
        return DescriptorRef{.keyType = descriptorEntry.keyType,
                             .targetRel = ASFW::ConfigROM::QuadletCount{descriptorEntry.targetRel}};
    }
    return std::nullopt;
}

// =============================================================================
// DetailsDiscovery flow
// =============================================================================

void ROMScanSession::DetailsDiscovery::Start(ROMScanSession& session, uint8_t nodeId,
                                             ASFW::ConfigROM::QuadletOffset rootDirStart,
                                             std::vector<uint32_t> rootDirBE) {
    auto state = std::make_shared<DetailsDiscovery>();
    state->session = &session;
    state->nodeId = nodeId;
    state->rootDirStart = rootDirStart;
    state->rootDirBE = std::move(rootDirBE);
    state->StartImpl();
}

void ROMScanSession::DetailsDiscovery::StartImpl() {
    if (session == nullptr) {
        return;
    }

    step = Step::Start;
    ASFW_LOG_V3(ConfigROM, "DetailsDiscovery: start node=%u rootDirStart=%u rootDirBE=%zu", nodeId,
                rootDirStart.value, rootDirBE.size());

    auto* node = FindNode();
    if (node == nullptr) {
        session->MaybeFinish();
        session->Pump();
        return;
    }

    if (!ROMScanSession::TransitionNodeState(*node, ROMScanNodeStateMachine::State::ReadingDetails,
                                             "RootDir parsed enter details discovery")) {
        session->MaybeFinish();
        session->Pump();
        return;
    }

    EnsureRootDirPrefix();
}

void ROMScanSession::DetailsDiscovery::EnsureRootDirPrefix() {
    if (session == nullptr) {
        return;
    }

    step = Step::EnsureRootPrefix;
    session->EnsurePrefix(nodeId, ASFW::ConfigROM::QuadletCount{rootDirStart.value},
                          [self = shared_from_this()](bool ok) { self->OnRootDirPrefixReady(ok); });
}

void ROMScanSession::DetailsDiscovery::OnRootDirPrefixReady(bool ok) {
    if (session == nullptr) {
        return;
    }

    auto* node = FindNode();
    if (node == nullptr) {
        session->MaybeFinish();
        session->Pump();
        return;
    }

    if (!ok) {
        ASFW_LOG_V3(ConfigROM,
                    "DetailsDiscovery: EnsurePrefix failed (node=%u required=%u have=%zu)", nodeId,
                    rootDirStart.value, node->ROM().rawQuadlets.size());
    }

    AppendRootDirAndParse();
}

void ROMScanSession::DetailsDiscovery::AppendRootDirAndParse() {
    if (session == nullptr) {
        return;
    }

    step = Step::ParseRoot;
    auto* node = FindNode();
    if (node == nullptr) {
        session->MaybeFinish();
        session->Pump();
        return;
    }

    if (!rootDirBE.empty()) {
        auto& rawQuadlets = node->MutableROM().rawQuadlets;
        rawQuadlets.reserve(rawQuadlets.size() + rootDirBE.size());
        rawQuadlets.insert(rawQuadlets.end(), rootDirBE.begin(), rootDirBE.end());
    }

    const auto entries = ParseDirectoryBestEffort(rootDirBE, 64);
    if (entries.empty()) {
        ASFW_LOG_V3(ConfigROM, "DetailsDiscovery: root directory parse failed/empty (node=%u)",
                    nodeId);
    }

    rootEntries = entries;
    vendorRef = FindDescriptorRef(rootEntries, ASFW::FW::ConfigKey::kModuleVendorId);
    modelRef = FindDescriptorRef(rootEntries, ASFW::FW::ConfigKey::kModelId);

    unitDirRelOffsets.clear();
    for (const auto& entry : rootEntries) {
        if (entry.keyType == ASFW::FW::EntryType::kDirectory &&
            entry.keyId == ASFW::FW::ConfigKey::kUnitDirectory && entry.hasTarget &&
            entry.targetRel != 0) {
            unitDirRelOffsets.push_back(ASFW::ConfigROM::QuadletCount{entry.targetRel});
        }
    }

    ASFW_LOG_V3(
        ConfigROM,
        "DetailsDiscovery: root parsed node=%u entries=%zu vendorRef=%d modelRef=%d units=%zu",
        nodeId, rootEntries.size(), vendorRef.has_value() ? 1 : 0, modelRef.has_value() ? 1 : 0,
        unitDirRelOffsets.size());

    rootDirBE.clear();
    rootDirBE.shrink_to_fit();

    StartVendorFetch();
}

void ROMScanSession::DetailsDiscovery::StartVendorFetch() {
    if (session == nullptr) {
        return;
    }

    step = Step::FetchVendor;
    if (!vendorRef.has_value()) {
        OnVendorFetched("");
        return;
    }

    const auto absOffset = rootDirStart + vendorRef->targetRel;
    ASFW_LOG_V3(ConfigROM, "DetailsDiscovery: fetch vendor node=%u abs=%u keyType=%u", nodeId,
                absOffset.value, vendorRef->keyType);
    TextDescriptorFetcher::Start(
        *session, nodeId, absOffset, vendorRef->keyType,
        [self = shared_from_this()](std::string text) { self->OnVendorFetched(std::move(text)); });
}

void ROMScanSession::DetailsDiscovery::OnVendorFetched(std::string text) {
    if (session == nullptr) {
        return;
    }

    if (auto* node = FindNode(); node != nullptr) {
        if (!text.empty()) {
            node->MutableROM().vendorName = std::move(text);
        }
        ASFW_LOG_V3(ConfigROM, "DetailsDiscovery: vendor done node=%u len=%zu", nodeId,
                    node->ROM().vendorName.size());
    }

    StartModelFetch();
}

void ROMScanSession::DetailsDiscovery::StartModelFetch() {
    if (session == nullptr) {
        return;
    }

    step = Step::FetchModel;
    if (!modelRef.has_value()) {
        OnModelFetched("");
        return;
    }

    const auto absOffset = rootDirStart + modelRef->targetRel;
    ASFW_LOG_V3(ConfigROM, "DetailsDiscovery: fetch model node=%u abs=%u keyType=%u", nodeId,
                absOffset.value, modelRef->keyType);
    TextDescriptorFetcher::Start(
        *session, nodeId, absOffset, modelRef->keyType,
        [self = shared_from_this()](std::string text) { self->OnModelFetched(std::move(text)); });
}

void ROMScanSession::DetailsDiscovery::OnModelFetched(std::string text) {
    if (session == nullptr) {
        return;
    }

    if (auto* node = FindNode(); node != nullptr) {
        if (!text.empty()) {
            node->MutableROM().modelName = std::move(text);
        }
        ASFW_LOG_V3(ConfigROM, "DetailsDiscovery: model done node=%u len=%zu", nodeId,
                    node->ROM().modelName.size());
    }

    unitIndex = 0;
    StartNextUnitDir();
}

void ROMScanSession::DetailsDiscovery::StartNextUnitDir() {
    if (session == nullptr) {
        return;
    }

    if (unitIndex >= unitDirRelOffsets.size()) {
        FinalizeNodeDiscovery();
        return;
    }

    unitRel = unitDirRelOffsets[unitIndex];
    absUnitDir = rootDirStart + unitRel;
    unitDirLen = 0;
    parsedUnit = UnitDirectory{};
    unitModelRef = std::nullopt;

    ASFW_LOG_V3(ConfigROM, "DetailsDiscovery: unit[%zu/%zu] start node=%u abs=%u rel=%u",
                unitIndex + 1, unitDirRelOffsets.size(), nodeId, absUnitDir.value, unitRel.value);
    EnsureUnitDirHeader();
}

void ROMScanSession::DetailsDiscovery::EnsureUnitDirHeader() {
    if (session == nullptr) {
        return;
    }

    step = Step::UnitHeader;
    session->EnsurePrefix(nodeId, ASFW::ConfigROM::QuadletCount{absUnitDir.value + 1U},
                          [self = shared_from_this()](bool ok) { self->OnUnitDirHeaderReady(ok); });
}

void ROMScanSession::DetailsDiscovery::OnUnitDirHeaderReady(bool ok) {
    if (session == nullptr) {
        return;
    }

    if (!ok) {
        ASFW_LOG_V3(ConfigROM, "DetailsDiscovery: unit header EnsurePrefix failed (node=%u abs=%u)",
                    nodeId, absUnitDir.value);
        ++unitIndex;
        StartNextUnitDir();
        return;
    }

    const auto* node = FindNode();
    if (node == nullptr || absUnitDir.value >= node->ROM().rawQuadlets.size()) {
        FinalizeNodeDiscovery();
        return;
    }

    const uint32_t hdr = OSSwapBigToHostInt32(node->ROM().rawQuadlets[absUnitDir.value]);
    unitDirLen = static_cast<uint16_t>((hdr >> 16) & 0xFFFFU);
    ASFW_LOG_V3(ConfigROM, "DetailsDiscovery: unit header node=%u abs=%u len=%u", nodeId,
                absUnitDir.value, unitDirLen);

    if (unitDirLen == 0) {
        ++unitIndex;
        StartNextUnitDir();
        return;
    }

    unitDirLen = std::min(unitDirLen, static_cast<uint16_t>(32U));
    EnsureUnitDirData();
}

void ROMScanSession::DetailsDiscovery::EnsureUnitDirData() {
    if (session == nullptr) {
        return;
    }

    step = Step::UnitData;
    const ASFW::ConfigROM::QuadletCount dirEndExclusive{absUnitDir.value + 1U +
                                                        static_cast<uint32_t>(unitDirLen)};
    session->EnsurePrefix(nodeId, dirEndExclusive,
                          [self = shared_from_this()](bool ok) { self->OnUnitDirDataReady(ok); });
}

void ROMScanSession::DetailsDiscovery::OnUnitDirDataReady(bool ok) {
    if (session == nullptr) {
        return;
    }

    if (!ok) {
        ASFW_LOG_V3(ConfigROM,
                    "DetailsDiscovery: unit data EnsurePrefix failed (node=%u abs=%u len=%u)",
                    nodeId, absUnitDir.value, unitDirLen);
        ++unitIndex;
        StartNextUnitDir();
        return;
    }

    auto* node = FindNode();
    if (node == nullptr ||
        absUnitDir.value + static_cast<uint32_t>(unitDirLen) >= node->ROM().rawQuadlets.size()) {
        FinalizeNodeDiscovery();
        return;
    }

    std::vector<uint32_t> unitDirBE;
    unitDirBE.reserve(static_cast<size_t>(unitDirLen) + 1U);
    for (uint32_t i = 0; i <= static_cast<uint32_t>(unitDirLen); ++i) {
        unitDirBE.push_back(node->ROM().rawQuadlets[absUnitDir.value + i]);
    }

    const auto unitEntries = ParseDirectoryBestEffort(unitDirBE, 32);
    if (unitEntries.empty()) {
        ASFW_LOG_V3(ConfigROM,
                    "DetailsDiscovery: unit directory parse failed/empty (node=%u abs=%u)", nodeId,
                    absUnitDir.value);
    }

    parsedUnit = UnitDirectory{};
    parsedUnit.offsetQuadlets = unitRel.value;
    for (const auto& entry : unitEntries) {
        if (entry.keyType != ASFW::FW::EntryType::kImmediate) {
            continue;
        }
        switch (entry.keyId) {
        case ASFW::FW::ConfigKey::kUnitSpecId:
            parsedUnit.unitSpecId = entry.value;
            break;
        case ASFW::FW::ConfigKey::kUnitSwVersion:
            parsedUnit.unitSwVersion = entry.value;
            break;
        case ASFW::FW::ConfigKey::kUnitDependentInfo:
            parsedUnit.logicalUnitNumber = entry.value;
            break;
        case ASFW::FW::ConfigKey::kModelId:
            parsedUnit.modelId = entry.value;
            break;
        default:
            break;
        }
    }

    unitModelRef = FindDescriptorRef(unitEntries, ASFW::FW::ConfigKey::kModelId);
    if (!unitModelRef) {
        node->MutableROM().unitDirectories.push_back(std::move(parsedUnit));
        ++unitIndex;
        StartNextUnitDir();
        return;
    }

    MaybeFetchUnitModelName();
}

void ROMScanSession::DetailsDiscovery::MaybeFetchUnitModelName() {
    if (session == nullptr) {
        return;
    }

    step = Step::UnitModelName;
    if (!unitModelRef.has_value()) {
        ++unitIndex;
        StartNextUnitDir();
        return;
    }

    const auto absOffset = absUnitDir + unitModelRef->targetRel;
    ASFW_LOG_V3(ConfigROM, "DetailsDiscovery: fetch unit model node=%u abs=%u keyType=%u", nodeId,
                absOffset.value, unitModelRef->keyType);
    TextDescriptorFetcher::Start(*session, nodeId, absOffset, unitModelRef->keyType,
                                 [self = shared_from_this()](std::string text) {
                                     self->OnUnitModelNameFetched(std::move(text));
                                 });
}

void ROMScanSession::DetailsDiscovery::OnUnitModelNameFetched(std::string text) {
    if (session == nullptr) {
        return;
    }

    auto* node = FindNode();
    if (node == nullptr) {
        FinalizeNodeDiscovery();
        return;
    }

    if (!text.empty()) {
        parsedUnit.modelName = std::move(text);
    }
    ASFW_LOG_V3(ConfigROM, "DetailsDiscovery: unit model done node=%u len=%zu", nodeId,
                parsedUnit.modelName.has_value() ? parsedUnit.modelName->size() : 0U);

    node->MutableROM().unitDirectories.push_back(std::move(parsedUnit));

    ++unitIndex;
    StartNextUnitDir();
}

void ROMScanSession::DetailsDiscovery::FinalizeNodeDiscovery() {
    if (session == nullptr) {
        return;
    }

    step = Step::Finalize;
    auto* node = FindNode();
    if (node == nullptr) {
        ASFW_LOG_V3(ConfigROM, "DetailsDiscovery: finalize node missing (node=%u)", nodeId);
        session->MaybeFinish();
        session->Pump();
        return;
    }

    ASFW_LOG_V3(ConfigROM, "DetailsDiscovery: done node=%u vendorLen=%zu modelLen=%zu units=%zu",
                nodeId, node->ROM().vendorName.size(), node->ROM().modelName.size(),
                node->ROM().unitDirectories.size());

    session->speedPolicy_.RecordSuccess(nodeId, node->CurrentSpeed());
    if (!ROMScanSession::TransitionNodeState(*node, ROMScanNodeStateMachine::State::Complete,
                                             "FinalizeNodeDiscovery complete")) {
        session->MaybeFinish();
        session->Pump();
        return;
    }

    session->completedROMs_.push_back(std::move(node->MutableROM()));
    session->MaybeFinish();
    session->Pump();
}

void ROMScanSession::StartDetailsDiscovery(uint8_t nodeId,
                                           ASFW::ConfigROM::QuadletOffset rootDirStart,
                                           std::vector<uint32_t> rootDirBE) {
    DetailsDiscovery::Start(*this, nodeId, rootDirStart, std::move(rootDirBE));
}

} // namespace ASFW::Discovery
