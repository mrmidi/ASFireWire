#pragma once

// CommandExecutor — SBP-2 command plane for one logged-in session.
//
// Decomposed from PR #19's SBP2SessionRegistry: the
// command god-object that bloated SBP2SessionRecord (command ORB, page table,
// management ORB, in-flight tracking, pending result) is lifted out into this
// type, one instance per SessionRecord. It drives the session's FetchAgent by
// submitting Normal Command ORBs through LoginSession::SubmitORB and matches
// completions back via the ORB completion callback.
//
// Lifetime: owned by the SessionRecord (created once the LoginSession exists). It
// holds a reference to its LoginSession and writes the session's lastError slot.
// Async completions capture a weak lifetime token and bail if the executor has
// been destroyed (session release / registry teardown).

#include "LoginSession.hpp"
#include "../SBP2CommandORB.hpp"
#include "../SBP2ManagementORB.hpp"
#include "../SBP2PageTable.hpp"
#include "../SCSICommandSet.hpp"
#include "../AddressSpaceManager.hpp"
#include "../../../Async/AsyncTypes.hpp"

#include <DriverKit/IOLib.h>
#ifdef ASFW_HOST_TEST
#include "../../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#endif

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace ASFW::Async {
class IFireWireBus;
class IFireWireBusInfo;
}

namespace ASFW::Protocols::SBP2 {

// Where a command's ORB, page table and data buffer are in their life. The
// target moves the data itself (OHCI physical DMA) and may still be doing so
// until a status block or a completed task-management function says it has
// stopped (SBP-2 §10.4), so resources leave Active/Recovering only through
// those edges, a bus reset, or by being parked on the session's logout.
enum class CommandPhase : uint8_t {
    Idle,        // no command resources held
    Active,      // ORB submitted, status pending
    Recovering,  // transport failure; LUN reset in flight, resources retained
};

// A command's resources after its session was released while the target still
// owned them. Freed when dropped: after the logout status (the target has
// aborted the task set, §8.4), at bus reset, or with the registry.
class ParkedCommand {
public:
    ParkedCommand(AddressSpaceManager& addrSpaceMgr,
                  void* owner,
                  uint64_t bufferHandle,
                  std::unique_ptr<SBP2CommandORB> orb,
                  std::unique_ptr<SBP2PageTable> pageTable) noexcept;
    ParkedCommand(ParkedCommand&& other) noexcept;
    ParkedCommand& operator=(ParkedCommand&& other) noexcept;
    ParkedCommand(const ParkedCommand&) = delete;
    ParkedCommand& operator=(const ParkedCommand&) = delete;
    ~ParkedCommand();

private:
    void Release() noexcept;

    AddressSpaceManager* addrSpaceMgr_;
    void* owner_;
    uint64_t bufferHandle_;
    std::unique_ptr<SBP2CommandORB> orb_;
    std::unique_ptr<SBP2PageTable> pageTable_;
};

class CommandExecutor {
public:
    CommandExecutor(Async::IFireWireBus& bus,
                    Async::IFireWireBusInfo& busInfo,
                    AddressSpaceManager& addrSpaceMgr,
                    LoginSession& session,
                    ISessionScheduler& scheduler,
                    void* owner,
                    int32_t& lastError) noexcept;
    ~CommandExecutor();

    CommandExecutor(const CommandExecutor&) = delete;
    CommandExecutor& operator=(const CommandExecutor&) = delete;

    // Push-style completion: fired exactly once per accepted SubmitCommand with a
    // callback — on ORB completion, on abort (bus reset / task management), or on
    // release-time Cleanup. It consumes the result (the poll-style Get*Result
    // readers see nothing). Runs on the Default queue without the registry lock,
    // except the Cleanup/teardown path, which may hold it — callbacks must not
    // re-enter the registry synchronously.
    using ResultCallback = std::function<void(const SCSI::CommandResult&)>;

    // Submit a generic SCSI command. Returns false if not logged in, another
    // command is active, the request is malformed, or the ORB cannot be built.
    // When rejected, `callback` is dropped without being invoked.
    [[nodiscard]] bool SubmitCommand(const SCSI::CommandRequest& request,
                                     ResultCallback callback = {});

    // Submit a SCSI INQUIRY (convenience wrapper over SubmitCommand).
    [[nodiscard]] bool SubmitInquiry(uint8_t allocationLength);

    // Destructive reads of the pending result. GetInquiryResult only returns a
    // result whose completed opcode was INQUIRY.
    [[nodiscard]] std::optional<SCSI::CommandResult> GetCommandResult();
    [[nodiscard]] std::optional<SCSI::CommandResult> GetInquiryResult();

    // Submit a task-management recovery ORB (abort task set / LU reset / target
    // reset). Returns false if not logged in, one is already in flight, or the
    // function is unsupported. onComplete (optional) fires when the management
    // ORB completes, regardless of its outcome.
    [[nodiscard]] bool SubmitTaskManagement(SBP2ManagementORB::Function function,
                                            std::function<void()> onComplete = {});

    // Bus reset: fail the active command (synthetic aborted result) and drop the
    // management ORB. The FetchAgent is unbound by LoginSession separately.
    void OnBusReset();

    // Last-resort recovery hook: invoked when the LUN-reset escalation itself
    // fails (management ORB never fetched — the target's fetch engine is dead
    // and only a bus reset can reach it). Wired by the registry; neutral
    // callback so this layer stays ignorant of Bus/ internals.
    void SetBusResetRequester(std::function<void()> requester) {
        busResetRequester_ = std::move(requester);
    }

    // Release-time cleanup: complete the active command (aborted) and drop
    // management resources. Resources the target may still be using come back
    // parked; the caller keeps them until the logout status or a bus reset.
    [[nodiscard]] std::optional<ParkedCommand> Cleanup();

    [[nodiscard]] static bool IsSupportedTaskManagementFunction(
        SBP2ManagementORB::Function function) noexcept;

private:
    void FailActiveCommand(int transportStatus, uint8_t sbpStatus) noexcept;
    void NotifyResultCallback();
    void RetireCommand();
    void CleanupCommandResources();
    void CleanupManagementResources();

    Async::IFireWireBus& bus_;
    Async::IFireWireBusInfo& busInfo_;
    AddressSpaceManager& addrSpaceMgr_;
    LoginSession& session_;
    ISessionScheduler& scheduler_;
    void* owner_;
    int32_t& lastError_;

    // Command god-object state (moved out of #19's SBP2SessionRecord).
    std::optional<SCSI::CommandRequest> activeCommandRequest_;
    std::optional<SCSI::CommandResult> pendingCommandResult_;
    std::optional<uint8_t> activeCommandOpcode_;
    std::optional<uint8_t> lastCompletedCommandOpcode_;
    bool commandReady_{false};
    CommandPhase phase_{CommandPhase::Idle};
    std::unique_ptr<SBP2CommandORB> commandORB_;
    std::unique_ptr<SBP2PageTable> commandPageTable_;
    uint64_t commandBufferHandle_{0};
    // The data buffer is the caller's own mapping (request.dataIOVA); it may be
    // reused the moment the result is delivered.
    bool commandInPlace_{false};
    // The timeout recovery's own LUN reset is in flight (its completion owns
    // the retained resources); a client task-management ORB is not.
    bool recoveryResetInFlight_{false};
    ResultCallback resultCallback_;

    std::unique_ptr<SBP2ManagementORB> managementORB_;

    std::function<void()> busResetRequester_;

    std::shared_ptr<int> lifetimeToken_{std::make_shared<int>(0)};
};

} // namespace ASFW::Protocols::SBP2
