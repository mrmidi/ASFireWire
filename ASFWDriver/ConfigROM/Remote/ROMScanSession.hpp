#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include <DriverKit/IOLib.h>

#include "../Common/ConfigROMUnits.hpp"
#include "../ROMReader.hpp"
#include "../ROMScanner.hpp"
#include "ROMScanNodeStateMachine.hpp"

namespace ASFW::Async {
class IFireWireBus;
}

namespace ASFW::Driver {
class TopologyManager;
}

namespace ASFW::Discovery {

class SpeedPolicy;

// Per-generation scan session. Owns the async control flow and guarantees:
// - Single serial execution context for state changes (dispatch queue / internal FIFO)
// - Completion fires exactly once (unless aborted)
// - Late callbacks are ignored after Abort()
class ROMScanSession final : public std::enable_shared_from_this<ROMScanSession> {
  public:
    ROMScanSession(Async::IFireWireBus& bus, SpeedPolicy& speedPolicy, ROMScannerParams params,
                   std::shared_ptr<ROMReader> reader, OSSharedPtr<IODispatchQueue> dispatchQueue,
                   Driver::TopologyManager* topologyManager);
    ~ROMScanSession();

    void Start(ROMScanRequest request, ScanCompletionCallback completion);
    void Abort();

    [[nodiscard]] Generation GetGeneration() const noexcept { return gen_; }

  private:
    struct DiscoveryFlow;

    void Post(std::function<void()> task);
    void DrainPending();
    void Pump();
    void MaybeFinish();

    [[nodiscard]] ROMScanNodeStateMachine* FindNode(uint8_t nodeId);

    [[nodiscard]] static bool TransitionNodeState(ROMScanNodeStateMachine& node,
                                                  ROMScanNodeStateMachine::State next,
                                                  const char* reason);

    void StartBIBRead(ROMScanNodeStateMachine& node);
    void HandleBIBComplete(uint8_t nodeId, ROMReader::ReadResult result);
    void ContinueAfterBIBSuccess(ROMScanNodeStateMachine& node, uint8_t nodeId);

    void StartIRMRead(ROMScanNodeStateMachine& node);
    void HandleIRMReadComplete(uint8_t nodeId, bool success, uint32_t valueHostOrder);
    void StartIRMLock(ROMScanNodeStateMachine& node);
    void HandleIRMLockComplete(uint8_t nodeId, bool success);
    void ContinueAfterIRMCheck(ROMScanNodeStateMachine& node);

    void StartRootDirRead(ROMScanNodeStateMachine& node);
    void HandleRootDirComplete(uint8_t nodeId, ROMReader::ReadResult result);

    void StartDetailsDiscovery(uint8_t nodeId, ASFW::ConfigROM::QuadletOffset rootDirStart,
                               std::vector<uint32_t> rootDirBE);

    void EnsurePrefix(uint8_t nodeId, ASFW::ConfigROM::QuadletCount requiredTotalQuadlets,
                      std::function<void(bool)> completion);

    void RetryWithFallback(ROMScanNodeStateMachine& node);

    void DispatchAsync(std::function<void()> work);

    Async::IFireWireBus& bus_;
    SpeedPolicy& speedPolicy_;
    ROMScannerParams params_;
    OSSharedPtr<IODispatchQueue> dispatchQueue_;
    Driver::TopologyManager* topologyManager_{nullptr};

    std::shared_ptr<ROMReader> reader_;

    std::atomic<bool> aborted_{false};

    Generation gen_{0};
    Driver::TopologySnapshot topology_{};
    uint8_t localNodeId_{0xFF};

    std::vector<ROMScanNodeStateMachine> nodeScans_;
    std::vector<ConfigROM> completedROMs_;

    uint16_t inflight_{0};
    bool hadBusyNodes_{false};
    bool completionNotified_{false};
    ScanCompletionCallback completion_;

    IOLock* executorLock_{nullptr};
    std::deque<std::function<void()>> executorQueue_;
    bool executorDraining_{false};
};

} // namespace ASFW::Discovery
