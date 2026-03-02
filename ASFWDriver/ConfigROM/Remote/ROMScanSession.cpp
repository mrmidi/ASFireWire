#include "ROMScanSession.hpp"

#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"
#include "../Common/ConfigROMConstants.hpp"
#include "../Common/ConfigROMPolicies.hpp"
#include "../ConfigROMParser.hpp"

#include <DriverKit/IOLib.h>

#include <algorithm>
#include <utility>

namespace ASFW::Discovery {

namespace {

void LogBIBReadFailed(uint8_t nodeId) {
    ASFW_LOG(ConfigROM, "ROMScanSession: Node %u BIB read failed, retrying", nodeId);
}

void LogBIBBootingRetry(uint8_t nodeId) {
    ASFW_LOG(ConfigROM, "ROMScanSession: Node %u BIB quadlet[0]=0 (booting), retry", nodeId);
}

void LogBIBShortRead(uint8_t nodeId, size_t quadletCount) {
    ASFW_LOG(ConfigROM, "ROMScanSession: Node %u BIB read returned %zu quadlets (expected %u)",
             nodeId, quadletCount, ASFW::ConfigROM::kBIBQuadletCount);
}

void LogBIBParseFailed(uint8_t nodeId, ConfigROMParser::Error error) {
    ASFW_LOG_V1(ConfigROM, "ROMScanSession: Node %u BIB parse failed (code=%u offset=%u)", nodeId,
                static_cast<uint8_t>(error.code), error.offsetQuadlets);
}

void LogBIBCRCMismatch(uint8_t nodeId, uint16_t computed, uint16_t expected) {
    ASFW_LOG_V1(ConfigROM,
                "ROMScanSession: Node %u BIB CRC mismatch (computed=0x%04x expected=0x%04x)",
                nodeId, computed, expected);
}

} // namespace

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
        LogBIBReadFailed(nodeId);
        hadBusyNodes_ = true;
        RetryWithFallback(node);
        Pump();
        return;
    }

    // IEEE 1212 allows temporary zero in header while device is still booting.
    if (!result.quadletsBE.empty() && result.quadletsBE[0] == 0) {
        LogBIBBootingRetry(nodeId);
        hadBusyNodes_ = true;
        RetryWithFallback(node);
        Pump();
        return;
    }

    if (result.quadletsBE.size() < ASFW::ConfigROM::kBIBQuadletCount) {
        LogBIBShortRead(nodeId, result.quadletsBE.size());
        (void)TransitionNodeState(node, ROMScanNodeStateMachine::State::Failed, "BIB short read");
        Pump();
        return;
    }

    auto bibRes = ConfigROMParser::ParseBIB(result.QuadletsBE());
    if (!bibRes) {
        LogBIBParseFailed(nodeId, bibRes.error());
        (void)TransitionNodeState(node, ROMScanNodeStateMachine::State::Failed, "BIB parse failed");
        Pump();
        return;
    }

    if (bibRes->crcStatus == ConfigROMParser::CRCStatus::Mismatch) {
        LogBIBCRCMismatch(nodeId, bibRes->computed.value_or(0), bibRes->bib.crc);
    }

    node.MutableROM().bib = bibRes->bib;

    node.MutableROM().rawQuadlets.clear();
    node.MutableROM().rawQuadlets.reserve(std::max<size_t>(256U, result.quadletsBE.size()));
    node.MutableROM().rawQuadlets.insert(node.MutableROM().rawQuadlets.end(),
                                         result.quadletsBE.begin(), result.quadletsBE.end());

    speedPolicy_.RecordSuccess(nodeId, node.CurrentSpeed());

    ContinueAfterBIBSuccess(node, nodeId);
}

void ROMScanSession::ContinueAfterBIBSuccess(ROMScanNodeStateMachine& node, uint8_t nodeId) {
    if (params_.doIRMCheck && topology_.irmNodeId.has_value() && *topology_.irmNodeId == nodeId &&
        node.ROM().bib.irmc) {
        StartIRMRead(node);
        return;
    }

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
    auto rootDir = ConfigROMParser::ParseRootDirectory(result.QuadletsBE(), quadletCount);
    if (rootDir) {
        node.MutableROM().rootDirMinimal = std::move(*rootDir);
    } else {
        node.MutableROM().rootDirMinimal.clear();
    }

    std::vector<uint32_t> rootDirBE = std::move(result.quadletsBE);

    const ASFW::ConfigROM::QuadletOffset rootDirStart{
        ASFW::ConfigROM::RootDirStartQuadlet(node.ROM().bib)};
    StartDetailsDiscovery(nodeId, rootDirStart, std::move(rootDirBE));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) - callback-driven state machine.
void ROMScanSession::EnsurePrefix(uint8_t nodeId,
                                  ASFW::ConfigROM::QuadletCount requiredTotalQuadlets,
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

    if (requiredTotalQuadlets.value > ASFW::ConfigROM::kMaxROMPrefixQuadlets) {
        ASFW_LOG(ConfigROM,
                 "EnsurePrefix: node=%u required=%u exceeds max ROM prefix (%u quadlets), skipping",
                 nodeId, requiredTotalQuadlets.value, ASFW::ConfigROM::kMaxROMPrefixQuadlets);
        if (completion) {
            completion(false);
        }
        return;
    }

    const ASFW::ConfigROM::QuadletCount have{static_cast<uint32_t>(node.ROM().rawQuadlets.size())};
    if (have >= requiredTotalQuadlets) {
        if (completion) {
            completion(true);
        }
        return;
    }

    const ASFW::ConfigROM::QuadletCount toRead{requiredTotalQuadlets.value - have.value};
    const ASFW::ConfigROM::ByteOffset offsetBytes = ASFW::ConfigROM::ToBytes(have);

    ++inflight_;

    auto completionHolder = std::make_shared<std::function<void(bool)>>(std::move(completion));

    auto weakSelf = weak_from_this();
    reader_->ReadRootDirQuadlets(
        nodeId, gen_, node.CurrentSpeed(), offsetBytes.value, toRead.value,
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

                    const bool ok =
                        rawQuadlets.size() >= static_cast<size_t>(requiredTotalQuadlets.value);
                    if (!ok) {
                        ASFW_LOG_V2(ConfigROM,
                                    "EnsurePrefix short read: node=%u have=%zu required=%u", nodeId,
                                    rawQuadlets.size(), requiredTotalQuadlets.value);
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
