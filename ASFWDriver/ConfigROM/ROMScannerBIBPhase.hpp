#pragma once

#include "ROMScanner.hpp"

namespace ASFW::Discovery {

class ROMScannerBIBPhase {
public:
    static void HandleCompletion(ROMScanner& scanner,
                                 uint8_t nodeId,
                                 const ROMReader::ReadResult& result);
};

} // namespace ASFW::Discovery