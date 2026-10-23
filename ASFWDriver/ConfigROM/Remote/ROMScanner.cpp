#include "../ROMScanner.hpp"
#include "../ROMReader.hpp"

#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"
#include "ROMScanSession.hpp"

#include <utility>

namespace ASFW::Discovery {

ROMScanner::ROMScanner(Async::IFireWireBus& bus, SpeedPolicy& speedPolicy,
                       const ROMScannerParams& params, OSSharedPtr<IODispatchQueue> dispatchQueue)
    : bus_(bus), speedPolicy_(speedPolicy), params_(params),
      dispatchQueue_(std::move(dispatchQueue)),
      reader_(std::make_shared<ROMReader>(bus_, dispatchQueue_)) {}

ROMScanner::~ROMScanner() {
    if (session_) {
        session_->Abort();
        session_.reset();
    }
}

void ROMScanner::SetTopologyManager(Driver::TopologyManager* topologyManager) {
    topologyManager_ = topologyManager;
}

bool ROMScanner::IsBusyFor(Generation gen) const {
    // Generation 0 is a real hardware value after the 8-bit counter wraps.
    return session_ && sessionGen_ == gen;
}

bool ROMScanner::Start(const ROMScanRequest& request, ScanCompletionCallback completion) {
    if (IsBusyFor(request.gen)) {
        ASFW_LOG_V2(ConfigROM, "ROMScanner::Start: scan already in progress for gen=%u",
                    request.gen.value);
        return false;
    }

    if (session_) {
        session_->Abort();
        session_.reset();
    }

    ASFW_LOG_V2(ConfigROM, "ROMScanner::Start gen=%u localNode=%u topologyNodes=%zu targets=%zu",
                request.gen.value, request.localNodeId, request.topology.physical.nodes.size(),
                request.targetNodes.size());

    auto session = std::make_shared<ROMScanSession>(bus_, speedPolicy_, params_, reader_,
                                                    dispatchQueue_, topologyManager_);
    session_ = session;
    sessionGen_ = request.gen;

    const std::weak_ptr<ROMScanSession> weakSession = session;
    ScanCompletionCallback wrapped = [this, weakSession, completion = std::move(completion)](
                                         Generation gen, std::vector<ConfigROM> roms,
                                         bool hadBusyNodes) mutable {
        if (auto strongSession = weakSession.lock(); strongSession && session_ == strongSession) {
            session_.reset();
        }
        if (completion) {
            completion(gen, std::move(roms), hadBusyNodes);
        }
    };

    session->Start(request, std::move(wrapped));
    return true;
}

bool ROMScanner::Abort() {
    if (!session_) {
        return false;
    }

    session_->Abort();
    session_.reset();
    return true;
}

} // namespace ASFW::Discovery
