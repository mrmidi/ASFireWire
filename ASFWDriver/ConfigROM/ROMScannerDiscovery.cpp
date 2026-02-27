#include "ROMScanner.hpp"
#include "ROMScannerDiscoveryFlow.hpp"
#include "ConfigROMConstants.hpp"
#include "ConfigROMStore.hpp"
#include "../Logging/Logging.hpp"
#include "../Logging/LogConfig.hpp"
#include <DriverKit/IOLib.h>

#include <algorithm>
#include <string_view>

namespace ASFW::Discovery {

std::vector<ROMScanner::DirEntry> ROMScannerDiscoveryFlow::ParseDirectory(const std::vector<uint32_t>& dirBE,
                                                                          uint32_t entryCap) {
    std::vector<ROMScanner::DirEntry> out;
    if (dirBE.empty()) {
        return out;
    }

    const uint32_t hdr = OSSwapBigToHostInt32(dirBE[0]);
    const uint32_t len = (hdr >> 16) & 0xFFFFu;
    const auto available = static_cast<uint32_t>(dirBE.size() - 1);
    uint32_t count = std::min({len, available, entryCap});

    out.reserve(count);
    for (uint32_t i = 1; i <= count; ++i) {
        const uint32_t entry = OSSwapBigToHostInt32(dirBE[i]);
        auto e = ROMScanner::DirEntry{};
        e.index = i;
        e.keyType = static_cast<uint8_t>((entry >> 30) & 0x3u);
        e.keyId = static_cast<uint8_t>((entry >> 24) & 0x3Fu);
        e.value = entry & 0x00FFFFFFu;

        if (e.keyType == ASFW::FW::EntryType::kLeaf || e.keyType == ASFW::FW::EntryType::kDirectory) {
            const int32_t off = (e.value & 0x00800000u)
                                    ? static_cast<int32_t>(e.value | 0xFF000000u)
                                    : static_cast<int32_t>(e.value);
            const int32_t rel = static_cast<int32_t>(i) + off;
            if (rel >= 0) {
                e.hasTarget = true;
                e.targetRel = static_cast<uint32_t>(rel);
            }
        }
        out.push_back(e);
    }
    return out;
}

std::optional<ROMScanner::DescriptorRef> ROMScannerDiscoveryFlow::FindDescriptorRef(const std::vector<ROMScanner::DirEntry>& entries,
                                                                                     uint8_t ownerKeyId) {
    for (size_t i = 0; i < entries.size(); ++i) {
        if (const auto& e = entries[i];
            e.keyType != ASFW::FW::EntryType::kImmediate || e.keyId != ownerKeyId) {
            continue;
        }
        if (i + 1 >= entries.size()) {
            return std::nullopt;
        }

        const auto& d = entries[i + 1];
        if (d.keyId != ASFW::FW::ConfigKey::kTextualDescriptor) {
            return std::nullopt;
        }
        if (d.keyType != ASFW::FW::EntryType::kLeaf && d.keyType != ASFW::FW::EntryType::kDirectory) {
            return std::nullopt;
        }
        if (!d.hasTarget || d.targetRel == 0) {
            return std::nullopt;
        }
        return ROMScanner::DescriptorRef{.keyType = d.keyType, .targetRel = d.targetRel};
    }
    return std::nullopt;
}

void ROMScannerDiscoveryFlow::FetchTextDescriptor(ROMScanner& scanner,
                                                  uint8_t nodeId,
                                                  uint32_t absOffset,
                                                  uint8_t keyType,
                                                  std::function<void(std::string)> completion) {
    if (keyType == ASFW::FW::EntryType::kLeaf) {
        FetchTextLeaf(scanner, nodeId, absOffset, std::move(completion));
    } else if (keyType == ASFW::FW::EntryType::kDirectory) {
        FetchDescriptorDirText(scanner, nodeId, absOffset, std::move(completion));
    } else if (completion) {
        completion("");
    }
}

void ROMScannerDiscoveryFlow::FetchTextLeaf(ROMScanner& scanner,
                                            uint8_t nodeId,
                                            uint32_t absLeafOffset,
                                            std::function<void(std::string)> completion) {
    auto completionHolder = std::make_shared<std::function<void(std::string)>>(std::move(completion));
    scanner.EnsurePrefix(nodeId,
                         absLeafOffset + 1,
                         [&scanner, nodeId, absLeafOffset, completion = std::move(completionHolder)](bool ok) {
                     ROMScannerDiscoveryFlow::OnFetchTextLeafHeaderReady(scanner, nodeId, absLeafOffset, completion, ok);
                 });
}

void ROMScannerDiscoveryFlow::FetchDescriptorDirText(ROMScanner& scanner,
                                                     uint8_t nodeId,
                                                     uint32_t absDirOffset,
                                                     std::function<void(std::string)> completion) {
    auto completionHolder = std::make_shared<std::function<void(std::string)>>(std::move(completion));
    scanner.EnsurePrefix(nodeId,
                         absDirOffset + 1,
                         [&scanner, nodeId, absDirOffset, completion = std::move(completionHolder)](bool ok) {
                     ROMScannerDiscoveryFlow::OnFetchDescriptorDirHeaderReady(scanner, nodeId, absDirOffset, completion, ok);
                 });
}

std::vector<uint32_t> ROMScannerDiscoveryFlow::FindTextDescriptorLeafCandidates(const std::vector<uint32_t>& quadlets,
                                                                                uint32_t absDirOffset,
                                                                                uint16_t dirLen) {
    std::vector<uint32_t> leafCandidates;
    for (uint32_t i = 1; i <= static_cast<uint32_t>(dirLen); ++i) {
        if (absDirOffset + i >= quadlets.size()) {
            break;
        }
        const uint32_t entry = OSSwapBigToHostInt32(quadlets[absDirOffset + i]);
        const auto keyType = static_cast<uint8_t>((entry >> 30) & 0x3u);
        const auto keyId = static_cast<uint8_t>((entry >> 24) & 0x3Fu);
        const uint32_t value = entry & 0x00FFFFFFu;

        if (keyId == ASFW::FW::ConfigKey::kTextualDescriptor && keyType == ASFW::FW::EntryType::kLeaf) {
            const int32_t off = (value & 0x00800000u)
                                    ? static_cast<int32_t>(value | 0xFF000000u)
                                    : static_cast<int32_t>(value);
            const int32_t rel = static_cast<int32_t>(i) + off;
            if (rel >= 0) {
                leafCandidates.push_back(absDirOffset + static_cast<uint32_t>(rel));
            }
        }
    }
    return leafCandidates;
}

void ROMScannerDiscoveryFlow::TryFetchNextTextCandidate(ROMScanner& scanner,
                                                        uint8_t nodeId,
                                                        std::vector<uint32_t> candidates,
                                                        size_t index,
                                                        std::function<void(std::string)> completion) {
    if (index >= candidates.size()) {
        if (completion) {
            completion("");
        }
        return;
    }

    uint32_t leafAbs = candidates[index];
    FetchTextLeaf(scanner, nodeId, leafAbs,
                  [&scanner, nodeId, candidates = std::move(candidates), index, completion = std::move(completion)](std::string text) mutable {
                      if (!text.empty()) {
                          if (completion) {
                              completion(std::move(text));
                          }
                      } else {
                          TryFetchNextTextCandidate(scanner,
                                                    nodeId,
                                                    std::move(candidates),
                                                    index + 1,
                                                    std::move(completion));
                      }
                  });
}

void ROMScannerDiscoveryFlow::DiscoverDetails(ROMScanner& scanner,
                                              uint8_t nodeId,
                                              uint32_t rootDirStart,
                                              std::vector<uint32_t>&& rootDirBE) {
    auto* node = scanner.FindNodeScan(nodeId);
    if (!node) {
        scanner.CheckAndNotifyCompletion();
        scanner.ScheduleAdvanceFSM();
        return;
    }

    if (!scanner.TransitionNodeState(*node,
                                     ROMScanner::NodeState::ReadingDetails,
                                     "RootDir parsed enter details discovery")) {
        scanner.CheckAndNotifyCompletion();
        scanner.ScheduleAdvanceFSM();
        return;
    }

    scanner.EnsurePrefix(nodeId,
                         rootDirStart,
                         [&scanner, nodeId, rootDirStart, rootDirBE = std::move(rootDirBE)](bool prefixOk) mutable {
                     ROMScannerDiscoveryFlow::OnDiscoverDetailsPrefixReady(scanner, nodeId, rootDirStart, rootDirBE, prefixOk);
                 });
}

void ROMScannerDiscoveryFlow::DiscoverVendorName(ROMScanner& scanner,
                                                 uint8_t nodeId,
                                                 uint32_t rootDirStart,
                                                 std::optional<ROMScanner::DescriptorRef> vendorRef,
                                                 std::optional<ROMScanner::DescriptorRef> modelRef,
                                                 std::vector<uint32_t> unitDirRelOffsets) {
    if (!vendorRef) {
        DiscoverModelName(scanner, nodeId, rootDirStart, modelRef, std::move(unitDirRelOffsets));
        return;
    }

    FetchTextDescriptor(scanner,
                        nodeId,
                        rootDirStart + vendorRef->targetRel,
                        vendorRef->keyType,
                        [&scanner, nodeId, rootDirStart, modelRef, unitDirRelOffsets = std::move(unitDirRelOffsets)](std::string_view vendor) mutable {
                            if (auto* node = scanner.FindNodeScan(nodeId); node && !vendor.empty()) {
                                node->MutableROM().vendorName.assign(vendor);
                            }
                            DiscoverModelName(scanner, nodeId, rootDirStart, modelRef, std::move(unitDirRelOffsets));
                        });
}

void ROMScannerDiscoveryFlow::DiscoverModelName(ROMScanner& scanner,
                                                uint8_t nodeId,
                                                uint32_t rootDirStart,
                                                std::optional<ROMScanner::DescriptorRef> modelRef,
                                                std::vector<uint32_t> unitDirRelOffsets) {
    if (!modelRef) {
        DiscoverUnitDirectories(scanner, nodeId, rootDirStart, std::move(unitDirRelOffsets), 0);
        return;
    }

    FetchTextDescriptor(scanner,
                        nodeId,
                        rootDirStart + modelRef->targetRel,
                        modelRef->keyType,
                        [&scanner, nodeId, rootDirStart, unitDirRelOffsets = std::move(unitDirRelOffsets)](std::string_view model) mutable {
                            if (auto* node = scanner.FindNodeScan(nodeId); node && !model.empty()) {
                                node->MutableROM().modelName.assign(model);
                            }
                            DiscoverUnitDirectories(scanner, nodeId, rootDirStart, std::move(unitDirRelOffsets), 0);
                        });
}

void ROMScannerDiscoveryFlow::DiscoverUnitDirectories(ROMScanner& scanner,
                                                      uint8_t nodeId,
                                                      uint32_t rootDirStart,
                                                      std::vector<uint32_t> unitDirRelOffsets,
                                                      size_t index) {
    if (index >= unitDirRelOffsets.size()) {
        FinalizeNodeDiscovery(scanner, nodeId);
        return;
    }

    const uint32_t unitRel = unitDirRelOffsets[index];
    const uint32_t absUnitDir = rootDirStart + unitRel;

    scanner.EnsurePrefix(nodeId,
                         absUnitDir + 1,
                 [&scanner,
                  nodeId,
                  rootDirStart,
                  unitDirRelOffsets = std::move(unitDirRelOffsets),
                  index,
                  absUnitDir,
                  unitRel](bool ok) mutable {
                     ROMScanner::UnitDirStepContext context{};
                     context.nodeId = nodeId;
                     context.rootDirStart = rootDirStart;
                     context.unitDirRelOffsets = std::move(unitDirRelOffsets);
                     context.index = index;
                     context.absUnitDir = absUnitDir;
                     context.unitRel = unitRel;
                     ROMScannerDiscoveryFlow::OnUnitDirHeaderReady(scanner, std::move(context), ok);
                 });
}

void ROMScannerDiscoveryFlow::FinalizeNodeDiscovery(ROMScanner& scanner,
                                                    uint8_t nodeId) {
    auto* node = scanner.FindNodeScan(nodeId);
    if (!node) {
        scanner.CheckAndNotifyCompletion();
        scanner.ScheduleAdvanceFSM();
        return;
    }

    scanner.speedPolicy_.RecordSuccess(nodeId, node->CurrentSpeed());
    if (!scanner.TransitionNodeState(*node, ROMScanner::NodeState::Complete, "FinalizeNodeDiscovery complete")) {
        scanner.CheckAndNotifyCompletion();
        scanner.ScheduleAdvanceFSM();
        return;
    }
    scanner.completedROMs_.push_back(std::move(node->MutableROM()));
    scanner.CheckAndNotifyCompletion();
    scanner.ScheduleAdvanceFSM();
}

void ROMScanner::OnRootDirComplete(uint8_t nodeId, const ROMReader::ReadResult& result) {
    DecrementInflight();

    auto* nodePtr = FindNodeScan(nodeId);
    if (!nodePtr) {
        CheckAndNotifyCompletion();
        ScheduleAdvanceFSM();
        return;
    }

    auto& node = *nodePtr;

    if (!result.success || !result.data || result.dataLength < 4) {
        ASFW_LOG(ConfigROM, "FSM: Node %u RootDir read failed - marking as failed", nodeId);
        TransitionNodeState(node, NodeState::Failed, "RootDir read failed");
        CheckAndNotifyCompletion();
        ScheduleAdvanceFSM();
        return;
    }

    const uint32_t quadletCount = result.dataLength / 4;
    node.MutableROM().rootDirMinimal = ConfigROMParser::ParseRootDirectory(result.data, quadletCount);

    std::vector<uint32_t> rootDirBE;
    rootDirBE.reserve(quadletCount);
    for (uint32_t i = 0; i < quadletCount; ++i) {
        rootDirBE.push_back(result.data[i]);
    }

    const uint32_t rootDirStart = ASFW::ConfigROM::RootDirStartQuadlet(node.ROM().bib);
    ROMScannerDiscoveryFlow::DiscoverDetails(*this, nodeId, rootDirStart, std::move(rootDirBE));
}

} // namespace ASFW::Discovery
