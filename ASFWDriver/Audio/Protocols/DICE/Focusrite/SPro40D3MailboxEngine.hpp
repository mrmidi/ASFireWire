// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3MailboxEngine.hpp - Serialized, transport-independent engine for the
// Saffire Pro 40 (TCD3070) DCP vendor mailbox.
//
// Design (Stage 1, host-only; behavioral facts cited in SPro40D3MailboxTypes.hpp;
// revised after the Stage 1 review - see the Stage 1 report §2 for the full
// failure-semantics table):
//
//   * Serialized: at most one command is in flight. The reference protocol is
//     blocking-sequential; this engine keeps that contract explicit by
//     rejecting a new command with kBusy while one is pending. The family
//     driver that adopts this engine must serialize its callers.
//
//   * Single-threaded pump (review P0 "reentrant transport and callback
//     safety"): every public entry point runs through one re-entrancy guard.
//     While the guard is held (an entry point is executing), any re-entrant
//     engine call - a notification delivered synchronously inside a transport
//     WriteBlock, or a Send*/Poll/OnBusReset issued from inside a completion
//     callback - is QUEUED, never executed nested. The owning entry point then
//     drains the queue iteratively. Consequences, by construction:
//       - no recursive completion or command launching (the pump is a loop,
//         not a call chain);
//       - the request buffer passed to WriteBlock is engine-owned storage
//         (InFlight::request) and NOTHING can move or free it while the
//         transport call is executing, because state transitions only happen
//         between transport calls, inside the pump. Transports receive the
//         guarantee: the span stays valid for the entire synchronous
//         WriteBlock call; transports must not retain it afterwards;
//       - completion callbacks fire only between pump steps, never inside a
//         transport call. Callbacks may call any engine API; the call is
//         queued and drained with bounded depth. Callbacks must not destroy
//         the engine and must not throw: the pump is noexcept (a throw would
//         leave the queue state unrestorable, so it terminates by design).
//
//   * Exactly-once completion (review P1): the Send* return value reports
//     ACCEPTANCE only - kBusy/kNotRegistered/kInvalidArgument, with the
//     completion callback NOT invoked. Once a command is accepted, its
//     callback fires EXACTLY once, with the final outcome of the logical
//     command: success, or the MailboxError that ended it (including
//     transport failures of the initial write and of chained steps).
//     Chained commands (set_control -> write_sw_notice; set_routing ->
//     low-rate table then high-rate table) carry the ORIGINAL callback and
//     complete the logical command only when the last required step
//     completes. If a chained step fails after the primary write was
//     confirmed applied, the completion is kPartialCompletion (explicit
//     partial state; see the report's failure-semantics table).
//
//   * Bounded response-area reads (review P0 "bound response-area reads";
//     Stage 0 §7a): the response area is read ONLY paired with a completed,
//     validated notification, and the read length is bounded by the
//     descriptor's responseAreaSize. Before any read the engine validates:
//     notification frame shape (>= 12 bytes); rlen + 16 <= responseAreaSize
//     (oversized or malformed notifications are counted and dropped WITHOUT
//     reading - the genuine notification may still arrive, and the command
//     deadline bounds the wait); rlen == the command's expected response
//     length (a well-formed but unexpected length fails the command with
//     kResponseShapeMismatch, again without reading). After the paired read
//     the response header must echo the request opcode and payload length
//     and carry the in-flight counter, else kMalformedResponse /
//     kCounterMismatch.
//
//   * Notification attribution: a response whose counter matches the
//     in-flight command completes it. A counter that matches a recent
//     COMPLETED command is a stale notification (counted; the in-flight
//     command keeps waiting). Any other counter is a device-side
//     desynchronization -> kCounterMismatch.
//
//   * Deadlines are checked at ingress, not only in Poll: a notification
//     arriving at or after the in-flight deadline fails the command with
//     kTimeout and is counted stale - a late response is never trusted, and
//     the response area is never read for it. Poll(nowMs) remains the
//     fallback that bounds the wait when nothing arrives.
//
//   * Conservative recovery after ambiguous completion: a timed-out WRITE
//     may or may not have landed on the device; the engine never assumes
//     either way. Affected cached values fall back to ValueProvenance::
//     Unknown (last value kept, untrusted) and are NOT automatically
//     re-sent. The caller decides whether to re-issue the command; the
//     provenance store forces a re-read before the value is trusted again.
//     The only automatic re-send in the engine is the reference's
//     counter-reset ritual (a defined protocol behavior, not an uncertain
//     write replay).
//
//   * Ownership/registration lifecycle (Stage 0 §7a): the descriptor's +0x08
//     field is both the notification address and the ownership CAS word.
//     Registration re-reads and revalidates the descriptor from the transport
//     first (static zone; safe) - required after a bus reset, cheap always -
//     and CASes using the FRESH owner value as the expected operand. The
//     ownership-release CAS (ours -> none) is UNVERIFIED on hardware (the
//     reference never unregisters) and is disabled by default; tests opt in
//     via Options::allowOwnershipRelease.
//
//   * Counter management: u16 counter, wraps naturally. A nonzero device
//     status triggers the reference's counter-reset ritual (reset frame,
//     counter = 0, one retry). Time is explicit: every entry point that can
//     need "now" takes nowMs; deterministic for host tests, the driver passes
//     a real clock later.

#pragma once

#include "SPro40D3MailboxTypes.hpp"

#include <DriverKit/IOReturn.h>

#include <array>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

// ============================================================================
// Transport seam
// ============================================================================

/// Synchronous register-block transport. Matches the blocking shape of
/// DiceDeviceIo (one async transaction + bounded wait per call), so the
/// production adapter is a thin forwarder; host tests bind a simulated device.
/// All offsets are absolute 1394 addresses (not DICE-relative).
///
/// Lifetime contract: the engine guarantees the request span passed to
/// WriteBlock remains valid for the entire synchronous call. Transports may
/// read it at any point during the call (including after delivering a
/// synchronous notification) but must not retain it afterwards.
class ISPro40D3Transport {
public:
    virtual ~ISPro40D3Transport() = default;

    [[nodiscard]] virtual std::expected<void, IOReturn>
    WriteBlock(uint64_t address, std::span<const uint8_t> bytes) = 0;

    [[nodiscard]] virtual std::expected<std::vector<uint8_t>, IOReturn>
    ReadBlock(uint64_t address, uint32_t length) = 0;

    /// Returns the previous value at `address`; the swap happened iff the
    /// returned value equals `expected`.
    [[nodiscard]] virtual std::expected<uint64_t, IOReturn>
    CompareSwap64(uint64_t address, uint64_t expected, uint64_t desired) = 0;
};

// ---------------------------------------------------------------------------
// MailboxError codes
// ---------------------------------------------------------------------------

enum class MailboxError : uint8_t {
    kTransport,           ///< transport call failed (see LastTransportError)
    kBadDescriptor,       ///< descriptor read/parse/revalidation failed
    kBusy,                ///< a command is already in flight (serialized engine)
    kNotRegistered,       ///< no notification address registered
    kOwnershipConflict,   ///< CAS saw a foreign owner (see ForeignOwner)
    kTimeout,             ///< response deadline expired (bounded wait; late
                           ///< notifications are never trusted)
    kResponseStatus,      ///< device status stayed nonzero after the reset ritual
    kCounterMismatch,     ///< response echoed a counter matching no recent command
    kShortResponse,       ///< transport returned fewer bytes than requested
    kMalformedResponse,   ///< response header inconsistent (opcode/length echo)
    kResponseShapeMismatch, ///< well-formed notification whose length contradicts
                           ///< the command's expected response (no read performed)
    kBusReset,            ///< in-flight command invalidated by a bus reset
    kInvalidArgument,     ///< command rejected: input out of range (never sent)
    kPartialCompletion,   ///< primary write confirmed, chained step failed
    kReleaseDisabled,     ///< ownership-release CAS disabled (unverified on hw)
};

/// Reads and parses the 48-byte descriptor (static zone; always safe).
[[nodiscard]] std::expected<DcpDescriptor, MailboxError>
ReadDcpDescriptor(ISPro40D3Transport& transport);

// ============================================================================
// Engine
// ============================================================================

class SPro40D3MailboxEngine {
public:
    struct Options {
        /// Deadline for one command's notification+response exchange.
        /// The reference passes timeout_ms per transaction (spro40d3.rs
        /// send_message); 200 ms matches its typical call sites.
        uint64_t responseTimeoutMs{200};
        /// Enable the ownership-release CAS in UnregisterNotificationAddress.
        /// OFF by default: the release CAS is UNVERIFIED on hardware (the
        /// reference never unregisters) and must not be exercised against a
        /// physical device until verified. Host tests opt in.
        bool allowOwnershipRelease{false};
    };

    using VoidCallback    = std::function<void(std::expected<void, MailboxError>)>;
    using ControlCallback = std::function<void(std::expected<uint32_t, MailboxError>)>;
    /// Meter values point into engine storage; valid until the next command
    /// starts. Copy them if they must outlive the command.
    using MeterCallback   = std::function<void(std::expected<std::span<const int32_t>, MailboxError>)>;

    SPro40D3MailboxEngine(ISPro40D3Transport& transport, DcpDescriptor descriptor,
                          Options options);
    SPro40D3MailboxEngine(ISPro40D3Transport& transport, DcpDescriptor descriptor);

    // ------------------------------------------------------------------
    // Ownership / notification registration lifecycle (Stage 0 §7a)
    // ------------------------------------------------------------------

    enum class RegistrationState : uint8_t { kUnregistered, kRegistered };

    /// Registers our host notification address. Re-reads and revalidates the
    /// descriptor from the transport first (required after a bus reset; the
    /// addresses must still match the engine's configured descriptor), then
    /// classifies ownership from the fresh owner field plus this engine's
    /// own registration state (review round 3, finding A):
    ///   - unowned (kDcpOwnerNone): CAS(kDcpOwnerNone -> hostAddress). The
    ///     expected operand is the CONSTANT sentinel, never a value read
    ///     from the device, so a CAS can never target a foreign owner. A
    ///     claim racing into that window fails the CAS atomically and is
    ///     reported via kOwnershipConflict + ForeignOwner().
    ///   - self-owned: only when THIS engine previously completed a
    ///     registration CAS (state) AND the device still names our address.
    ///     Equality of the field with our intended address alone is NOT
    ///     evidence of self-ownership (stale value, garbage, and a foreign
    ///     claimant are indistinguishable) and is treated as foreign.
    ///     Idempotent re-registration requires both. A mismatch while
    ///     registered is confirmed ownership loss: kOwnershipConflict +
    ///     ForeignOwner(), and the registration state is INVALIDATED
    ///     immediately (review round 4) so every subsequent Send*() fails
    ///     kNotRegistered instead of passing its precondition against a
    ///     mailbox we know we no longer own. No reacquisition and no
    ///     release CAS happen here; re-claiming is an explicit caller
    ///     decision (report §2.3a / §5.7).
    ///   - foreign-owned: kOwnershipConflict + ForeignOwner(); ZERO CAS
    ///     operations are performed.
    /// kBadDescriptor if the device no longer matches. kOwnershipConflict
    /// when already registered to a different address.
    [[nodiscard]] std::expected<void, MailboxError>
    RegisterNotificationAddress(uint64_t hostAddress);

    /// CAS our address back to kDcpOwnerNone. UNVERIFIED on hardware (the
    /// reference never unregisters) and DISABLED by default: returns
    /// kReleaseDisabled unless Options::allowOwnershipRelease is set. See the
    /// Stage 1 report, unverified assumptions.
    [[nodiscard]] std::expected<void, MailboxError> UnregisterNotificationAddress();

    /// Bus reset: registration is lost (TCAT firmware drops the owner every
    /// generation - see SimulatedDiceDevice.hpp busResetClearsOwner rationale),
    /// the counter restarts at 0, any in-flight command fails with kBusReset,
    /// and every control value falls back to ValueProvenance::Unknown.
    /// Re-registration revalidates the descriptor before any CAS.
    void OnBusReset();

    [[nodiscard]] RegistrationState GetRegistrationState() const noexcept { return registration_; }
    [[nodiscard]] uint64_t NotificationAddress() const noexcept { return notificationAddress_; }
    /// Observed foreign owner after kOwnershipConflict; 0 when none.
    [[nodiscard]] uint64_t ForeignOwner() const noexcept { return foreignOwner_; }

    // ------------------------------------------------------------------
    // Commands (serialized; one in flight at a time)
    // ------------------------------------------------------------------
    // Acceptance contract (exactly-once): the return value reports ACCEPTANCE
    // only - kBusy/kNotRegistered/kInvalidArgument, callback NOT invoked. Once
    // accepted, the callback fires exactly once with the final outcome of the
    // LOGICAL command (including chained steps). A callback may call engine
    // APIs; the call is queued and drained after the current step.

    /// Mixer crosspoint volume (channel < 18, mix bus < 16, 0 <= volume <=
    /// 0xFFFF - the wire carries 16 bits; device-side clamping unverified).
    /// On success the value is Cached (host-written).
    [[nodiscard]] std::expected<void, MailboxError>
    SendSetVolume(uint8_t channel, uint8_t mix, int32_t volume,
                  uint64_t nowMs, VoidCallback completion);

    /// Master (output) meters; currentRateHz > 0. Rate selects the meter
    /// position (> 48000 = high-rate positions, spro40d3.rs).
    [[nodiscard]] std::expected<void, MailboxError>
    SendGetMasterMeter(uint32_t currentRateHz, uint64_t nowMs, MeterCallback completion);

    /// Mixer input meters (18 channels); currentRateHz > 0.
    [[nodiscard]] std::expected<void, MailboxError>
    SendGetMixerMeter(uint32_t currentRateHz, uint64_t nowMs, MeterCallback completion);

    /// Reads one u32 from the control store. On success the value is Observed.
    [[nodiscard]] std::expected<void, MailboxError>
    SendGetControl32(uint32_t position, uint64_t nowMs, ControlCallback completion);

    /// Writes one u32 to the control store, then sends the software notice
    /// (the reference chains write_sw_notice after every set; spro40d3.rs
    /// set_control_u32). swNotice must be one of the sw_notice:: constants.
    /// The completion fires after the notice; if the notice step fails after
    /// the write was confirmed, the completion is kPartialCompletion (the
    /// value stays Cached - the write itself was confirmed). On full success
    /// the value is Cached.
    [[nodiscard]] std::expected<void, MailboxError>
    SendSetControl32(uint32_t position, uint32_t value, uint32_t swNotice,
                     uint64_t nowMs, VoidCallback completion);

    /// Writes BOTH routing tables (low-rate, then high-rate) as the reference
    /// does (spro40d3.rs set_routing). The completion fires after the second;
    /// if the high-rate write fails after the low-rate write was confirmed,
    /// the completion is kPartialCompletion and the routing mirror falls back
    /// to Unknown (mixed state, untrusted). On full success Cached.
    [[nodiscard]] std::expected<void, MailboxError>
    SendSetRouting(const RouterOutSrc& outSrc, const RouterMixerSrc& mixerSrc,
                   const RouterMeterSrc& meterSrc, uint64_t nowMs, VoidCallback completion);

    // ------------------------------------------------------------------
    // Device ingress
    // ------------------------------------------------------------------

    /// Delivers a notification block-write that arrived at the registered host
    /// address. Malformed (short) or oversized (rlen + 16 > response area)
    /// frames are counted and dropped without reading the response area; the
    /// in-flight command keeps waiting (its deadline bounds the wait). A
    /// notification with no command in flight, whose counter matches a recent
    /// completed command, or arriving at/after the in-flight deadline is
    /// stale: counted, no read.
    void OnNotification(std::span<const uint8_t> frame, uint64_t nowMs);

    /// Drives deadlines. Fails an expired in-flight command with kTimeout and
    /// marks its write targets Unknown (uncertain whether the device applied
    /// them). Never re-sends.
    void Poll(uint64_t nowMs);

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------

    [[nodiscard]] const SPro40D3ControlState& ControlState() const noexcept { return state_; }
    [[nodiscard]] uint16_t CommandCounter() const noexcept { return counter_; }
    /// Host-side counter override. The device echoes the counter it is sent;
    /// the reference never validates one device-side (unverified on hardware -
    /// see the Stage 1 report). Used to resynchronize after re-registration
    /// and by tests for wraparound.
    void OverrideCommandCounter(uint16_t value) noexcept { counter_ = value; }
    [[nodiscard]] uint32_t StaleNotificationCount() const noexcept { return staleNotifications_; }
    /// Malformed/oversized ingress frames (rejected without any response-area
    /// read). Invariant observer for the bounded-read guarantee.
    [[nodiscard]] uint32_t MalformedNotificationCount() const noexcept {
        return malformedNotifications_;
    }
    [[nodiscard]] uint32_t LastDeviceStatus() const noexcept { return lastDeviceStatus_; }
    [[nodiscard]] IOReturn LastTransportError() const noexcept { return lastTransportError_; }
    [[nodiscard]] bool CommandInFlight() const noexcept { return inFlight_.has_value(); }

private:
    enum class Kind : uint8_t {
        kSetVolume, kGetMasterMeter, kGetMixerMeter,
        kGetControl, kSetControl, kSwNotice, kSetRoutingLow, kSetRoutingHigh,
    };

    struct InFlight {
        Kind kind;
        std::vector<uint8_t> request;  ///< full frame incl. header; engine-owned
                                       ///< storage lent to WriteBlock (see seam)
        uint16_t counter{};
        uint64_t deadlineMs{};
        bool usedResetRitual{};
        bool isChainedStep{};          ///< failure reports kPartialCompletion
        uint32_t expectedResponseLength{};
        VoidCallback voidCb{};
        ControlCallback controlCb{};
        MeterCallback meterCb{};
        // Typed decode context
        uint32_t controlPos{};
        uint32_t swNotice{};
        uint8_t volChannel{}, volMix{};
        int32_t volValue{};
        RouterOutSrc routerOut{};
        RouterMixerSrc routerMixer{};
        RouterMeterSrc routerMeter{};
    };

    // -- pump plumbing (see design block) --------------------------------
    [[nodiscard]] bool Enter() noexcept;      ///< false if re-entrant: caller queues
    void Exit() noexcept;                     ///< Pump() then release the guard
    void LaunchTopLevel(InFlight cmd, uint64_t nowMs); ///< Enter+launch+drain
    void Pump() noexcept;                     ///< drain queued work iteratively
    void HandleBusReset() noexcept;           ///< the actual reset state change
    void HandlePoll(uint64_t nowMs) noexcept;
    void ProcessNotification(std::span<const uint8_t> frame, uint64_t arrivalMs);
    void Dispatch(InFlight cmd, std::span<const uint8_t> responsePayload,
                  uint64_t nowMs);
    void DoLaunch(InFlight cmd, uint64_t nowMs, MailboxError writeFailError);
    void CompleteFailure(MailboxError error);          ///< moves + fires inFlight_
    void FireCompletion(InFlight& cmd, MailboxError error) noexcept;
    void FireSuccessVoid(InFlight& cmd) noexcept;
    void InvalidateWriteTargets(const InFlight& cmd) noexcept; ///< ambiguous completion
    void RememberCounter(uint16_t counter) noexcept;
    [[nodiscard]] bool CounterIsRecent(uint16_t counter) const noexcept;
    [[nodiscard]] static Opcode OpcodeFor(Kind kind) noexcept;
    [[nodiscard]] static uint32_t ExpectedResponseLength(Kind kind) noexcept;

    // -- provenance store -------------------------------------------------
    void DecodeAndStoreControl(uint32_t position, uint32_t value, ValueProvenance prov) noexcept;
    void ApplyObserved(uint32_t position, uint32_t value) noexcept;
    void ApplyCached(uint32_t position, uint32_t value) noexcept;
    void InvalidateControl(uint32_t position) noexcept;

    ISPro40D3Transport& transport_;
    DcpDescriptor descriptor_;
    Options options_;

    RegistrationState registration_{RegistrationState::kUnregistered};
    uint64_t notificationAddress_{0};
    uint64_t foreignOwner_{0};

    uint16_t counter_{0};
    std::optional<InFlight> inFlight_{};
    SPro40D3ControlState state_{};
    std::array<int32_t, kChannelCount> meterBuffer_{};
    uint32_t staleNotifications_{0};
    uint32_t malformedNotifications_{0};
    uint32_t lastDeviceStatus_{0};
    IOReturn lastTransportError_{kIOReturnSuccess};

    // Pump state: work queued while an entry point is executing.
    bool active_{false};
    std::deque<std::pair<std::vector<uint8_t>, uint64_t>> queuedNotifications_{};
    std::deque<std::pair<InFlight, uint64_t>> queuedLaunches_{};
    bool queuedBusReset_{false};
    std::optional<uint64_t> queuedPoll_{};

    // Recent counters for stale-notification attribution (ring of 4).
    static constexpr size_t kRecentCounterWindow = 4;
    std::array<uint16_t, kRecentCounterWindow> recentCounters_{};
    size_t recentCounterCount_{0};
    size_t recentCounterNext_{0};
};

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
