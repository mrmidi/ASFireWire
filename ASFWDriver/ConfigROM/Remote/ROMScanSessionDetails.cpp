#include "ROMScanSession.hpp"

#include "../../Common/FWCommon.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"
#include "../Common/ConfigROMConstants.hpp"
#include "../ConfigROMParser.hpp"

#include <DriverKit/IOLib.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace ASFW::Discovery {

struct ROMScanSession::DiscoveryFlow {
    using DirectoryEntry = ConfigROMParser::DirectoryEntry;

    struct DescriptorRef {
        uint8_t keyType{0};
        ASFW::ConfigROM::QuadletCount targetRel{0};
    };

    struct UnitDirStepContext {
        uint8_t nodeId{0xFF};
        ASFW::ConfigROM::QuadletOffset rootDirStart{0};
        std::vector<ASFW::ConfigROM::QuadletCount> unitDirRelOffsets;
        size_t index{0};
        ASFW::ConfigROM::QuadletOffset absUnitDir{0};
        ASFW::ConfigROM::QuadletCount unitRel{0};
        uint16_t dirLen{0};
    };

    struct DescriptorDirTextState : std::enable_shared_from_this<DescriptorDirTextState> {
        ROMScanSession* session{nullptr};
        uint8_t nodeId{0xFF};
        ASFW::ConfigROM::QuadletOffset absDirOffset{0};
        std::function<void(std::string)> completion;

        uint16_t dirLen{0};
        std::vector<ASFW::ConfigROM::QuadletOffset> candidates;
        size_t candidateIndex{0};

        enum class Step : uint8_t { NeedHeader, NeedDirData } step{Step::NeedHeader};

        void Finish(std::string text);
        void FetchNextCandidate();
        void Advance(bool ok);
    };

    [[nodiscard]] static std::optional<DescriptorRef>
    FindDescriptorRef(std::span<const DirectoryEntry> entries, uint8_t ownerKeyId);
    [[nodiscard]] static std::vector<DirectoryEntry>
    ParseDirectoryBestEffort(std::span<const uint32_t> dirBE, uint32_t entryCap);

    static void FetchTextDescriptor(ROMScanSession& session, uint8_t nodeId,
                                    ASFW::ConfigROM::QuadletOffset absOffset, uint8_t keyType,
                                    std::function<void(std::string)> completion);
    static void FetchTextLeaf(ROMScanSession& session, uint8_t nodeId,
                              ASFW::ConfigROM::QuadletOffset absLeafOffset,
                              std::function<void(std::string)> completion);
    static void FetchDescriptorDirText(ROMScanSession& session, uint8_t nodeId,
                                       ASFW::ConfigROM::QuadletOffset absDirOffset,
                                       std::function<void(std::string)> completion);

    static void DiscoverDetails(ROMScanSession& session, uint8_t nodeId,
                                ASFW::ConfigROM::QuadletOffset rootDirStart,
                                std::vector<uint32_t> rootDirBE);
    static void OnDiscoverDetailsPrefixReady(ROMScanSession& session, uint8_t nodeId,
                                             ASFW::ConfigROM::QuadletOffset rootDirStart,
                                             const std::vector<uint32_t>& rootDirBE, bool prefixOk);
    static void DiscoverVendorName(ROMScanSession& session, uint8_t nodeId,
                                   ASFW::ConfigROM::QuadletOffset rootDirStart,
                                   std::optional<DescriptorRef> vendorRef,
                                   std::optional<DescriptorRef> modelRef,
                                   std::vector<ASFW::ConfigROM::QuadletCount> unitDirRelOffsets);
    static void DiscoverModelName(ROMScanSession& session, uint8_t nodeId,
                                  ASFW::ConfigROM::QuadletOffset rootDirStart,
                                  std::optional<DescriptorRef> modelRef,
                                  std::vector<ASFW::ConfigROM::QuadletCount> unitDirRelOffsets);
    static void DiscoverUnitDirectories(
        ROMScanSession& session, uint8_t nodeId, ASFW::ConfigROM::QuadletOffset rootDirStart,
        std::vector<ASFW::ConfigROM::QuadletCount> unitDirRelOffsets, size_t index);
    static void OnUnitDirHeaderReady(ROMScanSession& session, UnitDirStepContext context, bool ok);
    static void OnUnitDirDataReady(ROMScanSession& session, UnitDirStepContext context, bool ok);
    static void FinalizeNodeDiscovery(ROMScanSession& session, uint8_t nodeId);
};

std::vector<ROMScanSession::DiscoveryFlow::DirectoryEntry>
ROMScanSession::DiscoveryFlow::ParseDirectoryBestEffort(std::span<const uint32_t> dirBE,
                                                        uint32_t entryCap) {
    auto entries = ConfigROMParser::ParseDirectory(dirBE, entryCap);
    if (!entries) {
        return {};
    }
    return std::move(*entries);
}

std::optional<ROMScanSession::DiscoveryFlow::DescriptorRef>
ROMScanSession::DiscoveryFlow::FindDescriptorRef(std::span<const DirectoryEntry> entries,
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

void ROMScanSession::DiscoveryFlow::FetchTextDescriptor(
    ROMScanSession& session, uint8_t nodeId, ASFW::ConfigROM::QuadletOffset absOffset,
    uint8_t keyType, std::function<void(std::string)> completion) {
    if (keyType == ASFW::FW::EntryType::kLeaf) {
        FetchTextLeaf(session, nodeId, absOffset, std::move(completion));
    } else if (keyType == ASFW::FW::EntryType::kDirectory) {
        FetchDescriptorDirText(session, nodeId, absOffset, std::move(completion));
    } else if (completion) {
        completion("");
    }
}

void ROMScanSession::DiscoveryFlow::FetchTextLeaf(ROMScanSession& session, uint8_t nodeId,
                                                  ASFW::ConfigROM::QuadletOffset absLeafOffset,
                                                  std::function<void(std::string)> completion) {
    struct State : std::enable_shared_from_this<State> {
        ROMScanSession* session{nullptr};
        uint8_t nodeId{0xFF};
        ASFW::ConfigROM::QuadletOffset absLeafOffset{0};
        std::function<void(std::string)> completion;

        enum class Step : uint8_t { NeedHeader, NeedPayload } step{Step::NeedHeader};

        void Finish(std::string text) {
            if (completion) {
                completion(std::move(text));
            }
            completion = nullptr;
        }

        void Advance(bool ok) {
            if (!ok || session == nullptr) {
                Finish("");
                return;
            }

            const auto* node = session->FindNode(nodeId);
            if (node == nullptr || absLeafOffset.value >= node->ROM().rawQuadlets.size()) {
                Finish("");
                return;
            }

            if (step == Step::NeedHeader) {
                const uint32_t hdr =
                    OSSwapBigToHostInt32(node->ROM().rawQuadlets[absLeafOffset.value]);
                const auto leafLen = static_cast<uint16_t>((hdr >> 16) & 0xFFFFU);
                const ASFW::ConfigROM::QuadletCount leafEndExclusive{
                    absLeafOffset.value + 1U + static_cast<uint32_t>(leafLen)};

                step = Step::NeedPayload;
                session->EnsurePrefix(
                    nodeId, leafEndExclusive,
                    [self = shared_from_this()](bool ok2) { self->Advance(ok2); });
                return;
            }

            const auto span = std::span<const uint32_t>(node->ROM().rawQuadlets.data(),
                                                        node->ROM().rawQuadlets.size());
            auto textRes = ConfigROMParser::ParseTextDescriptorLeaf(span, absLeafOffset.value);
            Finish(textRes ? std::move(*textRes) : std::string{});
        }
    };

    auto state = std::make_shared<State>();
    state->session = &session;
    state->nodeId = nodeId;
    state->absLeafOffset = absLeafOffset;
    state->completion = std::move(completion);

    session.EnsurePrefix(nodeId, ASFW::ConfigROM::QuadletCount{absLeafOffset.value + 1U},
                         [state](bool ok) { state->Advance(ok); });
}

void ROMScanSession::DiscoveryFlow::DescriptorDirTextState::Finish(std::string text) {
    if (completion) {
        completion(std::move(text));
    }
    completion = nullptr;
}

void ROMScanSession::DiscoveryFlow::DescriptorDirTextState::FetchNextCandidate() {
    if (session == nullptr) {
        Finish("");
        return;
    }

    if (candidateIndex >= candidates.size()) {
        Finish("");
        return;
    }

    const auto leafAbs = candidates[candidateIndex];
    ROMScanSession::DiscoveryFlow::FetchTextLeaf(
        *session, nodeId, leafAbs, [self = shared_from_this()](std::string text) mutable {
            if (!text.empty()) {
                self->Finish(std::move(text));
                return;
            }

            ++self->candidateIndex;
            self->FetchNextCandidate();
        });
}

void ROMScanSession::DiscoveryFlow::DescriptorDirTextState::Advance(bool ok) {
    if (!ok || session == nullptr) {
        Finish("");
        return;
    }

    const auto* node = session->FindNode(nodeId);
    if (node == nullptr || absDirOffset.value >= node->ROM().rawQuadlets.size()) {
        Finish("");
        return;
    }

    if (step == Step::NeedHeader) {
        const uint32_t hdr = OSSwapBigToHostInt32(node->ROM().rawQuadlets[absDirOffset.value]);
        dirLen = static_cast<uint16_t>((hdr >> 16) & 0xFFFFU);
        if (dirLen == 0) {
            Finish("");
            return;
        }
        dirLen = std::min(dirLen, static_cast<uint16_t>(32U));

        const ASFW::ConfigROM::QuadletCount dirEndExclusive{absDirOffset.value + 1U +
                                                            static_cast<uint32_t>(dirLen)};

        step = Step::NeedDirData;
        session->EnsurePrefix(nodeId, dirEndExclusive,
                              [self = shared_from_this()](bool ok2) { self->Advance(ok2); });
        return;
    }

    const auto* base = node->ROM().rawQuadlets.data();
    const size_t available = node->ROM().rawQuadlets.size();
    if (static_cast<size_t>(absDirOffset.value) + 1U + static_cast<size_t>(dirLen) > available) {
        Finish("");
        return;
    }

    const auto dirSpan =
        std::span<const uint32_t>(base + absDirOffset.value, 1U + static_cast<size_t>(dirLen));
    auto entriesRes = ConfigROMParser::ParseDirectory(dirSpan, 32);
    if (!entriesRes) {
        Finish("");
        return;
    }

    auto entries = std::move(*entriesRes);
    candidates.clear();
    candidates.reserve(entries.size());
    for (const auto& entry : entries) {
        if (!entry.hasTarget || entry.targetRel == 0) {
            continue;
        }
        if (entry.keyId != ASFW::FW::ConfigKey::kTextualDescriptor ||
            entry.keyType != ASFW::FW::EntryType::kLeaf) {
            continue;
        }

        candidates.push_back(absDirOffset + ASFW::ConfigROM::QuadletCount{entry.targetRel});
    }

    FetchNextCandidate();
}

void ROMScanSession::DiscoveryFlow::FetchDescriptorDirText(
    ROMScanSession& session, uint8_t nodeId, ASFW::ConfigROM::QuadletOffset absDirOffset,
    std::function<void(std::string)> completion) {
    auto state = std::make_shared<DescriptorDirTextState>();
    state->session = &session;
    state->nodeId = nodeId;
    state->absDirOffset = absDirOffset;
    state->completion = std::move(completion);

    session.EnsurePrefix(nodeId, ASFW::ConfigROM::QuadletCount{absDirOffset.value + 1U},
                         [state](bool ok) { state->Advance(ok); });
}

void ROMScanSession::DiscoveryFlow::DiscoverVendorName(
    ROMScanSession& session, uint8_t nodeId, ASFW::ConfigROM::QuadletOffset rootDirStart,
    std::optional<DescriptorRef> vendorRef, std::optional<DescriptorRef> modelRef,
    std::vector<ASFW::ConfigROM::QuadletCount> unitDirRelOffsets) {
    if (!vendorRef) {
        DiscoverModelName(session, nodeId, rootDirStart, modelRef, std::move(unitDirRelOffsets));
        return;
    }

    FetchTextDescriptor(
        session, nodeId, rootDirStart + vendorRef->targetRel, vendorRef->keyType,
        [&session, nodeId, rootDirStart, modelRef,
         unitDirRelOffsets = std::move(unitDirRelOffsets)](std::string_view vendor) mutable {
            if (auto* node = session.FindNode(nodeId); node != nullptr && !vendor.empty()) {
                node->MutableROM().vendorName.assign(vendor);
            }
            DiscoverModelName(session, nodeId, rootDirStart, modelRef,
                              std::move(unitDirRelOffsets));
        });
}

void ROMScanSession::DiscoveryFlow::DiscoverModelName(
    ROMScanSession& session, uint8_t nodeId, ASFW::ConfigROM::QuadletOffset rootDirStart,
    std::optional<DescriptorRef> modelRef,
    std::vector<ASFW::ConfigROM::QuadletCount> unitDirRelOffsets) {
    if (!modelRef) {
        DiscoverUnitDirectories(session, nodeId, rootDirStart, std::move(unitDirRelOffsets), 0);
        return;
    }

    FetchTextDescriptor(
        session, nodeId, rootDirStart + modelRef->targetRel, modelRef->keyType,
        [&session, nodeId, rootDirStart,
         unitDirRelOffsets = std::move(unitDirRelOffsets)](std::string_view model) mutable {
            if (auto* node = session.FindNode(nodeId); node != nullptr && !model.empty()) {
                node->MutableROM().modelName.assign(model);
            }
            DiscoverUnitDirectories(session, nodeId, rootDirStart, std::move(unitDirRelOffsets), 0);
        });
}

void ROMScanSession::DiscoveryFlow::OnUnitDirHeaderReady(ROMScanSession& session,
                                                         UnitDirStepContext context, bool ok) {
    if (!ok) {
        DiscoverUnitDirectories(session, context.nodeId, context.rootDirStart,
                                std::move(context.unitDirRelOffsets), context.index + 1);
        return;
    }

    const auto* node = session.FindNode(context.nodeId);
    if (node == nullptr || context.absUnitDir.value >= node->ROM().rawQuadlets.size()) {
        FinalizeNodeDiscovery(session, context.nodeId);
        return;
    }

    const uint32_t hdr = OSSwapBigToHostInt32(node->ROM().rawQuadlets[context.absUnitDir.value]);
    context.dirLen = static_cast<uint16_t>((hdr >> 16) & 0xFFFFU);
    if (context.dirLen == 0) {
        DiscoverUnitDirectories(session, context.nodeId, context.rootDirStart,
                                std::move(context.unitDirRelOffsets), context.index + 1);
        return;
    }
    context.dirLen = std::min(context.dirLen, static_cast<uint16_t>(32U));

    const ASFW::ConfigROM::QuadletCount dirEndExclusive{context.absUnitDir.value + 1U +
                                                        static_cast<uint32_t>(context.dirLen)};
    const uint8_t ensureNodeId = context.nodeId;
    session.EnsurePrefix(ensureNodeId, dirEndExclusive,
                         [&session, context = std::move(context)](bool ok2) mutable {
                             OnUnitDirDataReady(session, std::move(context), ok2);
                         });
}

void ROMScanSession::DiscoveryFlow::OnUnitDirDataReady(ROMScanSession& session,
                                                       UnitDirStepContext context, bool ok) {
    if (!ok) {
        DiscoverUnitDirectories(session, context.nodeId, context.rootDirStart,
                                std::move(context.unitDirRelOffsets), context.index + 1);
        return;
    }

    auto* node = session.FindNode(context.nodeId);
    if (node == nullptr ||
        context.absUnitDir.value + context.dirLen >= node->ROM().rawQuadlets.size()) {
        FinalizeNodeDiscovery(session, context.nodeId);
        return;
    }

    std::vector<uint32_t> unitDirBE;
    unitDirBE.reserve(static_cast<size_t>(context.dirLen) + 1U);
    for (uint32_t i = 0; i <= static_cast<uint32_t>(context.dirLen); ++i) {
        unitDirBE.push_back(node->ROM().rawQuadlets[context.absUnitDir.value + i]);
    }

    auto unitEntries = ParseDirectoryBestEffort(unitDirBE, 32);
    UnitDirectory parsed{};
    parsed.offsetQuadlets = context.unitRel.value;

    for (const auto& entry : unitEntries) {
        if (entry.keyType != ASFW::FW::EntryType::kImmediate) {
            continue;
        }
        switch (entry.keyId) {
        case ASFW::FW::ConfigKey::kUnitSpecId:
            parsed.unitSpecId = entry.value;
            break;
        case ASFW::FW::ConfigKey::kUnitSwVersion:
            parsed.unitSwVersion = entry.value;
            break;
        case ASFW::FW::ConfigKey::kUnitDependentInfo:
            parsed.logicalUnitNumber = entry.value;
            break;
        case ASFW::FW::ConfigKey::kModelId:
            parsed.modelId = entry.value;
            break;
        default:
            break;
        }
    }

    auto unitModelRef = FindDescriptorRef(unitEntries, ASFW::FW::ConfigKey::kModelId);
    if (!unitModelRef) {
        node->MutableROM().unitDirectories.push_back(std::move(parsed));
        DiscoverUnitDirectories(session, context.nodeId, context.rootDirStart,
                                std::move(context.unitDirRelOffsets), context.index + 1);
        return;
    }

    FetchTextDescriptor(session, context.nodeId, context.absUnitDir + unitModelRef->targetRel,
                        unitModelRef->keyType,
                        [&session, nodeId = context.nodeId, rootDirStart = context.rootDirStart,
                         parsed = std::move(parsed),
                         unitDirRelOffsets = std::move(context.unitDirRelOffsets),
                         index = context.index](std::string_view name) mutable {
                            if (auto* node2 = session.FindNode(nodeId); node2 != nullptr) {
                                UnitDirectory unit = std::move(parsed);
                                if (!name.empty()) {
                                    unit.modelName = std::string(name);
                                }
                                node2->MutableROM().unitDirectories.push_back(std::move(unit));
                            }
                            DiscoverUnitDirectories(session, nodeId, rootDirStart,
                                                    std::move(unitDirRelOffsets), index + 1);
                        });
}

void ROMScanSession::DiscoveryFlow::DiscoverUnitDirectories(
    ROMScanSession& session, uint8_t nodeId, ASFW::ConfigROM::QuadletOffset rootDirStart,
    std::vector<ASFW::ConfigROM::QuadletCount> unitDirRelOffsets, size_t index) {
    if (index >= unitDirRelOffsets.size()) {
        FinalizeNodeDiscovery(session, nodeId);
        return;
    }

    const ASFW::ConfigROM::QuadletCount unitRel = unitDirRelOffsets[index];
    const ASFW::ConfigROM::QuadletOffset absUnitDir = rootDirStart + unitRel;

    session.EnsurePrefix(nodeId, ASFW::ConfigROM::QuadletCount{absUnitDir.value + 1U},
                         [&session, nodeId, rootDirStart,
                          unitDirRelOffsets = std::move(unitDirRelOffsets), index, absUnitDir,
                          unitRel](bool ok) mutable {
                             UnitDirStepContext context{};
                             context.nodeId = nodeId;
                             context.rootDirStart = rootDirStart;
                             context.unitDirRelOffsets = std::move(unitDirRelOffsets);
                             context.index = index;
                             context.absUnitDir = absUnitDir;
                             context.unitRel = unitRel;
                             OnUnitDirHeaderReady(session, std::move(context), ok);
                         });
}

void ROMScanSession::DiscoveryFlow::FinalizeNodeDiscovery(ROMScanSession& session, uint8_t nodeId) {
    auto* node = session.FindNode(nodeId);
    if (node == nullptr) {
        session.MaybeFinish();
        session.Pump();
        return;
    }

    session.speedPolicy_.RecordSuccess(nodeId, node->CurrentSpeed());
    if (!ROMScanSession::TransitionNodeState(*node, ROMScanNodeStateMachine::State::Complete,
                                             "FinalizeNodeDiscovery complete")) {
        session.MaybeFinish();
        session.Pump();
        return;
    }

    session.completedROMs_.push_back(std::move(node->MutableROM()));
    session.MaybeFinish();
    session.Pump();
}

void ROMScanSession::DiscoveryFlow::OnDiscoverDetailsPrefixReady(
    ROMScanSession& session, uint8_t nodeId, ASFW::ConfigROM::QuadletOffset rootDirStart,
    const std::vector<uint32_t>& rootDirBE, bool prefixOk) {
    auto* node = session.FindNode(nodeId);
    if (node == nullptr) {
        session.MaybeFinish();
        session.Pump();
        return;
    }

    if (!prefixOk) {
        ASFW_LOG(ConfigROM, "Node %u: ROM prefix could not be extended to rootDirStart=%u", nodeId,
                 rootDirStart.value);
    }

    auto& rawQuadlets = node->MutableROM().rawQuadlets;
    rawQuadlets.reserve(rawQuadlets.size() + rootDirBE.size());
    rawQuadlets.insert(rawQuadlets.end(), rootDirBE.begin(), rootDirBE.end());

    const auto rootEntries = ParseDirectoryBestEffort(rootDirBE, 64);
    const auto vendorRef = FindDescriptorRef(rootEntries, ASFW::FW::ConfigKey::kModuleVendorId);
    const auto modelRef = FindDescriptorRef(rootEntries, ASFW::FW::ConfigKey::kModelId);

    std::vector<ASFW::ConfigROM::QuadletCount> unitDirRelOffsets;
    for (const auto& entry : rootEntries) {
        if (entry.keyType == ASFW::FW::EntryType::kDirectory &&
            entry.keyId == ASFW::FW::ConfigKey::kUnitDirectory && entry.hasTarget &&
            entry.targetRel != 0) {
            unitDirRelOffsets.push_back(ASFW::ConfigROM::QuadletCount{entry.targetRel});
        }
    }

    DiscoverVendorName(session, nodeId, rootDirStart, vendorRef, modelRef,
                       std::move(unitDirRelOffsets));
}

void ROMScanSession::DiscoveryFlow::DiscoverDetails(ROMScanSession& session, uint8_t nodeId,
                                                    ASFW::ConfigROM::QuadletOffset rootDirStart,
                                                    std::vector<uint32_t> rootDirBE) {
    auto* node = session.FindNode(nodeId);
    if (node == nullptr) {
        session.MaybeFinish();
        session.Pump();
        return;
    }

    if (!ROMScanSession::TransitionNodeState(*node, ROMScanNodeStateMachine::State::ReadingDetails,
                                             "RootDir parsed enter details discovery")) {
        session.MaybeFinish();
        session.Pump();
        return;
    }

    session.EnsurePrefix(
        nodeId, ASFW::ConfigROM::QuadletCount{rootDirStart.value},
        [&session, nodeId, rootDirStart, rootDirBE = std::move(rootDirBE)](bool ok) mutable {
            OnDiscoverDetailsPrefixReady(session, nodeId, rootDirStart, rootDirBE, ok);
        });
}

void ROMScanSession::StartDetailsDiscovery(uint8_t nodeId,
                                           ASFW::ConfigROM::QuadletOffset rootDirStart,
                                           std::vector<uint32_t> rootDirBE) {
    DiscoveryFlow::DiscoverDetails(*this, nodeId, rootDirStart, std::move(rootDirBE));
}

} // namespace ASFW::Discovery
