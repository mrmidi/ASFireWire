#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSSharedPtr.h>
#endif

#include "../Controller/ControllerTypes.hpp"
#include "../Discovery/DiscoveryTypes.hpp"
#include "../Discovery/SpeedPolicy.hpp"

namespace ASFW::Async {
class IFireWireBus;
}

namespace ASFW::Driver {
class TopologyManager;
}

namespace ASFW::Discovery {

class ROMReader;
class ROMScanSession;

struct ROMScanRequest {
    Generation gen{0};
    Driver::TopologySnapshot topology;
    uint8_t localNodeId{0xFF};

    // If empty: scan all remote nodes (excluding local node, and skipping link-inactive nodes).
    // If non-empty: scan only these node IDs (local node is always skipped).
    std::vector<uint8_t> targetNodes;
};

using ScanCompletionCallback =
    std::function<void(Generation gen, std::vector<ConfigROM> roms, bool hadBusyNodes)>;

// Session-driven ROM scanner with bounded concurrency and retry logic.
// Completion fires exactly once for each successful Start().
class ROMScanner {
  public:
    explicit ROMScanner(Async::IFireWireBus& bus, SpeedPolicy& speedPolicy,
                        const ROMScannerParams& params,
                        OSSharedPtr<IODispatchQueue> dispatchQueue = nullptr);
    ~ROMScanner();

    // Start a scan for a given generation request.
    // Completion is fired exactly once for each successful Start().
    [[nodiscard]] bool Start(const ROMScanRequest& request, ScanCompletionCallback completion);

    // Cancel scan for given generation (abort in-flight operations)
    void Abort(Generation gen);

    void SetTopologyManager(Driver::TopologyManager* topologyManager);

  private:
    [[nodiscard]] bool IsBusyFor(Generation gen) const;

    Async::IFireWireBus& bus_;
    SpeedPolicy& speedPolicy_;
    ROMScannerParams params_;
    OSSharedPtr<IODispatchQueue> dispatchQueue_;
    Driver::TopologyManager* topologyManager_{nullptr};

    std::shared_ptr<ROMReader> reader_;
    std::shared_ptr<ROMScanSession> session_;
};

} // namespace ASFW::Discovery
