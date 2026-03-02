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
    if (gen == 0 || !session_) {
        return false;
    }
    return session_->GetGeneration() == gen;
}

bool ROMScanner::Start(const ROMScanRequest& request, ScanCompletionCallback completion) {
    if (IsBusyFor(request.gen)) {
        ASFW_LOG_V2(ConfigROM, "ROMScanner::Start: scan already in progress for gen=%u",
                    request.gen);
        return false;
    }

    if (session_) {
        session_->Abort();
        session_.reset();
    }

    ASFW_LOG_V2(ConfigROM, "ROMScanner::Start gen=%u localNode=%u topologyNodes=%zu targets=%zu",
                request.gen, request.localNodeId, request.topology.nodes.size(),
                request.targetNodes.size());

    auto session = std::make_shared<ROMScanSession>(bus_, speedPolicy_, params_, reader_,
                                                    dispatchQueue_, topologyManager_);
    session_ = session;

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

void ROMScanner::Abort(Generation gen) {
    if (!session_) {
        return;
    }
    if (gen == 0 || session_->GetGeneration() != gen) {
        return;
    }

    session_->Abort();
    session_.reset();
}

} // namespace ASFW::Discovery
