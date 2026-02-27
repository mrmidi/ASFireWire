#include "ROMScannerDiscoveryFlow.hpp"

#include "ConfigROMStore.hpp"
#include "../Logging/Logging.hpp"
#include "../Logging/LogConfig.hpp"
#include <DriverKit/IOLib.h>

#include <span>
#include <string_view>

namespace ASFW::Discovery {

void ROMScannerDiscoveryFlow::OnFetchTextLeafHeaderReady(ROMScanner& scanner,
                                                         uint8_t nodeId,
                                                         uint32_t absLeafOffset,
                                                         std::shared_ptr<std::function<void(std::string)>> completion,
                                                         bool ok) {
    if (!ok) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    const auto* node = scanner.FindNodeScan(nodeId);
    if (!node || absLeafOffset >= node->ROM().rawQuadlets.size()) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    const uint32_t hdr = OSSwapBigToHostInt32(node->ROM().rawQuadlets[absLeafOffset]);
    const auto leafLen = static_cast<uint16_t>((hdr >> 16) & 0xFFFFu);
    const uint32_t leafEndExclusive = absLeafOffset + 1u + static_cast<uint32_t>(leafLen);

    scanner.EnsurePrefix(nodeId,
                         leafEndExclusive,
                         [&scanner, nodeId, absLeafOffset, completion](bool ok2) {
                             OnFetchTextLeafDataReady(scanner, nodeId, absLeafOffset, completion, ok2);
                         });
}

void ROMScannerDiscoveryFlow::OnFetchTextLeafDataReady(ROMScanner& scanner,
                                                       uint8_t nodeId,
                                                       uint32_t absLeafOffset,
                                                       const std::shared_ptr<std::function<void(std::string)>>& completion,
                                                       bool ok) {
    if (!ok) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    const auto* node = scanner.FindNodeScan(nodeId);
    if (!node) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    std::string text = ConfigROMParser::ParseTextDescriptorLeaf(
        std::span<const uint32_t>(node->ROM().rawQuadlets.data(),
                                  node->ROM().rawQuadlets.size()),
        absLeafOffset,
        "big");
    if (completion && *completion) {
        (*completion)(std::move(text));
    }
}

void ROMScannerDiscoveryFlow::OnFetchDescriptorDirHeaderReady(ROMScanner& scanner,
                                                              uint8_t nodeId,
                                                              uint32_t absDirOffset,
                                                              std::shared_ptr<std::function<void(std::string)>> completion,
                                                              bool ok) {
    if (!ok) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    const auto* node = scanner.FindNodeScan(nodeId);
    if (!node || absDirOffset >= node->ROM().rawQuadlets.size()) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    const uint32_t hdr = OSSwapBigToHostInt32(node->ROM().rawQuadlets[absDirOffset]);
    auto dirLen = static_cast<uint16_t>((hdr >> 16) & 0xFFFFu);
    if (dirLen == 0) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }
    if (dirLen > 32) {
        dirLen = 32;
    }

    const uint32_t dirEndExclusive = absDirOffset + 1u + static_cast<uint32_t>(dirLen);
    scanner.EnsurePrefix(nodeId,
                         dirEndExclusive,
                         [&scanner, nodeId, absDirOffset, dirLen, completion](bool ok2) {
                             OnFetchDescriptorDirDataReady(scanner, nodeId, absDirOffset, dirLen, completion, ok2);
                         });
}

void ROMScannerDiscoveryFlow::OnFetchDescriptorDirDataReady(ROMScanner& scanner,
                                                            uint8_t nodeId,
                                                            uint32_t absDirOffset,
                                                            uint16_t dirLen,
                                                            std::shared_ptr<std::function<void(std::string)>> completion,
                                                            bool ok) {
    if (!ok) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    const auto* node = scanner.FindNodeScan(nodeId);
    if (!node || absDirOffset + dirLen >= node->ROM().rawQuadlets.size()) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    std::vector<uint32_t> leafCandidates = ROMScannerDiscoveryFlow::FindTextDescriptorLeafCandidates(
        node->ROM().rawQuadlets,
        absDirOffset,
        dirLen);
    if (leafCandidates.empty()) {
        if (completion && *completion) {
            (*completion)("");
        }
        return;
    }

    ROMScannerDiscoveryFlow::TryFetchNextTextCandidate(scanner,
                                                       nodeId,
                                                       std::move(leafCandidates),
                                                       0,
                                                       [completion = std::move(completion)](std::string text) {
                                          if (completion && *completion) {
                                              (*completion)(std::move(text));
                                          }
                                      });
}

void ROMScannerDiscoveryFlow::OnDiscoverDetailsPrefixReady(ROMScanner& scanner,
                                                           uint8_t nodeId,
                                                           uint32_t rootDirStart,
                                                           const std::vector<uint32_t>& rootDirBE,
                                                           bool prefixOk) {
    auto* node = scanner.FindNodeScan(nodeId);
    if (!node) {
        scanner.CheckAndNotifyCompletion();
        scanner.ScheduleAdvanceFSM();
        return;
    }

    if (!prefixOk) {
        ASFW_LOG(ConfigROM,
                 "Node %u: ROM prefix could not be extended to rootDirStart=%u",
                 nodeId,
                 rootDirStart);
    }

    auto& rawQuadlets = node->MutableROM().rawQuadlets;
    rawQuadlets.reserve(rawQuadlets.size() + rootDirBE.size());
    for (uint32_t q : rootDirBE) {
        rawQuadlets.push_back(q);
    }

    auto rootEntries = ROMScannerDiscoveryFlow::ParseDirectory(rootDirBE, 64);
    auto vendorRef = ROMScannerDiscoveryFlow::FindDescriptorRef(rootEntries, ASFW::FW::ConfigKey::kModuleVendorId);
    auto modelRef = ROMScannerDiscoveryFlow::FindDescriptorRef(rootEntries, ASFW::FW::ConfigKey::kModelId);

    std::vector<uint32_t> unitDirRelOffsets;
    for (const auto& e : rootEntries) {
        if (e.keyType == ASFW::FW::EntryType::kDirectory &&
            e.keyId == ASFW::FW::ConfigKey::kUnitDirectory &&
            e.hasTarget && e.targetRel != 0) {
            unitDirRelOffsets.push_back(e.targetRel);
        }
    }

    ROMScannerDiscoveryFlow::DiscoverVendorName(scanner,
                                                nodeId,
                                                rootDirStart,
                                                vendorRef,
                                                modelRef,
                                                std::move(unitDirRelOffsets));
}

void ROMScannerDiscoveryFlow::OnUnitDirHeaderReady(ROMScanner& scanner,
                                                   ROMScanner::UnitDirStepContext context,
                                                   bool ok) {
    if (!ok) {
        ROMScannerDiscoveryFlow::DiscoverUnitDirectories(scanner,
                                                         context.nodeId,
                                                         context.rootDirStart,
                                                         std::move(context.unitDirRelOffsets),
                                                         context.index + 1);
        return;
    }

    const auto* node = scanner.FindNodeScan(context.nodeId);
    if (!node || context.absUnitDir >= node->ROM().rawQuadlets.size()) {
        ROMScannerDiscoveryFlow::FinalizeNodeDiscovery(scanner, context.nodeId);
        return;
    }

    const uint32_t hdr = OSSwapBigToHostInt32(node->ROM().rawQuadlets[context.absUnitDir]);
    context.dirLen = static_cast<uint16_t>((hdr >> 16) & 0xFFFFu);
    if (context.dirLen == 0) {
        ROMScannerDiscoveryFlow::DiscoverUnitDirectories(scanner,
                                                         context.nodeId,
                                                         context.rootDirStart,
                                                         std::move(context.unitDirRelOffsets),
                                                         context.index + 1);
        return;
    }
    if (context.dirLen > 32) {
        context.dirLen = 32;
    }

    const uint32_t dirEndExclusive = context.absUnitDir + 1u + static_cast<uint32_t>(context.dirLen);
    scanner.EnsurePrefix(context.nodeId,
                         dirEndExclusive,
                         [&scanner, context = std::move(context)](bool ok2) mutable {
                             OnUnitDirDataReady(scanner, std::move(context), ok2);
                         });
}

void ROMScannerDiscoveryFlow::OnUnitDirDataReady(ROMScanner& scanner,
                                                 ROMScanner::UnitDirStepContext context,
                                                 bool ok) {
    if (!ok) {
        ROMScannerDiscoveryFlow::DiscoverUnitDirectories(scanner,
                                                         context.nodeId,
                                                         context.rootDirStart,
                                                         std::move(context.unitDirRelOffsets),
                                                         context.index + 1);
        return;
    }

    auto* node = scanner.FindNodeScan(context.nodeId);
    if (!node || context.absUnitDir + context.dirLen >= node->ROM().rawQuadlets.size()) {
        ROMScannerDiscoveryFlow::FinalizeNodeDiscovery(scanner, context.nodeId);
        return;
    }

    std::vector<uint32_t> unitDirBE;
    for (uint32_t i = 0; i <= static_cast<uint32_t>(context.dirLen); ++i) {
        unitDirBE.push_back(node->ROM().rawQuadlets[context.absUnitDir + i]);
    }

    auto unitEntries = ROMScannerDiscoveryFlow::ParseDirectory(unitDirBE, 32);
    UnitDirectory parsed{};
    parsed.offsetQuadlets = context.unitRel;

    for (const auto& e : unitEntries) {
        if (e.keyType == ASFW::FW::EntryType::kImmediate) {
            switch (e.keyId) {
                case ASFW::FW::ConfigKey::kUnitSpecId:
                    parsed.unitSpecId = e.value;
                    break;
                case ASFW::FW::ConfigKey::kUnitSwVersion:
                    parsed.unitSwVersion = e.value;
                    break;
                case ASFW::FW::ConfigKey::kUnitDependentInfo:
                    parsed.logicalUnitNumber = e.value;
                    break;
                case ASFW::FW::ConfigKey::kModelId:
                    parsed.modelId = e.value;
                    break;
                default:
                    break;
            }
        }
    }

    auto unitModelRef = ROMScannerDiscoveryFlow::FindDescriptorRef(unitEntries, ASFW::FW::ConfigKey::kModelId);
    if (!unitModelRef) {
        node->MutableROM().unitDirectories.push_back(std::move(parsed));
        ROMScannerDiscoveryFlow::DiscoverUnitDirectories(scanner,
                                                         context.nodeId,
                                                         context.rootDirStart,
                                                         std::move(context.unitDirRelOffsets),
                                                         context.index + 1);
        return;
    }

    ROMScannerDiscoveryFlow::FetchTextDescriptor(scanner,
                                context.nodeId,
                                context.absUnitDir + unitModelRef->targetRel,
                                unitModelRef->keyType,
                                [&scanner,
                                 nodeId = context.nodeId,
                                 rootDirStart = context.rootDirStart,
                                 parsed = std::move(parsed),
                                 unitDirRelOffsets = std::move(context.unitDirRelOffsets),
                                 index = context.index](std::string_view name) mutable {
                                    if (auto* node2 = scanner.FindNodeScan(nodeId); node2) {
                                        UnitDirectory unit = std::move(parsed);
                                        if (!name.empty()) {
                                            unit.modelName = std::string(name);
                                        }
                                        node2->MutableROM().unitDirectories.push_back(std::move(unit));
                                    }
                                    ROMScannerDiscoveryFlow::DiscoverUnitDirectories(scanner,
                                                                                     nodeId,
                                                                                     rootDirStart,
                                                                                     std::move(unitDirRelOffsets),
                                                                                     index + 1);
                                });
}

} // namespace ASFW::Discovery
