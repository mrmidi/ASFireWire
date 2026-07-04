#pragma once

// CommandExecutor — SBP-2 command plane for one logged-in session.
//
// Decomposed from PR #19's SBP2SessionRegistry (§2b of SBP2_SESSION_PORT.md): the
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

namespace ASFW::Async {
class IFireWireBus;
class IFireWireBusInfo;
}

namespace ASFW::Protocols::SBP2 {

class CommandExecutor {
public:
    CommandExecutor(Async::IFireWireBus& bus,
                    Async::IFireWireBusInfo& busInfo,
                    AddressSpaceManager& addrSpaceMgr,
                    LoginSession& session,
                    void* owner,
                    int32_t& lastError,
                    IODispatchQueue* workQueue) noexcept;
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

    // Release-time cleanup: drop command + management resources.
    void Cleanup();

    [[nodiscard]] static bool IsSupportedTaskManagementFunction(
        SBP2ManagementORB::Function function) noexcept;

private:
    void FailActiveCommand(int transportStatus, uint8_t sbpStatus) noexcept;
    void NotifyResultCallback();
    void RetireCommandKeepAnchor();
    void CleanupCommandResources();
    void CleanupManagementResources();

    Async::IFireWireBus& bus_;
    Async::IFireWireBusInfo& busInfo_;
    AddressSpaceManager& addrSpaceMgr_;
    LoginSession& session_;
    void* owner_;
    int32_t& lastError_;
    IODispatchQueue* workQueue_{nullptr};

    // Command god-object state (moved out of #19's SBP2SessionRecord).
    std::optional<SCSI::CommandRequest> activeCommandRequest_;
    std::optional<SCSI::CommandResult> pendingCommandResult_;
    std::optional<uint8_t> activeCommandOpcode_;
    std::optional<uint8_t> lastCompletedCommandOpcode_;
    bool commandReady_{false};
    bool commandInFlight_{false};
    std::unique_ptr<SBP2CommandORB> commandORB_;
    // Last completed ORB, kept alive as the doorbell-chain link anchor (the
    // target re-reads its next_ORB field). Dropped on reset/teardown.
    std::unique_ptr<SBP2CommandORB> linkAnchorORB_;
    std::unique_ptr<SBP2PageTable> commandPageTable_;
    uint64_t commandBufferHandle_{0};
    ResultCallback resultCallback_;

    std::unique_ptr<SBP2ManagementORB> managementORB_;

    std::function<void()> busResetRequester_;

    std::shared_ptr<int> lifetimeToken_{std::make_shared<int>(0)};
};

} // namespace ASFW::Protocols::SBP2
