// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3MailboxEngine.cpp - see SPro40D3MailboxEngine.hpp for the design.

#include "SPro40D3MailboxEngine.hpp"

#include <utility>

namespace ASFW::Audio::DICE::Focusrite::SPro40D3 {

std::expected<DcpDescriptor, MailboxError>
ReadDcpDescriptor(ISPro40D3Transport& transport) {
    auto raw = transport.ReadBlock(kDcpBaseAddress, static_cast<uint32_t>(kDcpDescriptorSize));
    if (!raw) return std::unexpected(MailboxError::kBadDescriptor);
    auto parsed = ParseDcpDescriptor(std::span<const uint8_t>(*raw));
    if (!parsed) return std::unexpected(MailboxError::kBadDescriptor);
    return *parsed;
}

SPro40D3MailboxEngine::SPro40D3MailboxEngine(ISPro40D3Transport& transport,
                                             DcpDescriptor descriptor, Options options)
    : transport_{transport}, descriptor_{descriptor}, options_{options} {}

SPro40D3MailboxEngine::SPro40D3MailboxEngine(ISPro40D3Transport& transport,
                                             DcpDescriptor descriptor)
    : SPro40D3MailboxEngine(transport, descriptor, Options{}) {}

// ---------------------------------------------------------------------------
// Pump: single-threaded deferred processing (review P0 reentrancy)
// ---------------------------------------------------------------------------

bool SPro40D3MailboxEngine::Enter() noexcept {
    if (active_) return false; // re-entrant call: caller must queue
    active_ = true;
    return true;
}

void SPro40D3MailboxEngine::Exit() noexcept {
    Pump();
    active_ = false;
}

void SPro40D3MailboxEngine::LaunchTopLevel(InFlight cmd, uint64_t nowMs) {
    // Called only from Send* after they verified active_ == false (and
    // queued instead otherwise), so the guard is free here by construction.
    active_ = true;
    DoLaunch(std::move(cmd), nowMs, MailboxError::kTransport);
    Exit();
}

void SPro40D3MailboxEngine::Pump() noexcept {
    for (;;) {
        if (queuedBusReset_) {
            queuedBusReset_ = false;
            HandleBusReset();
            continue;
        }
        if (queuedPoll_) {
            const uint64_t now = *queuedPoll_;
            queuedPoll_.reset();
            HandlePoll(now);
            continue;
        }
        if (!queuedNotifications_.empty()) {
            auto [frame, now] = std::move(queuedNotifications_.front());
            queuedNotifications_.pop_front();
            ProcessNotification(frame, now);
            continue;
        }
        if (!queuedLaunches_.empty()) {
            auto [cmd, now] = std::move(queuedLaunches_.front());
            queuedLaunches_.pop_front();
            if (registration_ != RegistrationState::kRegistered) {
                // Accepted, then a bus reset unregistered us before the
                // queued launch could run: the reset is what killed it.
                FireCompletion(cmd, MailboxError::kBusReset);
            } else if (inFlight_) {
                // Serialized slot occupied (an earlier queued launch is
                // still exchanging with the device). The acceptance
                // contract still holds: complete exactly once, with kBusy.
                FireCompletion(cmd, MailboxError::kBusy);
            } else {
                DoLaunch(std::move(cmd), now, MailboxError::kTransport);
            }
            continue;
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Ownership / registration lifecycle
// ---------------------------------------------------------------------------

std::expected<void, MailboxError>
SPro40D3MailboxEngine::RegisterNotificationAddress(uint64_t hostAddress) {
    // Register/Unregister return their result synchronously, so they cannot
    // be deferred: they are not callable from inside completion callbacks.
    if (active_) return std::unexpected(MailboxError::kBusy);
    if (inFlight_) return std::unexpected(MailboxError::kBusy);
    if (registration_ == RegistrationState::kRegistered &&
        hostAddress != notificationAddress_) {
        // Already registered to a different address: release first (the
        // release CAS is gated). Not a foreign-owner case; ForeignOwner()
        // is left untouched.
        return std::unexpected(MailboxError::kOwnershipConflict);
    }

    // Descriptor revalidation (review: required after a bus reset; cheap
    // always). The 48-byte header is a static zone (Stage 0 §7a) - safe to
    // read at any time. The device must still be the unit this engine was
    // configured for.
    auto raw = transport_.ReadBlock(kDcpBaseAddress,
                                    static_cast<uint32_t>(kDcpDescriptorSize));
    if (!raw) {
        lastTransportError_ = raw.error();
        return std::unexpected(MailboxError::kBadDescriptor);
    }
    auto fresh = ParseDcpDescriptor(std::span<const uint8_t>(*raw));
    if (!fresh) return std::unexpected(MailboxError::kBadDescriptor);
    if (fresh->writeAddress != descriptor_.writeAddress ||
        fresh->readAddress != descriptor_.readAddress ||
        fresh->responseAreaSize != descriptor_.responseAreaSize ||
        fresh->maxRequestSize != descriptor_.maxRequestSize ||
        fresh->version != descriptor_.version) {
        // Different unit / firmware revision: do not touch it further.
        return std::unexpected(MailboxError::kBadDescriptor);
    }

    // Ownership classification (review round 3, finding A). The fresh owner
    // field alone can never authorize a takeover: a CAS whose expected
    // operand was read from the device would succeed against a steady
    // foreign owner and steal the mailbox. Only the unowned state may be
    // claimed, and only against the CONSTANT none sentinel.
    const uint64_t observed = fresh->notificationAddress;

    if (registration_ == RegistrationState::kRegistered) {
        // Self-ownership is established by THIS engine's prior successful
        // CAS (state), corroborated by the device still naming our address.
        if (observed == notificationAddress_)
            return std::expected<void, MailboxError>{}; // idempotent; zero CAS
        // The device no longer names us: stolen, or cleared by a reset we
        // missed (ForeignOwner() may report kDcpOwnerNone in that case).
        // Never CAS back over whatever is there now.
        // Confirmed ownership loss invalidates the registration state
        // immediately (review round 4): a stale kRegistered would let
        // subsequent Send*() pass their precondition against a mailbox we
        // know we no longer own. No reacquisition CAS, no release CAS -
        // re-claiming is an explicit caller decision (§2.3a / §5.7);
        // ForeignOwner() preserves the observed value for diagnostics.
        foreignOwner_ = observed;
        registration_ = RegistrationState::kUnregistered;
        return std::unexpected(MailboxError::kOwnershipConflict);
    }

    if (observed != kDcpOwnerNone) {
        // Foreign-owned - INCLUDING observed == hostAddress: without
        // engine-state corroboration, the field equaling our intended
        // address is not self-ownership (a stale value from a previous
        // process, garbage, or a foreign claimant at that address are
        // indistinguishable). Zero CAS operations; report and stop.
        // Recovery is the operator's call: device power-cycle, or a bus
        // reset IF the firmware drops ownership on reset (unverified -
        // report §5.7).
        foreignOwner_ = observed;
        return std::unexpected(MailboxError::kOwnershipConflict);
    }

    // Unowned: claim with the CONSTANT sentinel as the expected operand. A
    // racer claiming between our read and this CAS fails it atomically -
    // the field is untouched by us and the racer is reported.
    auto prev = transport_.CompareSwap64(kDcpBaseAddress + 8,
                                         kDcpOwnerNone, hostAddress);
    if (!prev) {
        lastTransportError_ = prev.error();
        return std::unexpected(MailboxError::kTransport);
    }
    if (*prev != kDcpOwnerNone) {
        foreignOwner_ = *prev;
        return std::unexpected(MailboxError::kOwnershipConflict);
    }
    foreignOwner_ = 0;
    notificationAddress_ = hostAddress;
    registration_ = RegistrationState::kRegistered;
    return {};
}

std::expected<void, MailboxError>
SPro40D3MailboxEngine::UnregisterNotificationAddress() {
    if (active_) return std::unexpected(MailboxError::kBusy);
    if (inFlight_) return std::unexpected(MailboxError::kBusy);
    if (registration_ != RegistrationState::kRegistered)
        return std::unexpected(MailboxError::kNotRegistered);
    // The release CAS is UNVERIFIED on hardware (the reference never
    // unregisters). Disabled unless tests opt in (review requirement).
    if (!options_.allowOwnershipRelease)
        return std::unexpected(MailboxError::kReleaseDisabled);
    auto prev = transport_.CompareSwap64(kDcpBaseAddress + 8,
                                         notificationAddress_, kDcpOwnerNone);
    if (!prev) {
        lastTransportError_ = prev.error();
        return std::unexpected(MailboxError::kTransport);
    }
    if (*prev != notificationAddress_) {
        foreignOwner_ = *prev;
        return std::unexpected(MailboxError::kOwnershipConflict);
    }
    registration_ = RegistrationState::kUnregistered;
    notificationAddress_ = 0;
    return {};
}

void SPro40D3MailboxEngine::OnBusReset() {
    if (!Enter()) { queuedBusReset_ = true; return; }
    HandleBusReset();
    Exit();
}

void SPro40D3MailboxEngine::HandleBusReset() noexcept {
    // See SimulatedDiceDevice.hpp (busResetClearsOwner): TCAT re-claims
    // ownership every generation, implying the firmware drops the owner on
    // reset. Cached values are invalidated for the same reason: the hardware
    // never confirmed them (Stage 0 §7a).
    registration_ = RegistrationState::kUnregistered;
    notificationAddress_ = 0;
    foreignOwner_ = 0;
    counter_ = 0;
    state_.InvalidateAll();
    if (inFlight_) {
        InFlight cmd = std::move(*inFlight_);
        inFlight_.reset();
        FireCompletion(cmd, MailboxError::kBusReset);
    }
}

// ---------------------------------------------------------------------------
// Command send path
// ---------------------------------------------------------------------------

std::expected<void, MailboxError>
SPro40D3MailboxEngine::SendSetVolume(uint8_t channel, uint8_t mix, int32_t volume,
                                     uint64_t nowMs, VoidCallback completion) {
    if (inFlight_) return std::unexpected(MailboxError::kBusy);
    if (registration_ != RegistrationState::kRegistered)
        return std::unexpected(MailboxError::kNotRegistered);
    if (channel >= kChannelCount || mix >= kMixCount ||
        volume < 0 || volume > 0xFFFF)
        return std::unexpected(MailboxError::kInvalidArgument);
    InFlight cmd;
    cmd.kind = Kind::kSetVolume;
    cmd.expectedResponseLength = ExpectedResponseLength(Kind::kSetVolume);
    cmd.request.resize(kSetVolumeFrameSize);
    SerializeSetVolume(std::span<uint8_t, kSetVolumeFrameSize>(cmd.request), channel, mix, volume);
    cmd.volChannel = channel; cmd.volMix = mix; cmd.volValue = volume;
    cmd.voidCb = std::move(completion);
    if (active_) { queuedLaunches_.emplace_back(std::move(cmd), nowMs); return {}; }
    LaunchTopLevel(std::move(cmd), nowMs);
    return {};
}

std::expected<void, MailboxError>
SPro40D3MailboxEngine::SendGetMasterMeter(uint32_t currentRateHz, uint64_t nowMs,
                                          MeterCallback completion) {
    if (inFlight_) return std::unexpected(MailboxError::kBusy);
    if (registration_ != RegistrationState::kRegistered)
        return std::unexpected(MailboxError::kNotRegistered);
    if (currentRateHz == 0) return std::unexpected(MailboxError::kInvalidArgument);
    InFlight cmd;
    cmd.kind = Kind::kGetMasterMeter;
    cmd.expectedResponseLength = ExpectedResponseLength(Kind::kGetMasterMeter);
    cmd.request.resize(kGetMeterFrameSize);
    const bool highRate = currentRateHz > 48000;
    SerializeGetMeter(std::span<uint8_t, kGetMeterFrameSize>(cmd.request),
                      highRate ? kMasterMeterPosHighRate : kMasterMeterPosLowRate,
                      kMasterMeterCount);
    cmd.meterCb = std::move(completion);
    if (active_) { queuedLaunches_.emplace_back(std::move(cmd), nowMs); return {}; }
    LaunchTopLevel(std::move(cmd), nowMs);
    return {};
}

std::expected<void, MailboxError>
SPro40D3MailboxEngine::SendGetMixerMeter(uint32_t currentRateHz, uint64_t nowMs,
                                         MeterCallback completion) {
    if (inFlight_) return std::unexpected(MailboxError::kBusy);
    if (registration_ != RegistrationState::kRegistered)
        return std::unexpected(MailboxError::kNotRegistered);
    if (currentRateHz == 0) return std::unexpected(MailboxError::kInvalidArgument);
    InFlight cmd;
    cmd.kind = Kind::kGetMixerMeter;
    cmd.expectedResponseLength = ExpectedResponseLength(Kind::kGetMixerMeter);
    cmd.request.resize(kGetMeterFrameSize);
    const bool highRate = currentRateHz > 48000;
    SerializeGetMeter(std::span<uint8_t, kGetMeterFrameSize>(cmd.request),
                      highRate ? kMixerMeterPosHighRate : kMixerMeterPosLowRate,
                      kChannelCount);
    cmd.meterCb = std::move(completion);
    if (active_) { queuedLaunches_.emplace_back(std::move(cmd), nowMs); return {}; }
    LaunchTopLevel(std::move(cmd), nowMs);
    return {};
}

std::expected<void, MailboxError>
SPro40D3MailboxEngine::SendGetControl32(uint32_t position, uint64_t nowMs,
                                        ControlCallback completion) {
    if (inFlight_) return std::unexpected(MailboxError::kBusy);
    if (registration_ != RegistrationState::kRegistered)
        return std::unexpected(MailboxError::kNotRegistered);
    InFlight cmd;
    cmd.kind = Kind::kGetControl;
    cmd.expectedResponseLength = ExpectedResponseLength(Kind::kGetControl);
    cmd.request.resize(kGetControlFrameSize);
    SerializeGetControl(std::span<uint8_t, kGetControlFrameSize>(cmd.request), position, 4);
    cmd.controlPos = position;
    cmd.controlCb = std::move(completion);
    if (active_) { queuedLaunches_.emplace_back(std::move(cmd), nowMs); return {}; }
    LaunchTopLevel(std::move(cmd), nowMs);
    return {};
}

std::expected<void, MailboxError>
SPro40D3MailboxEngine::SendSetControl32(uint32_t position, uint32_t value, uint32_t swNotice,
                                        uint64_t nowMs, VoidCallback completion) {
    if (inFlight_) return std::unexpected(MailboxError::kBusy);
    if (registration_ != RegistrationState::kRegistered)
        return std::unexpected(MailboxError::kNotRegistered);
    if (swNotice != sw_notice::kOutputVolume && swNotice != sw_notice::kMuteDim &&
        swNotice != sw_notice::kMonitorPad && swNotice != sw_notice::kAdatAsSpdif)
        return std::unexpected(MailboxError::kInvalidArgument);
    InFlight cmd;
    cmd.kind = Kind::kSetControl;
    cmd.expectedResponseLength = ExpectedResponseLength(Kind::kSetControl);
    cmd.request.resize(28); // header + pos/len + 4-byte value
    const std::array<uint8_t, 4> valueBE{
        static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    SerializeSetControl(cmd.request, position, valueBE);
    cmd.controlPos = position;
    cmd.swNotice = swNotice;
    cmd.voidCb = std::move(completion);
    if (active_) { queuedLaunches_.emplace_back(std::move(cmd), nowMs); return {}; }
    LaunchTopLevel(std::move(cmd), nowMs);
    return {};
}

std::expected<void, MailboxError>
SPro40D3MailboxEngine::SendSetRouting(const RouterOutSrc& outSrc, const RouterMixerSrc& mixerSrc,
                                      const RouterMeterSrc& meterSrc, uint64_t nowMs,
                                      VoidCallback completion) {
    if (inFlight_) return std::unexpected(MailboxError::kBusy);
    if (registration_ != RegistrationState::kRegistered)
        return std::unexpected(MailboxError::kNotRegistered);
    // Source-domain validation (review round 3, finding B): every entry is
    // a symbolic source number; anything outside [0, kMaxSymbolicRouteSource]
    // has no device meaning and would overflow the route entry's 12-bit
    // source field, contaminating the destination bits. Rejected BEFORE
    // serialization: zero transport operations, callback not invoked.
    for (uint32_t from : outSrc)
        if (!IsValidRouteSource(from)) return std::unexpected(MailboxError::kInvalidArgument);
    for (uint32_t from : mixerSrc)
        if (!IsValidRouteSource(from)) return std::unexpected(MailboxError::kInvalidArgument);
    for (uint32_t from : meterSrc)
        if (!IsValidRouteSource(from)) return std::unexpected(MailboxError::kInvalidArgument);
    InFlight cmd;
    cmd.kind = Kind::kSetRoutingLow;
    cmd.expectedResponseLength = ExpectedResponseLength(Kind::kSetRoutingLow);
    cmd.request.resize(kRoutingLowRateFrameSize);
    SerializeRoutingLowRate(std::span<uint8_t, kRoutingLowRateFrameSize>(cmd.request),
                            outSrc, mixerSrc, meterSrc);
    cmd.routerOut = outSrc; cmd.routerMixer = mixerSrc; cmd.routerMeter = meterSrc;
    cmd.voidCb = std::move(completion);
    if (active_) { queuedLaunches_.emplace_back(std::move(cmd), nowMs); return {}; }
    LaunchTopLevel(std::move(cmd), nowMs);
    return {};
}

void SPro40D3MailboxEngine::DoLaunch(InFlight cmd, uint64_t nowMs,
                                     MailboxError writeFailError) {
    // Serialize the counter here, once: serializers wrote only the opcode and
    // payload (spro40d3.rs send_command fills [4..8] at send time).
    cmd.counter = counter_++;
    RememberCounter(cmd.counter);
    WriteMessageHeader(cmd.request, OpcodeFor(cmd.kind), cmd.counter,
                       static_cast<uint16_t>(cmd.request.size() - kMessageHeaderSize));
    cmd.deadlineMs = nowMs + options_.responseTimeoutMs;
    if (cmd.request.size() > descriptor_.maxRequestSize) {
        FireCompletion(cmd, MailboxError::kInvalidArgument); // defensive; static frames
        return;
    }
    // The request buffer is engine-owned storage (InFlight::request). Nothing
    // can move or free it while WriteBlock executes: re-entrant engine calls
    // queue (see Enter), and state transitions only happen between transport
    // calls, inside the pump. Transports get the lifetime guarantee declared
    // at the ISPro40D3Transport seam.
    inFlight_ = std::move(cmd);
    auto written = transport_.WriteBlock(descriptor_.writeAddress,
                                         std::span<const uint8_t>(inFlight_->request));
    if (!written) {
        lastTransportError_ = written.error();
        InFlight failed = std::move(*inFlight_);
        inFlight_.reset();
        FireCompletion(failed, writeFailError);
    }
    // A transport that delivered notifications synchronously inside WriteBlock
    // had them queued by OnNotification; the pump drains them next.
}

// ---------------------------------------------------------------------------
// Device ingress
// ---------------------------------------------------------------------------

void SPro40D3MailboxEngine::OnNotification(std::span<const uint8_t> frame, uint64_t nowMs) {
    if (!Enter()) {
        queuedNotifications_.emplace_back(
            std::vector<uint8_t>(frame.begin(), frame.end()), nowMs);
        return;
    }
    ProcessNotification(frame, nowMs);
    Exit();
}

void SPro40D3MailboxEngine::ProcessNotification(std::span<const uint8_t> frame,
                                                uint64_t arrivalMs) {
    auto notif = ParseNotification(frame);
    if (!notif) {
        // Malformed ingress: never read the response area for it. The
        // genuine notification may still arrive; the deadline bounds the wait.
        ++malformedNotifications_;
        return;
    }
    if (!inFlight_) {
        ++staleNotifications_;
        return;
    }
    lastDeviceStatus_ = notif->responseStatus;

    // Deadline checked at ingress (review P1): a response arriving at/after
    // the deadline is ambiguous - fail the command as timed out, count the
    // notification stale, and never read the response area for it.
    if (arrivalMs >= inFlight_->deadlineMs) {
        ++staleNotifications_;
        CompleteFailure(MailboxError::kTimeout);
        return;
    }

    if (notif->responseStatus != 0) {
        if (inFlight_->usedResetRitual) {
            CompleteFailure(MailboxError::kResponseStatus);
            return;
        }
        // Counter-reset ritual (spro40d3.rs send_command): reset frame, counter
        // back to 0, retry the SAME command once.
        std::array<uint8_t, kCounterResetFrameSize> reset{};
        reset[0] = 0x80;
        auto ok = transport_.WriteBlock(descriptor_.writeAddress, reset);
        if (!ok) {
            lastTransportError_ = ok.error();
            CompleteFailure(MailboxError::kTransport);
            return;
        }
        inFlight_->counter = 0;
        RememberCounter(0);
        counter_ = 1; // the retry consumes 0; the next command uses 1
        inFlight_->usedResetRitual = true;
        WriteMessageHeader(inFlight_->request, OpcodeFor(inFlight_->kind), 0,
                           static_cast<uint16_t>(inFlight_->request.size() - kMessageHeaderSize));
        inFlight_->deadlineMs = arrivalMs + options_.responseTimeoutMs;
        auto resent = transport_.WriteBlock(descriptor_.writeAddress,
                                            std::span<const uint8_t>(inFlight_->request));
        if (!resent) {
            lastTransportError_ = resent.error();
            CompleteFailure(MailboxError::kTransport);
            return;
        }
        return; // await the retried notification
    }

    // ---- Bounded response-area read (review P0; Stage 0 §7a) --------------
    // Every check below happens BEFORE the transport read.
    const uint32_t rlen = notif->responsePayloadLength;
    const uint32_t readLen = static_cast<uint32_t>(kMessageHeaderSize) + rlen;
    if (readLen > descriptor_.responseAreaSize) {
        // Oversized: adversarial or desynchronized device. Never read; the
        // deadline bounds the in-flight command.
        ++malformedNotifications_;
        return;
    }
    if (rlen != inFlight_->expectedResponseLength) {
        // Well-formed but contradicts the command's expected response shape:
        // our size model is wrong for this command. Fail it without reading.
        CompleteFailure(MailboxError::kResponseShapeMismatch);
        return;
    }

    // The one paired read of the response area.
    auto response = transport_.ReadBlock(descriptor_.readAddress, readLen);
    if (!response) {
        lastTransportError_ = response.error();
        // The notification says the device answered, but the confirmation
        // is unreadable: for a write, whether it applied is unknown.
        InvalidateWriteTargets(*inFlight_);
        CompleteFailure(MailboxError::kTransport);
        return;
    }
    if (response->size() < readLen) {
        CompleteFailure(MailboxError::kShortResponse);
        return;
    }
    const std::span<const uint8_t> responseSpan{*response};
    auto header = ParseResponseHeader(responseSpan);
    if (!header) {
        CompleteFailure(MailboxError::kShortResponse);
        return;
    }
    if (header->counter != inFlight_->counter) {
        if (CounterIsRecent(header->counter)) {
            // Response belongs to a recent COMPLETED command: this
            // notification is stale. The in-flight command keeps waiting.
            ++staleNotifications_;
            return;
        }
        CompleteFailure(MailboxError::kCounterMismatch);
        return;
    }
    if (!(header->opcode == OpcodeFor(inFlight_->kind))) {
        CompleteFailure(MailboxError::kMalformedResponse);
        return;
    }
    if (header->payloadLength != rlen) {
        CompleteFailure(MailboxError::kMalformedResponse);
        return;
    }

    InFlight cmd = std::move(*inFlight_);
    inFlight_.reset();
    Dispatch(std::move(cmd), responseSpan, arrivalMs);
}

void SPro40D3MailboxEngine::Dispatch(InFlight cmd,
                                     std::span<const uint8_t> response,
                                     uint64_t nowMs) {
    switch (cmd.kind) {
    case Kind::kSetVolume:
        state_.mixerVolumes[cmd.volChannel][cmd.volMix] = {cmd.volValue, ValueProvenance::Cached};
        FireSuccessVoid(cmd);
        break;
    case Kind::kGetMasterMeter:
        DeserializeMeters(response,
                           std::span<int32_t>(meterBuffer_).first(kMasterMeterCount));
        if (cmd.meterCb)
            cmd.meterCb(std::span<const int32_t>(meterBuffer_).first(kMasterMeterCount));
        break;
    case Kind::kGetMixerMeter:
        DeserializeMeters(response, meterBuffer_);
        if (cmd.meterCb) cmd.meterCb(std::span<const int32_t>(meterBuffer_));
        break;
    case Kind::kGetControl: {
        const uint32_t value = ::ASFW::FW::ReadBE32(response.data() + kMessageHeaderSize);
        ApplyObserved(cmd.controlPos, value);
        if (cmd.controlCb) cmd.controlCb(value);
        break;
    }
    case Kind::kSetControl: {
        // The write itself was confirmed by its response. Chain the software
        // notice (reference pairs every set with write_sw_notice); the
        // ORIGINAL callback travels with the chain and fires when the notice
        // completes. If the notice step fails, the completion is
        // kPartialCompletion: the value stays Cached (confirmed), the notice
        // did not land.
        ApplyCached(cmd.controlPos, ::ASFW::FW::ReadBE32(cmd.request.data() + 24));
        InFlight notice;
        notice.kind = Kind::kSwNotice;
        notice.isChainedStep = true;
        notice.expectedResponseLength = ExpectedResponseLength(Kind::kSwNotice);
        notice.request.resize(kSwNoticeFrameSize);
        SerializeWriteSwNotice(std::span<uint8_t, kSwNoticeFrameSize>(notice.request), cmd.swNotice);
        notice.voidCb = std::move(cmd.voidCb);
        DoLaunch(std::move(notice), nowMs, MailboxError::kPartialCompletion);
        break;
    }
    case Kind::kSwNotice:
        FireSuccessVoid(cmd); // the logical set_control command is complete
        break;
    case Kind::kSetRoutingLow: {
        // Low-rate table confirmed; chain the high-rate table. The original
        // callback travels with it. If the high-rate step fails, completion
        // is kPartialCompletion and the routing mirror falls back to Unknown
        // (mixed state: low applied, high not).
        InFlight high;
        high.kind = Kind::kSetRoutingHigh;
        high.isChainedStep = true;
        high.expectedResponseLength = ExpectedResponseLength(Kind::kSetRoutingHigh);
        high.request.resize(kRoutingHighRateFrameSize);
        SerializeRoutingHighRate(std::span<uint8_t, kRoutingHighRateFrameSize>(high.request),
                                  cmd.routerOut, cmd.routerMixer, cmd.routerMeter);
        high.routerOut = cmd.routerOut; high.routerMixer = cmd.routerMixer;
        high.routerMeter = cmd.routerMeter;
        high.voidCb = std::move(cmd.voidCb);
        DoLaunch(std::move(high), nowMs, MailboxError::kPartialCompletion);
        break;
    }
    case Kind::kSetRoutingHigh:
        for (size_t i = 0; i < kOutputCount; ++i)
            state_.routerOutSrc[i] = {cmd.routerOut[i], ValueProvenance::Cached};
        for (size_t i = 0; i < kChannelCount; ++i)
            state_.routerMixerSrc[i] = {cmd.routerMixer[i], ValueProvenance::Cached};
        for (size_t i = 0; i < kMasterMeterCount; ++i)
            state_.routerMeterSrc[i] = {cmd.routerMeter[i], ValueProvenance::Cached};
        FireSuccessVoid(cmd);
        break;
    }
}

// ---------------------------------------------------------------------------
// Deadlines
// ---------------------------------------------------------------------------

void SPro40D3MailboxEngine::Poll(uint64_t nowMs) {
    if (!Enter()) { queuedPoll_ = nowMs; return; }
    HandlePoll(nowMs);
    Exit();
}

void SPro40D3MailboxEngine::HandlePoll(uint64_t nowMs) noexcept {
    if (inFlight_ && nowMs >= inFlight_->deadlineMs)
        CompleteFailure(MailboxError::kTimeout);
}

// ---------------------------------------------------------------------------
// Completion (exactly-once point) and failure semantics
// ---------------------------------------------------------------------------

void SPro40D3MailboxEngine::CompleteFailure(MailboxError error) {
    InFlight cmd = std::move(*inFlight_);
    inFlight_.reset();
    // Ambiguous completion (review P1): a WRITE whose fate is unknown -
    // timed out, or failed validation at/after the notification stage -
    // may or may not have landed. Mark its targets Unknown; never
    // auto-replay. The caller decides whether to re-issue; the provenance
    // store forces a re-read before the value is trusted again.
    // NOT invalidated: kResponseStatus (the device explicitly rejected the
    // command - the previous value stands), kTransport from the initial or
    // ritual writes (the request never landed), and acceptance errors.
    switch (error) {
    case MailboxError::kTimeout:
    case MailboxError::kResponseShapeMismatch:
    case MailboxError::kMalformedResponse:
    case MailboxError::kCounterMismatch:
    case MailboxError::kShortResponse:
        InvalidateWriteTargets(cmd);
        break;
    default:
        break;
    }
    // A chained step failing after the primary write was confirmed is an
    // explicit partial completion (bus reset still reports kBusReset: it
    // invalidated everything, including possibly the applied primary).
    const MailboxError reported =
        (cmd.isChainedStep && error != MailboxError::kBusReset)
            ? MailboxError::kPartialCompletion
            : error;
    FireCompletion(cmd, reported);
}

void SPro40D3MailboxEngine::FireCompletion(InFlight& cmd, MailboxError error) noexcept {
    if (cmd.voidCb) cmd.voidCb(std::unexpected(error));
    else if (cmd.controlCb) cmd.controlCb(std::unexpected(error));
    else if (cmd.meterCb) cmd.meterCb(std::unexpected(error));
}

void SPro40D3MailboxEngine::FireSuccessVoid(InFlight& cmd) noexcept {
    if (cmd.voidCb) cmd.voidCb(std::expected<void, MailboxError>{});
}

void SPro40D3MailboxEngine::InvalidateWriteTargets(const InFlight& cmd) noexcept {
    switch (cmd.kind) {
    case Kind::kSetVolume:
        state_.mixerVolumes[cmd.volChannel][cmd.volMix].provenance = ValueProvenance::Unknown;
        break;
    case Kind::kSetControl:
        InvalidateControl(cmd.controlPos);
        break;
    case Kind::kSetRoutingLow:
    case Kind::kSetRoutingHigh:
        for (auto& slot : state_.routerOutSrc) slot.provenance = ValueProvenance::Unknown;
        for (auto& slot : state_.routerMixerSrc) slot.provenance = ValueProvenance::Unknown;
        for (auto& slot : state_.routerMeterSrc) slot.provenance = ValueProvenance::Unknown;
        break;
    default:
        break; // reads and sw-notice carry no mirrored device state
    }
}

// ---------------------------------------------------------------------------
// Counter attribution
// ---------------------------------------------------------------------------

void SPro40D3MailboxEngine::RememberCounter(uint16_t counter) noexcept {
    recentCounters_[recentCounterNext_] = counter;
    recentCounterNext_ = (recentCounterNext_ + 1) % kRecentCounterWindow;
    if (recentCounterCount_ < kRecentCounterWindow) ++recentCounterCount_;
}

bool SPro40D3MailboxEngine::CounterIsRecent(uint16_t counter) const noexcept {
    for (size_t i = 0; i < recentCounterCount_; ++i)
        if (recentCounters_[i] == counter) return true;
    return false;
}

Opcode SPro40D3MailboxEngine::OpcodeFor(Kind kind) noexcept {
    switch (kind) {
    case Kind::kSetVolume:      return kOpcodeSetVolume;
    case Kind::kGetMasterMeter:
    case Kind::kGetMixerMeter:  return kOpcodeGetMeter;
    case Kind::kGetControl:     return kOpcodeGetControl;
    case Kind::kSetControl:     return kOpcodeSetControl;
    case Kind::kSwNotice:       return kOpcodeWriteSwNotice;
    case Kind::kSetRoutingLow:
    case Kind::kSetRoutingHigh: return kOpcodeSetRouting;
    }
    return kOpcodeGetControl; // unreachable
}

uint32_t SPro40D3MailboxEngine::ExpectedResponseLength(Kind kind) noexcept {
    switch (kind) {
    case Kind::kGetControl:     return 4;
    case Kind::kGetMasterMeter: return kMasterMeterCount * 4;
    case Kind::kGetMixerMeter:  return kChannelCount * 4;
    default:                    return 0; // writes: header echo only (assumption)
    }
}

// ---------------------------------------------------------------------------
// Provenance store
// ---------------------------------------------------------------------------

void SPro40D3MailboxEngine::DecodeAndStoreControl(uint32_t position, uint32_t value,
                                                  ValueProvenance prov) noexcept {
    namespace off = control_offset;
    auto set = [](auto& slot, uint32_t v, ValueProvenance p) {
        slot = {v, p};
    };
    if (position == off::kMute) set(state_.mute, value, prov);
    else if (position == off::kDim) set(state_.dim, value, prov);
    else if (position == off::kMonitorPad) set(state_.monitorPad, value, prov);
    else if (position == off::kAdatAsSpdif) set(state_.adatAsSpdif, value, prov);
    else if (position == off::kMonitorVolume) set(state_.monitorVolume, value, prov);
    else if (position >= off::kOutputVolBase &&
             position < off::kOutputVolBase + kOutputPairCount * 4) {
        int32_t even{}, odd{};
        UnpackOutputVolumePair(value, even, odd);
        const size_t pair = (position - off::kOutputVolBase) / 4;
        state_.outputVolumes[pair * 2]     = {even, prov};
        state_.outputVolumes[pair * 2 + 1]  = {odd, prov};
    }
    // kVolHwctlBase / kHwAssign are not mirrored in the Stage 1 store.
}

void SPro40D3MailboxEngine::InvalidateControl(uint32_t position) noexcept {
    namespace off = control_offset;
    auto invalidate = [](auto& slot) { slot.provenance = ValueProvenance::Unknown; };
    if (position == off::kMute) invalidate(state_.mute);
    else if (position == off::kDim) invalidate(state_.dim);
    else if (position == off::kMonitorPad) invalidate(state_.monitorPad);
    else if (position == off::kAdatAsSpdif) invalidate(state_.adatAsSpdif);
    else if (position == off::kMonitorVolume) invalidate(state_.monitorVolume);
    else if (position >= off::kOutputVolBase &&
             position < off::kOutputVolBase + kOutputPairCount * 4) {
        const size_t pair = (position - off::kOutputVolBase) / 4;
        invalidate(state_.outputVolumes[pair * 2]);
        invalidate(state_.outputVolumes[pair * 2 + 1]);
    }
}

void SPro40D3MailboxEngine::ApplyObserved(uint32_t position, uint32_t value) noexcept {
    DecodeAndStoreControl(position, value, ValueProvenance::Observed);
}

void SPro40D3MailboxEngine::ApplyCached(uint32_t position, uint32_t value) noexcept {
    DecodeAndStoreControl(position, value, ValueProvenance::Cached);
}

} // namespace ASFW::Audio::DICE::Focusrite::SPro40D3
