// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DiceLockSnapshot.hpp"
#include "DiceReceiverDumpSnapshot.hpp"
#include "DiceReceiverRegisters.hpp"
#include "../../Protocols/DICE/Core/DICETypes.hpp"
#include "../../../Protocols/Ports/ProtocolRegisterIO.hpp"
#include "../../../Scheduling/ITimerScheduler.hpp"
#include "../../../Logging/Logging.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <utility>

namespace ASFW::Audio::DICE::Diagnostics {

// TCAT firmware-loader mailbox, relative to 0xFFFFE0000000 (FFADO
// dice_firmware_loader.h:34-63, the PEEK exchange at dice_firmware_loader.cpp:300-330).
namespace Loader {
inline constexpr uint32_t kBase = 0x00100000;
inline constexpr uint32_t kOpcode = kBase + 0x04;
inline constexpr uint32_t kReturnStatus = kBase + 0x08;
inline constexpr uint32_t kParameter = kBase + 0x2C;
inline constexpr uint32_t kExecute = 0x80000000U;
inline constexpr uint32_t kOpcodeMask = 0xFFFU;
inline constexpr uint32_t kOpTestAction = 0x6;
// TEST_ACTION sub-command 2 reads one quadlet. Sub-command 1 writes RAM; this
// file has no way to send it.
inline constexpr uint32_t kTestActionPeek = 2;
inline constexpr uint64_t kPollIntervalNs = 20'000'000;
inline constexpr uint64_t kPeekTimeoutNs = 2'000'000'000;
}

// The TCAT extension's general-caps word carries the ASIC type in bits 31:16
// (snd-firewire-ctl-services caps_section.rs:246-264, 299-301).
namespace ExtensionCaps {
inline constexpr uint32_t kGeneralOffset = 0x08;
inline constexpr uint32_t kMinimumBytes = 0x0C;
inline constexpr uint32_t kAsicTcd2210 = 1;
inline constexpr uint32_t kAsicTcd2220 = 2;
}

// Fixed TCD22xx allowlist. A dump reads exactly these addresses, in order.
struct DiceDumpPlan {
    std::array<uint32_t, DiceReceiverDumpSnapshot::kMaxEntries> addresses{};
    std::array<const char*, DiceReceiverDumpSnapshot::kMaxEntries> names{};
    uint32_t count{0};
    void Add(uint32_t address, const char* name) {
        if (count < addresses.size()) { addresses[count] = address; names[count] = name; ++count; }
    }
};

[[nodiscard]] inline DiceDumpPlan BuildTcd22xxDumpPlan(bool fifoEvents) {
    using namespace Registers::Tcd22xx;
    static constexpr const char* kArx[2][5] = {
        {"ARX1_CFG0", "ARX1_CFG1", "ARX1_CIP0", "ARX1_CIP1", "ARX1_ADO_CFG"},
        {"ARX2_CFG0", "ARX2_CFG1", "ARX2_CIP0", "ARX2_CIP1", "ARX2_ADO_CFG"},
    };
    static constexpr const char* kPartition[4][2] = {
        {"FIFO0_BASE", "FIFO0_LIMIT"}, {"FIFO1_BASE", "FIFO1_LIMIT"},
        {"FIFO2_BASE", "FIFO2_LIMIT"}, {"FIFO3_BASE", "FIFO3_LIMIT"},
    };
    DiceDumpPlan plan;
    plan.Add(kReceiverEvents, "RX_EVENTS");
    for (uint32_t r = 0; r < kReceiverBases.size(); ++r) {
        for (uint32_t i = 0; i < kReceiverRegisters.size(); ++i) {
            plan.Add(kReceiverBases[r] + kReceiverRegisters[i].offset, kArx[r][i]);
        }
    }
    for (uint32_t p = 0; p < kPartitionCount; ++p) {
        const uint32_t base = kPartitionBase0 + p * kPartitionStride;
        plan.Add(base, kPartition[p][0]);
        plan.Add(base + kPartitionLimitOffset, kPartition[p][1]);
    }
    plan.Add(kTransmitterEvents, "TX_EVENTS");
    if (fifoEvents) plan.Add(kFifoEventsReadToClear, "FIFO_EVENTS");
    return plan;
}

// Reads TCD22xx receiver registers through the firmware loader's PEEK. Owned
// by DiceFamilyDriver; completions and timers retain only a session, never
// the owner. Every refusal is decided before the first mailbox write.
class DiceReceiverDump final {
    struct Guard {
        IOLock* lock;
        explicit Guard(IOLock* value) : lock(value) { IOLockLock(lock); }
        ~Guard() { IOLockUnlock(lock); }
    };

    struct Session final : std::enable_shared_from_this<Session> {
        enum class Step { Sections, Caps, Probe, WriteParams, VerifyParams, Execute, Poll, Status, Value };

        Protocols::Ports::ProtocolRegisterIO io;
        // AudioRuntimeRegistry's scheduler: it outlives every protocol.
        Scheduling::ITimerScheduler* scheduler;
        IOLock* const lock{IOLockAlloc()};
        DiceReceiverDumpSnapshot snapshot{};
        DiceDumpPlan plan{};
        uint32_t index{0};
        uint32_t capsGeneral{0};
        std::array<uint8_t, 8> params{};
        uint64_t pollDeadlineNs{0};
        uint32_t seq{0};
        Async::AsyncHandle handle{};
        Scheduling::TimerToken timer{};

        Session(Protocols::Ports::ProtocolRegisterIO source, Scheduling::ITimerScheduler* timerScheduler)
            : io(source), scheduler(timerScheduler) {}
        ~Session() { if (lock) IOLockFree(lock); }

        [[nodiscard]] DiceReceiverDumpSnapshot Copy() const {
            if (!lock) return snapshot;
            Guard guard(lock);
            return snapshot;
        }
        [[nodiscard]] bool Running() const {
            Guard guard(lock);
            return snapshot.phase == DiceDumpPhase::Running;
        }

        static const char* RefusalName(DiceDumpRefusal refusal) {
            switch (refusal) {
                case DiceDumpRefusal::NotVerifiedFirmware: return "no verified PEEK firmware";
                case DiceDumpRefusal::NoExtension: return "no TCAT extension (chip not identifiable)";
                case DiceDumpRefusal::AsicNotTcd22xx: return "ASIC is not TCD2210/TCD2220";
                case DiceDumpRefusal::LoaderAbsent: return "no firmware-loader mailbox";
                case DiceDumpRefusal::LoaderBusy: return "firmware-loader mailbox busy";
                case DiceDumpRefusal::NoScheduler: return "no timer scheduler";
                case DiceDumpRefusal::None: break;
            }
            return "none";
        }
        static const char* StageName(DiceDumpStage stage) {
            constexpr const char* kNames[] = {"none", "sections", "caps", "probe", "write-params",
                                              "verify-params", "execute", "poll", "status", "value"};
            const auto i = static_cast<uint32_t>(stage);
            return i < std::size(kNames) ? kNames[i] : "?";
        }

        // lock held
        void RefuseLocked(DiceDumpRefusal refusal, IOReturn error = kIOReturnSuccess) {
            snapshot.phase = DiceDumpPhase::Refused;
            snapshot.refusal = refusal;
            snapshot.lastError = static_cast<uint32_t>(error);
            ASFW_LOG(DICE, "[DiceDump] guid=%016llx gen=%u refused: %{public}s kr=0x%x asic=0x%x",
                     snapshot.guid, snapshot.generation, RefusalName(refusal), error, snapshot.asicType);
        }
        void FailLocked(DiceDumpStage stage, IOReturn error) {
            snapshot.phase = DiceDumpPhase::Failed;
            snapshot.failedStage = stage;
            snapshot.lastError = static_cast<uint32_t>(error);
            ASFW_LOG(DICE, "[DiceDump] guid=%016llx gen=%u failed at %{public}s kr=0x%x entry=%u addr=0x%08x read=%u/%u",
                     snapshot.guid, snapshot.generation, StageName(stage), error, index,
                     index < plan.count ? plan.addresses[index] : 0U, index, plan.count);
        }
        void CompleteLocked() {
            snapshot.phase = DiceDumpPhase::Complete;
            for (uint32_t i = 0; i < plan.count; ++i) {
                ASFW_LOG(DICE, "[DiceDump] guid=%016llx %{public}s @0x%08x = 0x%08x",
                         snapshot.guid, plan.names[i], plan.addresses[i], snapshot.entries[i].value);
            }
            const auto ado = [this](uint32_t name) {
                for (uint32_t i = 0; i < plan.count; ++i)
                    if (plan.addresses[i] == name) return snapshot.entries[i].value;
                return 0U;
            };
            using namespace Registers::Tcd22xx;
            ASFW_LOG(DICE, "[DiceDump] guid=%016llx gen=%u complete trigger=%u entries=%u ARX1 locked=%u ARX2 locked=%u",
                     snapshot.guid, snapshot.generation, static_cast<uint32_t>(snapshot.trigger), plan.count,
                     (ado(kReceiverBases[0] + 0x28) & kReceiverLocked) ? 1U : 0U,
                     (ado(kReceiverBases[1] + 0x28) & kReceiverLocked) ? 1U : 0U);
        }

        // Claims the next sequence for `step`; false once the session stopped.
        bool ClaimLocked(uint32_t& mySeq) {
            if (snapshot.phase != DiceDumpPhase::Running) return false;
            mySeq = ++seq;
            handle = {};
            return true;
        }
        // Entry guard for every completion.
        bool AcceptLocked(uint32_t mySeq) {
            if (snapshot.phase != DiceDumpPhase::Running || mySeq != seq) return false;
            handle = {};
            return true;
        }
        void Retain(uint32_t mySeq, Async::AsyncHandle issued) {
            Guard guard(lock);
            // A fake bus completes inline; never retain a retired handle.
            if (snapshot.phase == DiceDumpPhase::Running && mySeq == seq) handle = issued;
        }

        void Run(Step step) {
            uint32_t mySeq = 0;
            uint32_t address = 0;
            {
                Guard guard(lock);
                if (!ClaimLocked(mySeq)) return;
                if (step == Step::WriteParams) {
                    address = plan.addresses[index];
                    FW::WriteBE32(params.data(), Loader::kTestActionPeek);
                    FW::WriteBE32(params.data() + 4, address);
                }
            }
            const auto self = shared_from_this();
            Async::AsyncHandle issued{};
            switch (step) {
                case Step::Sections:
                    issued = io.ReadBlock(MakeDICEAddress(kDICEExtensionOffset),
                        static_cast<uint32_t>(ExtensionSections::kWireSize),
                        [self, mySeq](Async::AsyncStatus status, std::span<const uint8_t> payload) {
                            self->OnSections(mySeq, status, payload);
                        });
                    break;
                case Step::Caps:
                    issued = io.ReadQuadBE(MakeDICEAddress(capsGeneral),
                        [self, mySeq](Async::AsyncStatus status, uint32_t value) { self->OnCaps(mySeq, status, value); });
                    break;
                case Step::Probe:
                    issued = io.ReadQuadBE(MakeDICEAddress(Loader::kOpcode),
                        [self, mySeq](Async::AsyncStatus status, uint32_t value) { self->OnProbe(mySeq, status, value); });
                    break;
                case Step::WriteParams: {
                    std::array<uint8_t, 8> bytes{};
                    {
                        Guard guard(lock);
                        bytes = params;
                    }
                    issued = io.WriteBlock(MakeDICEAddress(Loader::kParameter), std::span<const uint8_t>(bytes),
                        [self, mySeq](Async::AsyncStatus status) {
                            self->OnWritten(mySeq, status, DiceDumpStage::WriteParams, Step::VerifyParams);
                        });
                    break;
                }
                case Step::VerifyParams:
                    issued = io.ReadBlock(MakeDICEAddress(Loader::kParameter), 8,
                        [self, mySeq](Async::AsyncStatus status, std::span<const uint8_t> payload) {
                            self->OnVerify(mySeq, status, payload);
                        });
                    break;
                case Step::Execute:
                    issued = io.WriteQuadBE(MakeDICEAddress(Loader::kOpcode), Loader::kExecute | Loader::kOpTestAction,
                        [self, mySeq](Async::AsyncStatus status) {
                            self->OnWritten(mySeq, status, DiceDumpStage::Execute, Step::Poll);
                        });
                    break;
                case Step::Poll:
                    issued = io.ReadQuadBE(MakeDICEAddress(Loader::kOpcode),
                        [self, mySeq](Async::AsyncStatus status, uint32_t value) { self->OnPoll(mySeq, status, value); });
                    break;
                case Step::Status:
                    issued = io.ReadQuadBE(MakeDICEAddress(Loader::kReturnStatus),
                        [self, mySeq](Async::AsyncStatus status, uint32_t value) { self->OnStatus(mySeq, status, value); });
                    break;
                case Step::Value:
                    issued = io.ReadQuadBE(MakeDICEAddress(Loader::kParameter),
                        [self, mySeq](Async::AsyncStatus status, uint32_t value) { self->OnValue(mySeq, status, value); });
                    break;
            }
            Retain(mySeq, issued);
        }

        void OnSections(uint32_t mySeq, Async::AsyncStatus status, std::span<const uint8_t> payload) {
            {
                Guard guard(lock);
                if (!AcceptLocked(mySeq)) return;
                if (status != Async::AsyncStatus::kSuccess || payload.size() < ExtensionSections::kWireSize) {
                    RefuseLocked(DiceDumpRefusal::NoExtension, Protocols::Ports::MapAsyncStatusToIOReturn(status));
                    return;
                }
                const auto sections = ExtensionSections::Deserialize(payload.data());
                if (sections.caps.size < ExtensionCaps::kMinimumBytes) {
                    RefuseLocked(DiceDumpRefusal::NoExtension);
                    return;
                }
                capsGeneral = ExtensionAbsoluteOffset(sections.caps, ExtensionCaps::kGeneralOffset);
            }
            Run(Step::Caps);
        }
        void OnCaps(uint32_t mySeq, Async::AsyncStatus status, uint32_t value) {
            {
                Guard guard(lock);
                if (!AcceptLocked(mySeq)) return;
                if (status != Async::AsyncStatus::kSuccess) {
                    RefuseLocked(DiceDumpRefusal::NoExtension, Protocols::Ports::MapAsyncStatusToIOReturn(status));
                    return;
                }
                snapshot.asicType = (value >> 16) & 0xFFFFU;
                if (snapshot.asicType != ExtensionCaps::kAsicTcd2210 &&
                    snapshot.asicType != ExtensionCaps::kAsicTcd2220) {
                    RefuseLocked(DiceDumpRefusal::AsicNotTcd22xx);
                    return;
                }
            }
            Run(Step::Probe);
        }
        void OnProbe(uint32_t mySeq, Async::AsyncStatus status, uint32_t value) {
            {
                Guard guard(lock);
                if (!AcceptLocked(mySeq)) return;
                const bool first = index == 0;
                if (status != Async::AsyncStatus::kSuccess) {
                    const IOReturn error = Protocols::Ports::MapAsyncStatusToIOReturn(status);
                    if (first) RefuseLocked(DiceDumpRefusal::LoaderAbsent, error);
                    else FailLocked(DiceDumpStage::Probe, error);
                    return;
                }
                if (value & Loader::kExecute) {
                    if (first) RefuseLocked(DiceDumpRefusal::LoaderBusy);
                    else FailLocked(DiceDumpStage::Probe, kIOReturnBusy);
                    return;
                }
            }
            Run(Step::WriteParams);
        }
        void OnWritten(uint32_t mySeq, Async::AsyncStatus status, DiceDumpStage stage, Step next) {
            {
                Guard guard(lock);
                if (!AcceptLocked(mySeq)) return;
                if (status != Async::AsyncStatus::kSuccess) {
                    FailLocked(stage, Protocols::Ports::MapAsyncStatusToIOReturn(status));
                    return;
                }
                if (next == Step::Poll && scheduler) pollDeadlineNs = scheduler->NowNs() + Loader::kPeekTimeoutNs;
            }
            Run(next);
        }
        void OnVerify(uint32_t mySeq, Async::AsyncStatus status, std::span<const uint8_t> payload) {
            {
                Guard guard(lock);
                if (!AcceptLocked(mySeq)) return;
                if (status != Async::AsyncStatus::kSuccess) {
                    FailLocked(DiceDumpStage::VerifyParams, Protocols::Ports::MapAsyncStatusToIOReturn(status));
                    return;
                }
                // Another master changed the parameters: never execute them.
                if (payload.size() < params.size() ||
                    !std::equal(params.begin(), params.end(), payload.begin())) {
                    FailLocked(DiceDumpStage::VerifyParams, kIOReturnIOError);
                    return;
                }
            }
            Run(Step::Execute);
        }
        void OnPoll(uint32_t mySeq, Async::AsyncStatus status, uint32_t value) {
            bool wait = false;
            {
                Guard guard(lock);
                if (!AcceptLocked(mySeq)) return;
                if (status != Async::AsyncStatus::kSuccess) {
                    FailLocked(DiceDumpStage::Poll, Protocols::Ports::MapAsyncStatusToIOReturn(status));
                    return;
                }
                if (value & Loader::kExecute) {
                    // Still executing. On timeout the mailbox is left alone.
                    if (scheduler->NowNs() >= pollDeadlineNs) {
                        FailLocked(DiceDumpStage::Poll, kIOReturnTimeout);
                        return;
                    }
                    wait = true;
                } else if ((value & Loader::kOpcodeMask) != Loader::kOpTestAction) {
                    FailLocked(DiceDumpStage::Poll, kIOReturnNotPermitted); // another master's command
                    return;
                }
            }
            if (!wait) {
                Run(Step::Status);
                return;
            }
            const std::weak_ptr<Session> weak = weak_from_this();
            const auto token = scheduler->ScheduleAfter(Loader::kPollIntervalNs, [weak] {
                if (auto self = weak.lock()) self->Run(Step::Poll);
            });
            Guard guard(lock);
            if (!token) {
                if (snapshot.phase == DiceDumpPhase::Running) FailLocked(DiceDumpStage::Poll, kIOReturnNoResources);
                return;
            }
            if (snapshot.phase == DiceDumpPhase::Running) timer = token;
            else scheduler->Cancel(token);
        }
        void OnStatus(uint32_t mySeq, Async::AsyncStatus status, uint32_t value) {
            {
                Guard guard(lock);
                if (!AcceptLocked(mySeq)) return;
                if (status != Async::AsyncStatus::kSuccess) {
                    FailLocked(DiceDumpStage::Status, Protocols::Ports::MapAsyncStatusToIOReturn(status));
                    return;
                }
                if (value != 0) {
                    FailLocked(DiceDumpStage::Status, static_cast<IOReturn>(value)); // firmware rejected PEEK
                    return;
                }
            }
            Run(Step::Value);
        }
        void OnValue(uint32_t mySeq, Async::AsyncStatus status, uint32_t value) {
            bool more = false;
            {
                Guard guard(lock);
                if (!AcceptLocked(mySeq)) return;
                if (status != Async::AsyncStatus::kSuccess) {
                    FailLocked(DiceDumpStage::Value, Protocols::Ports::MapAsyncStatusToIOReturn(status));
                    return;
                }
                snapshot.entries[index].value = value;
                snapshot.validMask |= 1U << index;
                ++index;
                more = index < plan.count;
                if (!more) CompleteLocked();
            }
            if (more) Run(Step::Probe);
        }

        void Begin(bool verified) {
            {
                Guard guard(lock);
                if (!verified) { RefuseLocked(DiceDumpRefusal::NotVerifiedFirmware); return; }
                if (!scheduler) { RefuseLocked(DiceDumpRefusal::NoScheduler); return; }
                snapshot.phase = DiceDumpPhase::Running;
                ASFW_LOG(DICE, "[DiceDump] guid=%016llx gen=%u start trigger=%u entries=%u fifoEvents=%u",
                         snapshot.guid, snapshot.generation, static_cast<uint32_t>(snapshot.trigger),
                         plan.count, snapshot.flags & 1U);
            }
            Run(Step::Sections);
        }
        void Cancel() {
            Async::AsyncHandle pending{};
            Scheduling::TimerToken token{};
            {
                Guard guard(lock);
                if (snapshot.phase != DiceDumpPhase::Running) return;
                snapshot.phase = DiceDumpPhase::Cancelled;
                ++seq;
                pending = std::exchange(handle, {});
                token = std::exchange(timer, {});
                ASFW_LOG(DICE, "[DiceDump] guid=%016llx gen=%u cancelled at entry %u/%u",
                         snapshot.guid, snapshot.generation, index, plan.count);
            }
            if (token && scheduler) scheduler->Cancel(token);
            if (pending) (void)io.CancelTransaction(pending);
        }
    };

    struct Shared {
        IOLock* const lock{IOLockAlloc()};
        std::shared_ptr<Session> session;
        bool closed{false};
        bool verified{false};
        ~Shared() { if (lock) IOLockFree(lock); }
    };

    static IOReturn StartOn(const std::shared_ptr<Shared>& shared, const Protocols::Ports::ProtocolRegisterIO& io,
                            Scheduling::ITimerScheduler* scheduler, DiceDumpTrigger trigger, bool fifoEvents) {
        if (!shared || !shared->lock) return kIOReturnNoMemory;
        std::shared_ptr<Session> next;
        bool verified = false;
        {
            Guard guard(shared->lock);
            if (shared->closed) return kIOReturnNotReady;
            if (shared->session && shared->session->Running()) return kIOReturnBusy;
            next = std::make_shared<Session>(io, scheduler);
            if (!next->lock) return kIOReturnNoMemory;
            next->snapshot.guid = io.Route().guid;
            next->snapshot.generation = io.Generation().value;
            next->snapshot.trigger = trigger;
            next->snapshot.flags = fifoEvents ? 1U : 0U;
            next->plan = BuildTcd22xxDumpPlan(fifoEvents);
            next->snapshot.count = next->plan.count;
            for (uint32_t i = 0; i < next->plan.count; ++i)
                next->snapshot.entries[i].address = next->plan.addresses[i];
            shared->session = next;
            verified = shared->verified;
        }
        next->Begin(verified);
        return kIOReturnSuccess;
    }

    std::shared_ptr<Shared> shared_{std::make_shared<Shared>()};

public:
    // `verifiedLoaderFirmware`: the protocol's firmware TEST_ACTION table has
    // been checked (today: Pro24 DSP). Without it no request reaches the bus.
    explicit DiceReceiverDump(bool verifiedLoaderFirmware = false) {
        if (shared_) shared_->verified = verifiedLoaderFirmware;
    }
    DiceReceiverDump(const DiceReceiverDump&) = delete;
    DiceReceiverDump& operator=(const DiceReceiverDump&) = delete;
    ~DiceReceiverDump() { Close(); }

    // Manual request (user client). Refusals are recorded in the snapshot.
    IOReturn Request(const Protocols::Ports::ProtocolRegisterIO& io, Scheduling::ITimerScheduler* scheduler,
                     bool fifoEvents) {
        return StartOn(shared_, io, scheduler, DiceDumpTrigger::Request, fifoEvents);
    }

    // For DiceLockWatch: one dump after every lock verdict, locked or not. A
    // silent device can report every receiver locked, so the registers are
    // wanted on a good verdict too. A device without verified firmware logs
    // its refusal once per start, so the absence of a dump is never silent.
    [[nodiscard]] std::function<void(const DiceLockSnapshot&)> VerdictTrigger(
        const Protocols::Ports::ProtocolRegisterIO& io, Scheduling::ITimerScheduler* scheduler) const {
        std::weak_ptr<Shared> weak = shared_;
        return [weak, io, scheduler](const DiceLockSnapshot&) {
            auto shared = weak.lock();
            if (!shared || !shared->lock) return;
            (void)StartOn(shared, io, scheduler, DiceDumpTrigger::LockVerdict, false);
        };
    }

    void Cancel() {
        if (!shared_ || !shared_->lock) return;
        std::shared_ptr<Session> current;
        {
            Guard guard(shared_->lock);
            current = shared_->session;
        }
        if (current) current->Cancel();
    }
    void Close() {
        if (!shared_ || !shared_->lock) return;
        {
            Guard guard(shared_->lock);
            shared_->closed = true;
        }
        Cancel();
    }
    [[nodiscard]] DiceReceiverDumpSnapshot Copy() const {
        if (!shared_ || !shared_->lock) return {};
        std::shared_ptr<Session> current;
        {
            Guard guard(shared_->lock);
            current = shared_->session;
        }
        return current ? current->Copy() : DiceReceiverDumpSnapshot{};
    }
};

} // namespace ASFW::Audio::DICE::Diagnostics
