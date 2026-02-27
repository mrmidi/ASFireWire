#pragma once

#include "ROMScanner.hpp"

namespace ASFW::Discovery {

class ROMScannerDiscoveryFlow {
public:
    static void DiscoverDetails(ROMScanner& scanner,
                                uint8_t nodeId,
                                uint32_t rootDirStart,
                                std::vector<uint32_t>&& rootDirBE);

    static void DiscoverVendorName(ROMScanner& scanner,
                                   uint8_t nodeId,
                                   uint32_t rootDirStart,
                                   std::optional<ROMScanner::DescriptorRef> vendorRef,
                                   std::optional<ROMScanner::DescriptorRef> modelRef,
                                   std::vector<uint32_t> unitDirRelOffsets);

    static void DiscoverModelName(ROMScanner& scanner,
                                  uint8_t nodeId,
                                  uint32_t rootDirStart,
                                  std::optional<ROMScanner::DescriptorRef> modelRef,
                                  std::vector<uint32_t> unitDirRelOffsets);

    static void DiscoverUnitDirectories(ROMScanner& scanner,
                                        uint8_t nodeId,
                                        uint32_t rootDirStart,
                                        std::vector<uint32_t> unitDirRelOffsets,
                                        size_t index);

    static void FinalizeNodeDiscovery(ROMScanner& scanner,
                                      uint8_t nodeId);

    static void FetchTextDescriptor(ROMScanner& scanner,
                                    uint8_t nodeId,
                                    uint32_t absOffset,
                                    uint8_t keyType,
                                    std::function<void(std::string)> completion);

    static void FetchTextLeaf(ROMScanner& scanner,
                              uint8_t nodeId,
                              uint32_t absLeafOffset,
                              std::function<void(std::string)> completion);

    static void FetchDescriptorDirText(ROMScanner& scanner,
                                       uint8_t nodeId,
                                       uint32_t absDirOffset,
                                       std::function<void(std::string)> completion);

    static void TryFetchNextTextCandidate(ROMScanner& scanner,
                                          uint8_t nodeId,
                                          std::vector<uint32_t> candidates,
                                          size_t index,
                                          std::function<void(std::string)> completion);

    static std::vector<uint32_t> FindTextDescriptorLeafCandidates(const std::vector<uint32_t>& quadlets,
                                                                  uint32_t absDirOffset,
                                                                  uint16_t dirLen);

    static std::vector<ROMScanner::DirEntry> ParseDirectory(const std::vector<uint32_t>& dirBE,
                                                            uint32_t entryCap);

    static std::optional<ROMScanner::DescriptorRef> FindDescriptorRef(const std::vector<ROMScanner::DirEntry>& entries,
                                                                      uint8_t ownerKeyId);

    static void OnFetchTextLeafHeaderReady(ROMScanner& scanner,
                                           uint8_t nodeId,
                                           uint32_t absLeafOffset,
                                                                                     std::shared_ptr<std::function<void(std::string)>> completion,
                                           bool ok);

    static void OnFetchTextLeafDataReady(ROMScanner& scanner,
                                         uint8_t nodeId,
                                         uint32_t absLeafOffset,
                                                                                 const std::shared_ptr<std::function<void(std::string)>>& completion,
                                         bool ok);

    static void OnFetchDescriptorDirHeaderReady(ROMScanner& scanner,
                                                uint8_t nodeId,
                                                uint32_t absDirOffset,
                                                                                                std::shared_ptr<std::function<void(std::string)>> completion,
                                                bool ok);

    static void OnFetchDescriptorDirDataReady(ROMScanner& scanner,
                                              uint8_t nodeId,
                                              uint32_t absDirOffset,
                                              uint16_t dirLen,
                                                                                            std::shared_ptr<std::function<void(std::string)>> completion,
                                              bool ok);

    static void OnDiscoverDetailsPrefixReady(ROMScanner& scanner,
                                             uint8_t nodeId,
                                             uint32_t rootDirStart,
                                             const std::vector<uint32_t>& rootDirBE,
                                             bool prefixOk);

    static void OnUnitDirHeaderReady(ROMScanner& scanner,
                                     ROMScanner::UnitDirStepContext context,
                                     bool ok);

    static void OnUnitDirDataReady(ROMScanner& scanner,
                                   ROMScanner::UnitDirStepContext context,
                                   bool ok);
};

} // namespace ASFW::Discovery
