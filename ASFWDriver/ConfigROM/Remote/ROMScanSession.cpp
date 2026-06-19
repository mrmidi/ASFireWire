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

uint32_t SpeedMbps(FwSpeed speed) {
    return 100u << static_cast<uint8_t>(speed);
}

void LogBIBReadFailed(uint8_t nodeId) {
    ASFW_LOG(ConfigROM, "ROMScanSession: Node %u BIB read failed, retrying", nodeId);
}

void LogBIBBootingRetry(uint8_t nodeId) {
    ASFW_LOG(ConfigROM, "ROMScanSession: Node %u BIB quadlet[0]=0 (booting), retry", nodeId);
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

void LogConfigROMReadyRetry(uint8_t nodeId, const char* reason, uint8_t retriesLeft) {
    ASFW_LOG(ConfigROM,
             "ROMScanSession: Node %u Config ROM not ready (%s), delayed retry scheduled "
             "(remaining=%u)",
             nodeId, reason != nullptr ? reason : "unspecified", retriesLeft);
}

void LogMinimalROMSkipped(uint8_t nodeId) {
    ASFW_LOG(ConfigROM,
             "ROMScanSession: Node %u IEEE 1212 minimal Config ROM has no GUID/root directory; "
             "skipping normal device discovery",
             nodeId);
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
        session->rootCapabilityCallback_ = std::move(request.rootCapabilityCallback);
        session->completionNotified_ = false;
        session->hadBusyNodes_ = false;
        session->inflight_ = 0;
        session->completedROMs_.clear();
        session->nodeScans_.clear();
        session->rootProbeStarted_ = false;
        session->rootProbeTerminal_ = false;

        if (request.targetNodes.empty()) {
            for (const auto& node : session->topology_.physical.nodes) {
                if (node.physicalId == session->localNodeId_) {
                    continue;
                }
                if (!node.linkActive) {
                    continue;
                }
                session->nodeScans_.emplace_back(node.physicalId, session->gen_,
                                                 session->params_.startSpeed,
                                                 session->params_.perStepRetries);
                session->nodeScans_.back().SetConfigROMReadyRetriesLeft(
                    session->params_.configROMReadyRetries);
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
                session->nodeScans_.back().SetConfigROMReadyRetriesLeft(
                    session->params_.configROMReadyRetries);
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
        if (session->rootProbeStarted_ && !session->rootProbeTerminal_ &&
            session->topology_.rootNodeId != Driver::kInvalidPhysicalId) {
            session->NotifyRootBIBFailure(session->topology_.rootNodeId,
                                          Driver::Role::RootBibReadStatus::AbortedByReset);
        }
        session->completion_ = nullptr;
        session->rootCapabilityCallback_ = nullptr;
        session->completionNotified_ = true;
        session->nodeScans_.clear();
        session->completedROMs_.clear();
        session->inflight_ = 0;
        session->gen_ = Generation{0};
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

void ROMScanSession::DispatchDelayed(std::function<void()> work, uint64_t delayNs) {
    if (!work) {
        return;
    }

    if (!dispatchQueue_) {
#ifdef ASFW_HOST_TEST
        Post(std::move(work));
        return;
#else
        const uint64_t delayMs = delayNs / 1'000'000ULL;
        const uint64_t trailingNs = delayNs % 1'000'000ULL;
        Post([delayMs, trailingNs, work = std::move(work)]() mutable {
            if (delayMs > 0) {
                IOSleep(delayMs);
            }
            if (trailingNs > 0) {
                IODelay((trailingNs + 999ULL) / 1000ULL);
            }
            work();
        });
        return;
#endif
    }

#ifdef ASFW_HOST_TEST
    dispatchQueue_->DispatchAsyncAfter(delayNs, std::move(work));
#else
    const uint64_t delayMs = delayNs / 1'000'000ULL;
    const uint64_t trailingNs = delayNs % 1'000'000ULL;
    auto queue = dispatchQueue_;
    auto captured = std::make_shared<std::function<void()>>(std::move(work));
    queue->DispatchAsync(^{
      if (delayMs > 0) {
          IOSleep(delayMs);
      }
      if (trailingNs > 0) {
          IODelay((trailingNs + 999ULL) / 1000ULL);
      }
      (*captured)();
    });
#endif
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
    NotifyRootBIBPending(nodeId);

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
        ASFW_LOG_V2(ConfigROM, "ROMScanSession: Node %u retry at S%u (retries left=%u)",
                    node.NodeId(), SpeedMbps(node.CurrentSpeed()),
                    node.RetriesLeft());
        break;
    case RetryBackoffPolicy::Decision::RetryWithFallback: {
        const auto newSpeed = node.CurrentSpeed();
        (void)newSpeed;
        ASFW_LOG_V2(ConfigROM,
                    "ROMScanSession: Node %u speed fallback S%u -> S%u, retries reset",
                    node.NodeId(), SpeedMbps(oldSpeed), SpeedMbps(newSpeed));
        break;
    }
    case RetryBackoffPolicy::Decision::FailedExhausted:
        ASFW_LOG(ConfigROM, "ROMScanSession: Node %u -> Failed (exhausted retries)", node.NodeId());
        break;
    }
}

bool ROMScanSession::IsRootNode(uint8_t nodeId) const noexcept {
    return topology_.rootNodeId != Driver::kInvalidPhysicalId && topology_.rootNodeId == nodeId;
}

void ROMScanSession::NotifyRootBIBPending(uint8_t nodeId) {
    if (!IsRootNode(nodeId) || !rootCapabilityCallback_ || rootProbeStarted_) {
        return;
    }

    rootProbeStarted_ = true;
    Driver::Role::RootCapabilityEvidence evidence{};
    evidence.generation = gen_.value;
    evidence.rootNodeId = nodeId;
    evidence.bibReadStatus = Driver::Role::RootBibReadStatus::Pending;
    evidence.verdict = Driver::Role::RootCapability::Unknown;
    rootCapabilityCallback_(evidence);
}

void ROMScanSession::NotifyRootBIBSuccess(uint8_t nodeId, const BusInfoBlock& bib) {
    if (!IsRootNode(nodeId) || !rootCapabilityCallback_ || rootProbeTerminal_) {
        return;
    }

    rootProbeStarted_ = true;
    rootProbeTerminal_ = true;
    Driver::Role::RootCapabilityEvidence evidence{};
    evidence.generation = gen_.value;
    evidence.rootNodeId = nodeId;
    evidence.bibReadStatus = Driver::Role::RootBibReadStatus::Success;
    evidence.cmcKnown = true;
    evidence.cmc = bib.cmc;
    evidence.configRomHeaderValid = true;
    evidence.verdict = Driver::Role::DeriveRootCapabilityVerdict(
        evidence.bibReadStatus, evidence.cmcKnown, evidence.cmc,
        evidence.cycleObservationComplete, evidence.cycles);
    rootCapabilityCallback_(evidence);
}

void ROMScanSession::NotifyRootBIBFailure(uint8_t nodeId,
                                          Driver::Role::RootBibReadStatus status) {
    if (!IsRootNode(nodeId) || !rootCapabilityCallback_ || rootProbeTerminal_) {
        return;
    }

    rootProbeStarted_ = true;
    rootProbeTerminal_ = true;
    Driver::Role::RootCapabilityEvidence evidence{};
    evidence.generation = gen_.value;
    evidence.rootNodeId = nodeId;
    evidence.bibReadStatus = status;
    evidence.verdict = Driver::Role::RootCapability::Unknown;
    rootCapabilityCallback_(evidence);
}

Driver::Role::RootBibReadStatus ROMScanSession::MapBIBFailureStatus(
    Async::AsyncStatus status) noexcept {
    using Driver::Role::RootBibReadStatus;
    switch (status) {
    case Async::AsyncStatus::kTimeout:
    case Async::AsyncStatus::kBusyRetryExhausted:
        return RootBibReadStatus::Timeout;
    case Async::AsyncStatus::kAborted:
    case Async::AsyncStatus::kStaleGeneration:
        return RootBibReadStatus::AbortedByReset;
    case Async::AsyncStatus::kSuccess:
    case Async::AsyncStatus::kShortRead:
    case Async::AsyncStatus::kHardwareError:
    case Async::AsyncStatus::kLockCompareFail:
        return RootBibReadStatus::Failed;
    }
    return RootBibReadStatus::Failed;
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
        if (ShouldDelayConfigROMReadyRetry(node)) {
            ScheduleConfigROMReadyRetry(node, "BIB read failed");
            return;
        }
        RetryWithFallback(node);
        if (node.CurrentState() == ROMScanNodeStateMachine::State::Failed) {
            NotifyRootBIBFailure(nodeId, MapBIBFailureStatus(result.status));
        }
        Pump();
        return;
    }

    // IEEE 1212 allows temporary zero in header while device is still booting.
    if (!result.quadletsBE.empty() && result.quadletsBE[0] == 0) {
        LogBIBBootingRetry(nodeId);
        hadBusyNodes_ = true;
        if (ShouldDelayConfigROMReadyRetry(node)) {
            ScheduleConfigROMReadyRetry(node, "BIB q0 booting");
            return;
        }
        RetryWithFallback(node);
        if (node.CurrentState() == ROMScanNodeStateMachine::State::Failed) {
            NotifyRootBIBFailure(nodeId, Driver::Role::RootBibReadStatus::Timeout);
        }
        Pump();
        return;
    }

    auto bibRes = ConfigROMParser::ParseBIB(result.QuadletsBE());
    if (!bibRes) {
        LogBIBParseFailed(nodeId, bibRes.error());
        (void)TransitionNodeState(node, ROMScanNodeStateMachine::State::Failed, "BIB parse failed");
        NotifyRootBIBFailure(nodeId, Driver::Role::RootBibReadStatus::Failed);
        Pump();
        return;
    }

    if (bibRes->crcStatus == ConfigROMParser::CRCStatus::Mismatch) {
        LogBIBCRCMismatch(nodeId, bibRes->computed.value_or(0), bibRes->bib.crc);
    }

    node.MutableROM().rawQuadlets.clear();
    node.MutableROM().rawQuadlets.reserve(std::max<size_t>(256U, result.quadletsBE.size()));
    node.MutableROM().rawQuadlets.insert(node.MutableROM().rawQuadlets.end(),
                                         result.quadletsBE.begin(), result.quadletsBE.end());

    node.MutableROM().bib = bibRes->bib;

    speedPolicy_.RecordSuccess(nodeId, node.CurrentSpeed());

    if (bibRes->bib.format == ConfigROMFormat::Minimal1212) {
        NotifyRootBIBFailure(nodeId, Driver::Role::RootBibReadStatus::Failed);
        CompleteUnsupportedMinimalROM(node);
        return;
    }

    NotifyRootBIBSuccess(nodeId, bibRes->bib);

    ContinueAfterBIBSuccess(node, nodeId);
}

void ROMScanSession::ContinueAfterBIBSuccess(ROMScanNodeStateMachine& node, uint8_t nodeId) {
    if (params_.doIRMCheck && topology_.irmNodeId != Driver::kInvalidPhysicalId && topology_.irmNodeId == nodeId &&
        node.ROM().bib.irmc) {
        StartIRMRead(node);
        return;
    }

    // Cross-validated with Linux: firewire/core-device.c:650-652 and
    // Apple: IOFireWireFamily.kmodproj/IOFireWireDevice.cpp:917.
    // General IEEE 1394 ROMs always continue to the root directory; crc_length
    // is CRC coverage and must not be used as a total-ROM/minimal-ROM test.
    StartRootDirRead(node);
}

bool ROMScanSession::ShouldDelayConfigROMReadyRetry(
    const ROMScanNodeStateMachine& node) const {
    return params_.configROMReadyRetryDelayNs > 0 && node.ConfigROMReadyRetriesLeft() > 0;
}

void ROMScanSession::ScheduleConfigROMReadyRetry(ROMScanNodeStateMachine& node,
                                                 const char* reason) {
    if (!TransitionNodeState(node, ROMScanNodeStateMachine::State::WaitingConfigROMReady,
                             "Config ROM readiness wait")) {
        Pump();
        return;
    }

    hadBusyNodes_ = true;
    node.DecrementConfigROMReadyRetries();
    LogConfigROMReadyRetry(node.NodeId(), reason, node.ConfigROMReadyRetriesLeft());

    const uint8_t nodeId = node.NodeId();
    auto weakSelf = weak_from_this();
    // Cross-validated with Linux: firewire/core-device.c:849-852,1018-1024 and
    // Apple: IOFireWireFamily.kmodproj/IOFireWireController.cpp:2703-2716.
    // Linux reschedules failed Config ROM scans; Apple treats an initial BIB
    // timeout after ACK-pending as a device-not-ready condition.
    DispatchDelayed(
        [weakSelf, nodeId]() {
            if (auto self = weakSelf.lock(); self) {
                self->DispatchAsync([self = std::move(self), nodeId]() {
                    if (self->aborted_.load(std::memory_order_relaxed)) {
                        return;
                    }

                    auto* nodePtr = self->FindNode(nodeId);
                    if (nodePtr == nullptr) {
                        return;
                    }

                    auto& delayedNode = *nodePtr;
                    if (delayedNode.CurrentState() !=
                        ROMScanNodeStateMachine::State::WaitingConfigROMReady) {
                        return;
                    }

                    if (!TransitionNodeState(delayedNode, ROMScanNodeStateMachine::State::Idle,
                                             "Config ROM readiness retry")) {
                        self->Pump();
                        return;
                    }
                    self->Pump();
                });
            }
        },
        params_.configROMReadyRetryDelayNs);
}

void ROMScanSession::CompleteUnsupportedMinimalROM(ROMScanNodeStateMachine& node) {
    LogMinimalROMSkipped(node.NodeId());
    if (!TransitionNodeState(node, ROMScanNodeStateMachine::State::Complete,
                             "IEEE 1212 minimal ROM skipped")) {
        Pump();
        return;
    }

    Pump();
}

void ROMScanSession::StartRootDirRead(ROMScanNodeStateMachine& node) {
    const uint8_t nodeId = node.NodeId();
    const uint32_t offsetBytes = ASFW::ConfigROM::RootDirStartBytes(node.ROM().bib);

    if (!TransitionNodeState(node, ROMScanNodeStateMachine::State::ReadingRootDir,
                             "BIB complete enter root dir read")) {
        Pump();
        return;
    }

    node.SetRetriesLeft(params_.perStepRetries);
    ++inflight_;

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
        hadBusyNodes_ = true;
        if (ShouldDelayConfigROMReadyRetry(node)) {
            ScheduleConfigROMReadyRetry(node, "root directory read failed");
            return;
        }

        ASFW_LOG(ConfigROM, "ROMScanSession: Node %u RootDir read failed, retrying scan step",
                 nodeId);
        RetryWithFallback(node);
        Pump();
        return;
    }

    const uint32_t quadletCount = static_cast<uint32_t>(result.quadletsBE.size());
    ASFW_LOG_V2(ConfigROM, "ROMScanSession: Node %u root directory read returned %u quadlets",
                nodeId, quadletCount);
    auto rootDir = ConfigROMParser::ParseRootDirectory(result.QuadletsBE(), quadletCount);
    if (rootDir) {
        node.MutableROM().rootDirMinimal = std::move(*rootDir);
        if (node.MutableROM().rootDirMinimal.empty() && quadletCount > 1) {
            ASFW_LOG_V1(ConfigROM,
                        "ROMScanSession: Node %u root directory contained no recognized entries",
                        nodeId);
        }
    } else {
        const auto error = rootDir.error();
        ASFW_LOG(ConfigROM,
                 "ROMScanSession: Node %u root directory parse failed code=%u offset=q%u "
                 "quadlets=%u status=%{public}s",
                 nodeId, static_cast<uint8_t>(error.code), error.offsetQuadlets, quadletCount,
                 Async::ToString(result.status));
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
    if (gen_ == Generation{0}) {
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
    gen_ = Generation{0};
    hadBusyNodes_ = false;

    if (completion) {
        completion(gen, std::move(roms), hadBusyNodes);
    }
}

} // namespace ASFW::Discovery
