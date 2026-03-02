#include "ROMScanSession.hpp"

#include "../../Async/Interfaces/IFireWireBus.hpp"
#include "../../Bus/TopologyManager.hpp"
#include "../../IRM/IRMTypes.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"
#include "../Common/ConfigROMConstants.hpp"
#include "../Common/ConfigROMPolicies.hpp"
#include "../ConfigROMParser.hpp"

#include <DriverKit/IOLib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace ASFW::Discovery {

struct ROMScanSession::DiscoveryFlow {
    struct DirEntry {
        uint32_t index{0};
        uint8_t keyType{0};
        uint8_t keyId{0};
        uint32_t value{0};
        bool hasTarget{false};
        uint32_t targetRel{0};
    };

    struct DescriptorRef {
        uint8_t keyType{0};
        uint32_t targetRel{0};
    };

    struct UnitDirStepContext {
        uint8_t nodeId{0xFF};
        uint32_t rootDirStart{0};
        std::vector<uint32_t> unitDirRelOffsets;
        size_t index{0};
        uint32_t absUnitDir{0};
        uint32_t unitRel{0};
        uint16_t dirLen{0};
    };

    [[nodiscard]] static std::vector<DirEntry> ParseDirectory(const std::vector<uint32_t>& dirBE,
                                                              uint32_t entryCap);
    [[nodiscard]] static std::optional<DescriptorRef>
    FindDescriptorRef(const std::vector<DirEntry>& entries, uint8_t ownerKeyId);
    [[nodiscard]] static std::vector<uint32_t>
    FindTextDescriptorLeafCandidates(const std::vector<uint32_t>& quadlets, uint32_t absDirOffset,
                                     uint16_t dirLen);

    static void FetchTextDescriptor(ROMScanSession& session, uint8_t nodeId, uint32_t absOffset,
                                    uint8_t keyType, std::function<void(std::string)> completion);
    static void FetchTextLeaf(ROMScanSession& session, uint8_t nodeId, uint32_t absLeafOffset,
                              std::function<void(std::string)> completion);
    static void FetchDescriptorDirText(ROMScanSession& session, uint8_t nodeId,
                                       uint32_t absDirOffset,
                                       std::function<void(std::string)> completion);
    static void TryFetchNextTextCandidate(ROMScanSession& session, uint8_t nodeId,
                                          std::vector<uint32_t> candidates, size_t index,
                                          std::function<void(std::string)> completion);

    static void
    OnFetchTextLeafHeaderReady(ROMScanSession& session, uint8_t nodeId, uint32_t absLeafOffset,
                               const std::shared_ptr<std::function<void(std::string)>>& completion,
                               bool ok);
    static void
    OnFetchTextLeafDataReady(ROMScanSession& session, uint8_t nodeId, uint32_t absLeafOffset,
                             const std::shared_ptr<std::function<void(std::string)>>& completion,
                             bool ok);
    static void OnFetchDescriptorDirHeaderReady(
        ROMScanSession& session, uint8_t nodeId, uint32_t absDirOffset,
        const std::shared_ptr<std::function<void(std::string)>>& completion, bool ok);
    static void OnFetchDescriptorDirDataReady(
        ROMScanSession& session, uint8_t nodeId, uint32_t absDirOffset, uint16_t dirLen,
        std::shared_ptr<std::function<void(std::string)>> completion, bool ok);

    static void DiscoverDetails(ROMScanSession& session, uint8_t nodeId, uint32_t rootDirStart,
                                std::vector<uint32_t> rootDirBE);
    static void OnDiscoverDetailsPrefixReady(ROMScanSession& session, uint8_t nodeId,
                                             uint32_t rootDirStart,
                                             const std::vector<uint32_t>& rootDirBE, bool prefixOk);
    static void DiscoverVendorName(ROMScanSession& session, uint8_t nodeId, uint32_t rootDirStart,
                                   std::optional<DescriptorRef> vendorRef,
                                   std::optional<DescriptorRef> modelRef,
                                   std::vector<uint32_t> unitDirRelOffsets);
    static void DiscoverModelName(ROMScanSession& session, uint8_t nodeId, uint32_t rootDirStart,
                                  std::optional<DescriptorRef> modelRef,
                                  std::vector<uint32_t> unitDirRelOffsets);
    static void DiscoverUnitDirectories(ROMScanSession& session, uint8_t nodeId,
                                        uint32_t rootDirStart,
                                        std::vector<uint32_t> unitDirRelOffsets, size_t index);
    static void OnUnitDirHeaderReady(ROMScanSession& session, UnitDirStepContext context, bool ok);
    static void OnUnitDirDataReady(ROMScanSession& session, UnitDirStepContext context, bool ok);
    static void FinalizeNodeDiscovery(ROMScanSession& session, uint8_t nodeId);
};

std::vector<ROMScanSession::DiscoveryFlow::DirEntry>
ROMScanSession::DiscoveryFlow::ParseDirectory(const std::vector<uint32_t>& dirBE,
                                              uint32_t entryCap) {
    std::vector<DirEntry> out;
    if (dirBE.empty()) {
        return out;
    }

    const uint32_t hdr = OSSwapBigToHostInt32(dirBE[0]);
    const uint32_t len = (hdr >> 16) & 0xFFFFU;
    const auto available = static_cast<uint32_t>(dirBE.size() - 1);
    const uint32_t count = std::min({len, available, entryCap});

    out.reserve(count);
    for (uint32_t i = 1; i <= count; ++i) {
        const uint32_t entry = OSSwapBigToHostInt32(dirBE[i]);
        DirEntry entryOut{};
        entryOut.index = i;
        entryOut.keyType = static_cast<uint8_t>((entry >> 30) & 0x3U);
        entryOut.keyId = static_cast<uint8_t>((entry >> 24) & 0x3FU);
        entryOut.value = entry & 0x00FFFFFFU;

        if (entryOut.keyType == ASFW::FW::EntryType::kLeaf ||
            entryOut.keyType == ASFW::FW::EntryType::kDirectory) {
            const int32_t off = ((entryOut.value & 0x00800000U) != 0U)
                                    ? static_cast<int32_t>(entryOut.value | 0xFF000000U)
                                    : static_cast<int32_t>(entryOut.value);
            const int32_t rel = static_cast<int32_t>(i) + off;
            if (rel >= 0) {
                entryOut.hasTarget = true;
                entryOut.targetRel = static_cast<uint32_t>(rel);
            }
        }
        out.push_back(entryOut);
    }
    return out;
}

std::optional<ROMScanSession::DiscoveryFlow::DescriptorRef>
ROMScanSession::DiscoveryFlow::FindDescriptorRef(const std::vector<DirEntry>& entries,
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
                             .targetRel = descriptorEntry.targetRel};
    }
    return std::nullopt;
}

std::vector<uint32_t> ROMScanSession::DiscoveryFlow::FindTextDescriptorLeafCandidates(
    const std::vector<uint32_t>& quadlets, uint32_t absDirOffset, uint16_t dirLen) {
    std::vector<uint32_t> leafCandidates;
    for (uint32_t i = 1; i <= static_cast<uint32_t>(dirLen); ++i) {
        if (absDirOffset + i >= quadlets.size()) {
            break;
        }
        const uint32_t entry = OSSwapBigToHostInt32(quadlets[absDirOffset + i]);
        const auto keyType = static_cast<uint8_t>((entry >> 30) & 0x3U);
        const auto keyId = static_cast<uint8_t>((entry >> 24) & 0x3FU);
        const uint32_t value = entry & 0x00FFFFFFU;

        if (keyId == ASFW::FW::ConfigKey::kTextualDescriptor &&
            keyType == ASFW::FW::EntryType::kLeaf) {
            const int32_t off = ((value & 0x00800000U) != 0U)
                                    ? static_cast<int32_t>(value | 0xFF000000U)
                                    : static_cast<int32_t>(value);
            const int32_t rel = static_cast<int32_t>(i) + off;
            if (rel >= 0) {
                leafCandidates.push_back(absDirOffset + static_cast<uint32_t>(rel));
            }
        }
    }
    return leafCandidates;
}

void ROMScanSession::DiscoveryFlow::FetchTextDescriptor(
    ROMScanSession& session, uint8_t nodeId, uint32_t absOffset, uint8_t keyType,
    std::function<void(std::string)> completion) {
    if (keyType == ASFW::FW::EntryType::kLeaf) {
        FetchTextLeaf(session, nodeId, absOffset, std::move(completion));
    } else if (keyType == ASFW::FW::EntryType::kDirectory) {
        FetchDescriptorDirText(session, nodeId, absOffset, std::move(completion));
    } else if (completion) {
        completion("");
    }
}

void ROMScanSession::DiscoveryFlow::OnFetchTextLeafDataReady(
    ROMScanSession& session, uint8_t nodeId, uint32_t absLeafOffset,
    const std::shared_ptr<std::function<void(std::string)>>& completion, bool ok) {
    if (!ok) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    const auto* node = session.FindNode(nodeId);
    if (node == nullptr) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    const auto span =
        std::span<const uint32_t>(node->ROM().rawQuadlets.data(), node->ROM().rawQuadlets.size());
    std::string text = ConfigROMParser::ParseTextDescriptorLeaf(span, absLeafOffset);
    if (completion && *completion) {
        (*completion)(std::move(text));
    }
}

void ROMScanSession::DiscoveryFlow::OnFetchTextLeafHeaderReady(
    ROMScanSession& session, uint8_t nodeId, uint32_t absLeafOffset,
    const std::shared_ptr<std::function<void(std::string)>>& completion, bool ok) {
    if (!ok) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    const auto* node = session.FindNode(nodeId);
    if (node == nullptr || absLeafOffset >= node->ROM().rawQuadlets.size()) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    const uint32_t hdr = OSSwapBigToHostInt32(node->ROM().rawQuadlets[absLeafOffset]);
    const auto leafLen = static_cast<uint16_t>((hdr >> 16) & 0xFFFFU);
    const uint32_t leafEndExclusive = absLeafOffset + 1U + static_cast<uint32_t>(leafLen);

    session.EnsurePrefix(
        nodeId, leafEndExclusive, [&session, nodeId, absLeafOffset, completion](bool ok2) {
            OnFetchTextLeafDataReady(session, nodeId, absLeafOffset, completion, ok2);
        });
}

void ROMScanSession::DiscoveryFlow::FetchTextLeaf(ROMScanSession& session, uint8_t nodeId,
                                                  uint32_t absLeafOffset,
                                                  std::function<void(std::string)> completion) {
    auto completionHolder =
        std::make_shared<std::function<void(std::string)>>(std::move(completion));
    session.EnsurePrefix(
        nodeId, absLeafOffset + 1,
        [&session, nodeId, absLeafOffset, completion = std::move(completionHolder)](bool ok) {
            OnFetchTextLeafHeaderReady(session, nodeId, absLeafOffset, completion, ok);
        });
}

void ROMScanSession::DiscoveryFlow::OnFetchDescriptorDirDataReady(
    ROMScanSession& session, uint8_t nodeId, uint32_t absDirOffset, uint16_t dirLen,
    std::shared_ptr<std::function<void(std::string)>> completion, bool ok) {
    if (!ok) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    const auto* node = session.FindNode(nodeId);
    if (node == nullptr || absDirOffset + dirLen >= node->ROM().rawQuadlets.size()) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    std::vector<uint32_t> leafCandidates =
        FindTextDescriptorLeafCandidates(node->ROM().rawQuadlets, absDirOffset, dirLen);
    if (leafCandidates.empty()) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    TryFetchNextTextCandidate(session, nodeId, std::move(leafCandidates), 0,
                              [completion = std::move(completion)](std::string text) {
                                  if (completion && *completion) {
                                      (*completion)(std::move(text));
                                  }
                              });
}

void ROMScanSession::DiscoveryFlow::OnFetchDescriptorDirHeaderReady(
    ROMScanSession& session, uint8_t nodeId, uint32_t absDirOffset,
    const std::shared_ptr<std::function<void(std::string)>>& completion, bool ok) {
    if (!ok) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    const auto* node = session.FindNode(nodeId);
    if (node == nullptr || absDirOffset >= node->ROM().rawQuadlets.size()) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    const uint32_t hdr = OSSwapBigToHostInt32(node->ROM().rawQuadlets[absDirOffset]);
    auto dirLen = static_cast<uint16_t>((hdr >> 16) & 0xFFFFU);
    if (dirLen == 0) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }
    dirLen = std::min(dirLen, static_cast<uint16_t>(32U));

    const uint32_t dirEndExclusive = absDirOffset + 1U + static_cast<uint32_t>(dirLen);
    session.EnsurePrefix(
        nodeId, dirEndExclusive, [&session, nodeId, absDirOffset, dirLen, completion](bool ok2) {
            OnFetchDescriptorDirDataReady(session, nodeId, absDirOffset, dirLen, completion, ok2);
        });
}

void ROMScanSession::DiscoveryFlow::FetchDescriptorDirText(
    ROMScanSession& session, uint8_t nodeId, uint32_t absDirOffset,
    std::function<void(std::string)> completion) {
    auto completionHolder =
        std::make_shared<std::function<void(std::string)>>(std::move(completion));
    session.EnsurePrefix(
        nodeId, absDirOffset + 1,
        [&session, nodeId, absDirOffset, completion = std::move(completionHolder)](bool ok) {
            OnFetchDescriptorDirHeaderReady(session, nodeId, absDirOffset, completion, ok);
        });
}

void ROMScanSession::DiscoveryFlow::TryFetchNextTextCandidate(
    ROMScanSession& session, uint8_t nodeId, std::vector<uint32_t> candidates, size_t index,
    std::function<void(std::string)> completion) {
    if (index >= candidates.size()) {
        if (completion) {
            completion("");
        }
        return;
    }

    const uint32_t leafAbs = candidates[index];
    FetchTextLeaf(session, nodeId, leafAbs,
                  [&session, nodeId, candidates = std::move(candidates), index,
                   completion = std::move(completion)](std::string text) mutable {
                      if (!text.empty()) {
                          if (completion) {
                              completion(std::move(text));
                          }
                      } else {
                          TryFetchNextTextCandidate(session, nodeId, std::move(candidates),
                                                    index + 1, std::move(completion));
                      }
                  });
}

void ROMScanSession::DiscoveryFlow::DiscoverVendorName(ROMScanSession& session, uint8_t nodeId,
                                                       uint32_t rootDirStart,
                                                       std::optional<DescriptorRef> vendorRef,
                                                       std::optional<DescriptorRef> modelRef,
                                                       std::vector<uint32_t> unitDirRelOffsets) {
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

void ROMScanSession::DiscoveryFlow::DiscoverModelName(ROMScanSession& session, uint8_t nodeId,
                                                      uint32_t rootDirStart,
                                                      std::optional<DescriptorRef> modelRef,
                                                      std::vector<uint32_t> unitDirRelOffsets) {
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
    if (node == nullptr || context.absUnitDir >= node->ROM().rawQuadlets.size()) {
        FinalizeNodeDiscovery(session, context.nodeId);
        return;
    }

    const uint32_t hdr = OSSwapBigToHostInt32(node->ROM().rawQuadlets[context.absUnitDir]);
    context.dirLen = static_cast<uint16_t>((hdr >> 16) & 0xFFFFU);
    if (context.dirLen == 0) {
        DiscoverUnitDirectories(session, context.nodeId, context.rootDirStart,
                                std::move(context.unitDirRelOffsets), context.index + 1);
        return;
    }
    context.dirLen = std::min(context.dirLen, static_cast<uint16_t>(32U));

    const uint32_t dirEndExclusive =
        context.absUnitDir + 1U + static_cast<uint32_t>(context.dirLen);
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
    if (node == nullptr || context.absUnitDir + context.dirLen >= node->ROM().rawQuadlets.size()) {
        FinalizeNodeDiscovery(session, context.nodeId);
        return;
    }

    std::vector<uint32_t> unitDirBE;
    unitDirBE.reserve(static_cast<size_t>(context.dirLen) + 1U);
    for (uint32_t i = 0; i <= static_cast<uint32_t>(context.dirLen); ++i) {
        unitDirBE.push_back(node->ROM().rawQuadlets[context.absUnitDir + i]);
    }

    auto unitEntries = ParseDirectory(unitDirBE, 32);
    UnitDirectory parsed{};
    parsed.offsetQuadlets = context.unitRel;

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

void ROMScanSession::DiscoveryFlow::DiscoverUnitDirectories(ROMScanSession& session, uint8_t nodeId,
                                                            uint32_t rootDirStart,
                                                            std::vector<uint32_t> unitDirRelOffsets,
                                                            size_t index) {
    if (index >= unitDirRelOffsets.size()) {
        FinalizeNodeDiscovery(session, nodeId);
        return;
    }

    const uint32_t unitRel = unitDirRelOffsets[index];
    const uint32_t absUnitDir = rootDirStart + unitRel;

    session.EnsurePrefix(nodeId, absUnitDir + 1,
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
    ROMScanSession& session, uint8_t nodeId, uint32_t rootDirStart,
    const std::vector<uint32_t>& rootDirBE, bool prefixOk) {
    auto* node = session.FindNode(nodeId);
    if (node == nullptr) {
        session.MaybeFinish();
        session.Pump();
        return;
    }

    if (!prefixOk) {
        ASFW_LOG(ConfigROM, "Node %u: ROM prefix could not be extended to rootDirStart=%u", nodeId,
                 rootDirStart);
    }

    auto& rawQuadlets = node->MutableROM().rawQuadlets;
    rawQuadlets.reserve(rawQuadlets.size() + rootDirBE.size());
    rawQuadlets.insert(rawQuadlets.end(), rootDirBE.begin(), rootDirBE.end());

    const auto rootEntries = ParseDirectory(rootDirBE, 64);
    const auto vendorRef = FindDescriptorRef(rootEntries, ASFW::FW::ConfigKey::kModuleVendorId);
    const auto modelRef = FindDescriptorRef(rootEntries, ASFW::FW::ConfigKey::kModelId);

    std::vector<uint32_t> unitDirRelOffsets;
    for (const auto& entry : rootEntries) {
        if (entry.keyType == ASFW::FW::EntryType::kDirectory &&
            entry.keyId == ASFW::FW::ConfigKey::kUnitDirectory && entry.hasTarget &&
            entry.targetRel != 0) {
            unitDirRelOffsets.push_back(entry.targetRel);
        }
    }

    DiscoverVendorName(session, nodeId, rootDirStart, vendorRef, modelRef,
                       std::move(unitDirRelOffsets));
}

void ROMScanSession::DiscoveryFlow::DiscoverDetails(ROMScanSession& session, uint8_t nodeId,
                                                    uint32_t rootDirStart,
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
        nodeId, rootDirStart,
        [&session, nodeId, rootDirStart, rootDirBE = std::move(rootDirBE)](bool ok) mutable {
            OnDiscoverDetailsPrefixReady(session, nodeId, rootDirStart, rootDirBE, ok);
        });
}

ROMScanSession::ROMScanSession(Async::IFireWireBus& bus, SpeedPolicy& speedPolicy,
                               ROMScannerParams params, std::shared_ptr<ROMReader> reader,
                               OSSharedPtr<IODispatchQueue> dispatchQueue,
                               Driver::TopologyManager* topologyManager)
    : bus_(bus), speedPolicy_(speedPolicy), params_(params),
      dispatchQueue_(std::move(dispatchQueue)), topologyManager_(topologyManager),
      reader_(std::move(reader)) {
    executorLock_ = IOLockAlloc();
    if (executorLock_ == nullptr) {
        ASFW_LOG(ConfigROM, "ROMScanSession: failed to allocate executor lock");
    }
    if (!reader_) {
        reader_ = std::make_shared<ROMReader>(bus_, dispatchQueue_);
    }
}

ROMScanSession::~ROMScanSession() {
    aborted_.store(true, std::memory_order_relaxed);
    if (executorLock_ != nullptr) {
        IOLockFree(executorLock_);
        executorLock_ = nullptr;
    }
}

void ROMScanSession::Start(ROMScanRequest request, ScanCompletionCallback completion) {
    aborted_.store(false, std::memory_order_relaxed);

    DispatchAsync([self = weak_from_this(), request = std::move(request),
                   completion = std::move(completion)]() mutable {
        auto session = self.lock();
        if (!session) {
            return;
        }

        session->gen_ = request.gen;
        session->topology_ = std::move(request.topology);
        session->localNodeId_ = request.localNodeId;
        session->completion_ = std::move(completion);
        session->completionNotified_ = false;
        session->hadBusyNodes_ = false;
        session->inflight_ = 0;
        session->completedROMs_.clear();
        session->nodeScans_.clear();

        if (request.targetNodes.empty()) {
            for (const auto& node : session->topology_.nodes) {
                if (node.nodeId == session->localNodeId_) {
                    continue;
                }
                if (!node.linkActive) {
                    continue;
                }
                session->nodeScans_.emplace_back(node.nodeId, session->gen_,
                                                 session->params_.startSpeed,
                                                 session->params_.perStepRetries);
            }
        } else {
            auto targets = std::move(request.targetNodes);
            std::ranges::sort(targets);
            const auto uniqueRange = std::ranges::unique(targets);
            targets.erase(uniqueRange.begin(), targets.end());

            for (const uint8_t nodeId : targets) {
                if (nodeId == session->localNodeId_) {
                    continue;
                }
                session->nodeScans_.emplace_back(nodeId, session->gen_, session->params_.startSpeed,
                                                 session->params_.perStepRetries);
            }
        }

        if (session->nodeScans_.empty()) {
            session->MaybeFinish();
            return;
        }

        session->Pump();
    });
}

void ROMScanSession::Abort() {
    aborted_.store(true, std::memory_order_relaxed);
    DispatchAsync([self = weak_from_this()] {
        auto session = self.lock();
        if (!session) {
            return;
        }
        session->completion_ = nullptr;
        session->completionNotified_ = true;
        session->nodeScans_.clear();
        session->completedROMs_.clear();
        session->inflight_ = 0;
        session->gen_ = 0;
        session->hadBusyNodes_ = false;
    });
}

void ROMScanSession::DispatchAsync(std::function<void()> work) {
    if (!work) {
        return;
    }

    if (!dispatchQueue_) {
        Post(std::move(work));
        return;
    }

    auto queue = dispatchQueue_;
    auto captured = std::make_shared<std::function<void()>>(std::move(work));
    queue->DispatchAsync(^{
      (*captured)();
    });
}

void ROMScanSession::Post(std::function<void()> task) {
    if (!task) {
        return;
    }

    std::shared_ptr<ROMScanSession> keepAlive;
    if (executorLock_ != nullptr) {
        IOLockLock(executorLock_);
    }

    executorQueue_.push_back(std::move(task));
    if (executorDraining_) {
        if (executorLock_ != nullptr) {
            IOLockUnlock(executorLock_);
        }
        return;
    }
    executorDraining_ = true;
    keepAlive = shared_from_this();

    if (executorLock_ != nullptr) {
        IOLockUnlock(executorLock_);
    }

    keepAlive->DrainPending();
}

void ROMScanSession::DrainPending() {
    while (true) {
        std::function<void()> next;

        if (executorLock_ != nullptr) {
            IOLockLock(executorLock_);
        }

        if (executorQueue_.empty()) {
            executorDraining_ = false;
            if (executorLock_ != nullptr) {
                IOLockUnlock(executorLock_);
            }
            return;
        }

        next = std::move(executorQueue_.front());
        executorQueue_.pop_front();

        if (executorLock_ != nullptr) {
            IOLockUnlock(executorLock_);
        }
        next();
    }
}

ROMScanNodeStateMachine* ROMScanSession::FindNode(uint8_t nodeId) {
    auto it = std::ranges::find_if(nodeScans_, [nodeId](const ROMScanNodeStateMachine& node) {
        return node.NodeId() == nodeId;
    });
    return (it != nodeScans_.end()) ? std::to_address(it) : nullptr;
}

bool ROMScanSession::TransitionNodeState(ROMScanNodeStateMachine& node,
                                         ROMScanNodeStateMachine::State next, const char* reason) {
    if (node.TransitionTo(next)) {
        return true;
    }

    ASFW_LOG(ConfigROM, "ROMScanSession: invalid node state transition node=%u from=%u to=%u (%s)",
             node.NodeId(), static_cast<uint8_t>(node.CurrentState()), static_cast<uint8_t>(next),
             reason != nullptr ? reason : "unspecified");
    node.ForceState(ROMScanNodeStateMachine::State::Failed);
    return false;
}

void ROMScanSession::Pump() {
    if (aborted_.load(std::memory_order_relaxed)) {
        return;
    }

    for (auto& node : nodeScans_) {
        if (inflight_ >= params_.maxInflight) {
            break;
        }
        if (node.CurrentState() != ROMScanNodeStateMachine::State::Idle || node.BIBInProgress()) {
            continue;
        }
        StartBIBRead(node);
    }

    MaybeFinish();
}

void ROMScanSession::StartBIBRead(ROMScanNodeStateMachine& node) {
    if (!TransitionNodeState(node, ROMScanNodeStateMachine::State::ReadingBIB, "Pump start BIB")) {
        return;
    }

    node.SetBIBInProgress(true);
    ++inflight_;

    const uint8_t nodeId = node.NodeId();
    const Generation gen = gen_;
    const auto speed = node.CurrentSpeed();

    auto weakSelf = weak_from_this();
    reader_->ReadBIB(nodeId, gen, speed, [weakSelf, nodeId](ROMReader::ReadResult result) mutable {
        if (auto self = weakSelf.lock(); self) {
            self->DispatchAsync(
                [self = std::move(self), nodeId, result = std::move(result)]() mutable {
                    self->HandleBIBComplete(nodeId, std::move(result));
                });
        }
    });
}

void ROMScanSession::RetryWithFallback(ROMScanNodeStateMachine& node) {
    const auto oldSpeed = node.CurrentSpeed();
    (void)oldSpeed;
    const RetryBackoffPolicy retryPolicy{};
    const auto decision = retryPolicy.Apply(
        node, speedPolicy_, params_.perStepRetries,
        [](ROMScanNodeStateMachine& nodeStateMachine, ROMScanNodeStateMachine::State next,
           const char* reason) { return TransitionNodeState(nodeStateMachine, next, reason); });

    switch (decision) {
    case RetryBackoffPolicy::Decision::RetrySameSpeed:
        ASFW_LOG_V2(ConfigROM, "ROMScanSession: Node %u retry at S%u00 (retries left=%u)",
                    node.NodeId(), static_cast<uint32_t>(node.CurrentSpeed()) + 1,
                    node.RetriesLeft());
        break;
    case RetryBackoffPolicy::Decision::RetryWithFallback: {
        const auto newSpeed = node.CurrentSpeed();
        (void)newSpeed;
        ASFW_LOG_V2(ConfigROM,
                    "ROMScanSession: Node %u speed fallback S%u00 -> S%u00, retries reset",
                    node.NodeId(), static_cast<uint32_t>(oldSpeed) + 1,
                    static_cast<uint32_t>(newSpeed) + 1);
        break;
    }
    case RetryBackoffPolicy::Decision::FailedExhausted:
        ASFW_LOG(ConfigROM, "ROMScanSession: Node %u -> Failed (exhausted retries)", node.NodeId());
        break;
    }
}

void ROMScanSession::HandleBIBComplete(uint8_t nodeId, ROMReader::ReadResult result) {
    if (aborted_.load(std::memory_order_relaxed) || result.generation != gen_) {
        return;
    }

    if (inflight_ > 0) {
        --inflight_;
    }

    auto* nodePtr = FindNode(nodeId);
    if (nodePtr == nullptr) {
        Pump();
        return;
    }

    auto& node = *nodePtr;
    node.SetBIBInProgress(false);

    if (!result.success) {
        ASFW_LOG(ConfigROM, "ROMScanSession: Node %u BIB read failed, retrying", nodeId);
        hadBusyNodes_ = true;
        RetryWithFallback(node);
        Pump();
        return;
    }

    // IEEE 1212 allows temporary zero in header while device is still booting.
    if (!result.quadletsBE.empty() && result.quadletsBE[0] == 0) {
        ASFW_LOG(ConfigROM, "ROMScanSession: Node %u BIB quadlet[0]=0 (booting), retry", nodeId);
        hadBusyNodes_ = true;
        RetryWithFallback(node);
        Pump();
        return;
    }

    if (result.quadletsBE.size() < ASFW::ConfigROM::kBIBQuadletCount) {
        ASFW_LOG(ConfigROM, "ROMScanSession: Node %u BIB read returned %zu quadlets (expected %u)",
                 nodeId, result.quadletsBE.size(), ASFW::ConfigROM::kBIBQuadletCount);
        (void)TransitionNodeState(node, ROMScanNodeStateMachine::State::Failed, "BIB short read");
        Pump();
        return;
    }

    auto bibOpt = ConfigROMParser::ParseBIB(result.quadletsBE.data());
    if (!bibOpt.has_value()) {
        ASFW_LOG(ConfigROM, "ROMScanSession: Node %u BIB parse failed", nodeId);
        (void)TransitionNodeState(node, ROMScanNodeStateMachine::State::Failed, "BIB parse failed");
        Pump();
        return;
    }

    node.MutableROM().bib = bibOpt.value();

    node.MutableROM().rawQuadlets.clear();
    node.MutableROM().rawQuadlets.reserve(std::max<size_t>(256U, result.quadletsBE.size()));
    node.MutableROM().rawQuadlets.insert(node.MutableROM().rawQuadlets.end(),
                                         result.quadletsBE.begin(), result.quadletsBE.end());

    speedPolicy_.RecordSuccess(nodeId, node.CurrentSpeed());

    ContinueAfterBIBSuccess(node, nodeId);
}

void ROMScanSession::ContinueAfterBIBSuccess(ROMScanNodeStateMachine& node, uint8_t nodeId) {
    if (node.ROM().bib.crcLength <= node.ROM().bib.busInfoLength) {
        if (!TransitionNodeState(node, ROMScanNodeStateMachine::State::Complete,
                                 "BIB minimal ROM complete")) {
            Pump();
            return;
        }
        completedROMs_.push_back(std::move(node.MutableROM()));
        Pump();
        return;
    }

    if (params_.doIRMCheck && topology_.irmNodeId.has_value() && *topology_.irmNodeId == nodeId &&
        node.ROM().bib.irmc) {
        StartIRMRead(node);
        return;
    }

    StartRootDirRead(node);
}

void ROMScanSession::StartIRMRead(ROMScanNodeStateMachine& node) {
    const uint8_t nodeId = node.NodeId();

    node.SetNeedsIRMCheck(true);
    if (!TransitionNodeState(node, ROMScanNodeStateMachine::State::VerifyingIRM_Read,
                             "BIB complete enter IRM verify read")) {
        Pump();
        return;
    }

    ++inflight_;

    Async::FWAddress addr{IRM::IRMRegisters::kAddressHi,
                          IRM::IRMRegisters::kChannelsAvailable63_32};

    const Generation gen = gen_;
    auto weakSelf = weak_from_this();
    const auto handle = bus_.ReadQuad(
        FW::Generation{static_cast<uint32_t>(gen)}, FW::NodeId{nodeId}, addr, FW::FwSpeed::S100,
        [weakSelf, nodeId](Async::AsyncStatus status, std::span<const uint8_t> payload) mutable {
            bool ok = status == Async::AsyncStatus::kSuccess && payload.size() == 4;
            uint32_t valueHost = 0;
            if (ok) {
                uint32_t raw = 0;
                std::memcpy(&raw, payload.data(), sizeof(raw));
                valueHost = OSSwapBigToHostInt32(raw);
            }

            if (auto self = weakSelf.lock(); self) {
                self->DispatchAsync([self = std::move(self), nodeId, ok, valueHost]() mutable {
                    self->HandleIRMReadComplete(nodeId, ok, valueHost);
                });
            }
        });

    if (!handle) {
        HandleIRMReadComplete(nodeId, /*success=*/false, 0);
    }
}

void ROMScanSession::HandleIRMReadComplete(uint8_t nodeId, bool success, uint32_t valueHostOrder) {
    if (aborted_.load(std::memory_order_relaxed)) {
        return;
    }
    if (gen_ == 0) {
        return;
    }

    if (inflight_ > 0) {
        --inflight_;
    }

    auto* nodePtr = FindNode(nodeId);
    if (nodePtr == nullptr) {
        Pump();
        return;
    }

    auto& node = *nodePtr;

    if (!success) {
        ASFW_LOG_V1(ConfigROM, "ROMScanSession: Node %u IRM read verify failed - marking bad IRM",
                    nodeId);
        node.SetIRMIsBad(true);
        if (topologyManager_ != nullptr && topology_.irmNodeId.has_value() &&
            *topology_.irmNodeId == nodeId) {
            topologyManager_->MarkNodeAsBadIRM(nodeId);
        }

        node.SetNeedsIRMCheck(false);
        StartRootDirRead(node);
        return;
    }

    node.SetIRMBitBucket(valueHostOrder);
    node.SetIRMCheckReadDone(true);
    StartIRMLock(node);
}

void ROMScanSession::StartIRMLock(ROMScanNodeStateMachine& node) {
    const uint8_t nodeId = node.NodeId();

    if (!TransitionNodeState(node, ROMScanNodeStateMachine::State::VerifyingIRM_Lock,
                             "IRM read complete enter lock verify")) {
        Pump();
        return;
    }

    ++inflight_;

    Async::FWAddress addr{IRM::IRMRegisters::kAddressHi,
                          IRM::IRMRegisters::kChannelsAvailable63_32};

    std::array<uint8_t, 8> casOperand{};
    const uint32_t beCompare = OSSwapHostToBigInt32(0xFFFFFFFFU);
    const uint32_t beSwap = OSSwapHostToBigInt32(0xFFFFFFFFU);
    std::memcpy(casOperand.data(), &beCompare, sizeof(beCompare));
    std::memcpy(casOperand.data() + sizeof(beCompare), &beSwap, sizeof(beSwap));

    const Generation gen = gen_;
    auto weakSelf = weak_from_this();
    const auto handle = bus_.Lock(
        FW::Generation{static_cast<uint32_t>(gen)}, FW::NodeId{nodeId}, addr,
        FW::LockOp::kCompareSwap, std::span<const uint8_t>{casOperand},
        /*responseLength=*/4, FW::FwSpeed::S100,
        [weakSelf, nodeId](Async::AsyncStatus status, std::span<const uint8_t> payload) mutable {
            const bool ok = status == Async::AsyncStatus::kSuccess && payload.size() == 4;
            if (auto self = weakSelf.lock(); self) {
                self->DispatchAsync([self = std::move(self), nodeId, ok]() mutable {
                    self->HandleIRMLockComplete(nodeId, ok);
                });
            }
        });

    if (!handle) {
        HandleIRMLockComplete(nodeId, /*success=*/false);
    }
}

void ROMScanSession::HandleIRMLockComplete(uint8_t nodeId, bool success) {
    if (aborted_.load(std::memory_order_relaxed)) {
        return;
    }
    if (gen_ == 0) {
        return;
    }

    if (inflight_ > 0) {
        --inflight_;
    }

    auto* nodePtr = FindNode(nodeId);
    if (nodePtr == nullptr) {
        Pump();
        return;
    }

    auto& node = *nodePtr;

    if (!success) {
        ASFW_LOG_V1(ConfigROM, "ROMScanSession: Node %u IRM lock verify failed - marking bad IRM",
                    nodeId);
        node.SetIRMIsBad(true);
        if (topologyManager_ != nullptr && topology_.irmNodeId.has_value() &&
            *topology_.irmNodeId == nodeId) {
            topologyManager_->MarkNodeAsBadIRM(nodeId);
        }
    } else {
        node.SetIRMCheckLockDone(true);
    }

    node.SetNeedsIRMCheck(false);
    StartRootDirRead(node);
}

void ROMScanSession::StartRootDirRead(ROMScanNodeStateMachine& node) {
    const uint8_t nodeId = node.NodeId();

    if (!TransitionNodeState(node, ROMScanNodeStateMachine::State::ReadingRootDir,
                             "BIB complete enter root dir read")) {
        Pump();
        return;
    }

    node.SetRetriesLeft(params_.perStepRetries);
    ++inflight_;

    const uint32_t offsetBytes = ASFW::ConfigROM::RootDirStartBytes(node.ROM().bib);
    auto weakSelf = weak_from_this();
    reader_->ReadRootDirQuadlets(
        nodeId, gen_, node.CurrentSpeed(), offsetBytes, 0,
        [weakSelf, nodeId](ROMReader::ReadResult result) mutable {
            if (auto self = weakSelf.lock(); self) {
                self->DispatchAsync(
                    [self = std::move(self), nodeId, result = std::move(result)]() mutable {
                        self->HandleRootDirComplete(nodeId, std::move(result));
                    });
            }
        });
}

void ROMScanSession::HandleRootDirComplete(uint8_t nodeId, ROMReader::ReadResult result) {
    if (aborted_.load(std::memory_order_relaxed)) {
        return;
    }
    if (result.generation != gen_) {
        return;
    }

    if (inflight_ > 0) {
        --inflight_;
    }

    auto* nodePtr = FindNode(nodeId);
    if (nodePtr == nullptr) {
        Pump();
        return;
    }

    auto& node = *nodePtr;

    if (!result.success || result.quadletsBE.empty()) {
        ASFW_LOG(ConfigROM, "ROMScanSession: Node %u RootDir read failed - marking failed", nodeId);
        (void)TransitionNodeState(node, ROMScanNodeStateMachine::State::Failed,
                                  "RootDir read failed");
        Pump();
        return;
    }

    const uint32_t quadletCount = static_cast<uint32_t>(result.quadletsBE.size());
    node.MutableROM().rootDirMinimal =
        ConfigROMParser::ParseRootDirectory(result.quadletsBE.data(), quadletCount);

    std::vector<uint32_t> rootDirBE = std::move(result.quadletsBE);

    const uint32_t rootDirStart = ASFW::ConfigROM::RootDirStartQuadlet(node.ROM().bib);
    DiscoveryFlow::DiscoverDetails(*this, nodeId, rootDirStart, std::move(rootDirBE));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) - callback-driven state machine.
void ROMScanSession::EnsurePrefix(uint8_t nodeId, uint32_t requiredTotalQuadlets,
                                  std::function<void(bool)> completion) {
    if (aborted_.load(std::memory_order_relaxed)) {
        return;
    }

    auto* nodePtr = FindNode(nodeId);
    if (nodePtr == nullptr) {
        if (completion) {
            completion(false);
        }
        return;
    }

    auto& node = *nodePtr;

    if (requiredTotalQuadlets > ASFW::ConfigROM::kMaxROMPrefixQuadlets) {
        ASFW_LOG(ConfigROM,
                 "EnsurePrefix: node=%u required=%u exceeds max ROM prefix (%u quadlets), skipping",
                 nodeId, requiredTotalQuadlets, ASFW::ConfigROM::kMaxROMPrefixQuadlets);
        if (completion) {
            completion(false);
        }
        return;
    }

    const auto have = static_cast<uint32_t>(node.ROM().rawQuadlets.size());
    if (have >= requiredTotalQuadlets) {
        if (completion) {
            completion(true);
        }
        return;
    }

    const uint32_t toRead = requiredTotalQuadlets - have;
    const uint32_t offsetBytes = have * 4U;

    ++inflight_;

    auto completionHolder = std::make_shared<std::function<void(bool)>>(std::move(completion));

    auto weakSelf = weak_from_this();
    reader_->ReadRootDirQuadlets(
        nodeId, gen_, node.CurrentSpeed(), offsetBytes, toRead,
        // NOLINTNEXTLINE(readability-function-cognitive-complexity) - nested async continuation.
        [weakSelf, nodeId, requiredTotalQuadlets,
         completionHolder](ROMReader::ReadResult res) mutable {
            if (auto self = weakSelf.lock(); self) {
                // NOLINTNEXTLINE(readability-function-cognitive-complexity)
                self->DispatchAsync([self = std::move(self), nodeId, requiredTotalQuadlets,
                                     completionHolder, res = std::move(res)]() mutable {
                    if (self->aborted_.load(std::memory_order_relaxed)) {
                        return;
                    }
                    if (res.generation != self->gen_) {
                        return;
                    }

                    if (self->inflight_ > 0) {
                        --self->inflight_;
                    }

                    auto* node = self->FindNode(nodeId);
                    if (node == nullptr) {
                        if (completionHolder && *completionHolder) {
                            (*completionHolder)(false);
                        }
                        self->Pump();
                        return;
                    }

                    if (!res.success || res.quadletsBE.empty()) {
                        ASFW_LOG(ConfigROM, "EnsurePrefix read failed: node=%u", nodeId);
                        if (completionHolder && *completionHolder) {
                            (*completionHolder)(false);
                        }
                        self->Pump();
                        return;
                    }

                    auto& rawQuadlets = node->MutableROM().rawQuadlets;
                    rawQuadlets.reserve(rawQuadlets.size() + res.quadletsBE.size());
                    rawQuadlets.insert(rawQuadlets.end(), res.quadletsBE.begin(),
                                       res.quadletsBE.end());

                    const bool ok = rawQuadlets.size() >= requiredTotalQuadlets;
                    if (!ok) {
                        ASFW_LOG_V2(ConfigROM,
                                    "EnsurePrefix short read: node=%u have=%zu required=%u", nodeId,
                                    rawQuadlets.size(), requiredTotalQuadlets);
                    }

                    if (completionHolder && *completionHolder) {
                        (*completionHolder)(ok);
                    }
                    self->Pump();
                });
            }
        });
}

void ROMScanSession::MaybeFinish() {
    if (aborted_.load(std::memory_order_relaxed)) {
        return;
    }
    if (completionNotified_ || completion_ == nullptr) {
        return;
    }
    if (gen_ == 0) {
        return;
    }
    if (!nodeScans_.empty() && inflight_ > 0) {
        return;
    }

    const bool allTerminal = std::ranges::all_of(
        nodeScans_, [](const ROMScanNodeStateMachine& node) { return node.IsTerminal(); });
    if (!nodeScans_.empty() && !allTerminal) {
        return;
    }

    completionNotified_ = true;

    auto completion = std::move(completion_);
    auto roms = std::move(completedROMs_);
    const bool hadBusyNodes = hadBusyNodes_;
    const Generation gen = gen_;

    // Make the session inert before calling out.
    nodeScans_.clear();
    completedROMs_.clear();
    inflight_ = 0;
    gen_ = 0;
    hadBusyNodes_ = false;

    if (completion) {
        completion(gen, std::move(roms), hadBusyNodes);
    }
}

} // namespace ASFW::Discovery
