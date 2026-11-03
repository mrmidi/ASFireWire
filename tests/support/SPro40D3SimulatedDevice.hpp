// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro40D3SimulatedDevice.hpp - Deterministic host-side TCD3070 (Saffire Pro
// 40 rev 2) DCP mailbox for Stage 1 engine tests.
//
// Follows the SimulatedDiceDevice.hpp discipline: behaviour is modelled only
// as far as the evidence goes, and each modelled behaviour cites it. Fault
// knobs let tests script every failure the engine must survive.
//
// Evidence base:
//   - The 48-byte descriptor is the REAL hardware capture
//     (documentation/evidence/stage0-tcd3070/mailbox-descriptor-raw.txt,
//     2026-11-03): magic 0x1810196604061973, owner none, version 2, max
//     request 1024, response area 400, write +0x1C0 / read +0x30.
//   - Command semantics mirror the ALSA reference behavioral facts
//     (spro40d3.rs, cited per knob); meter VALUE ENCODING is unknown on
//     hardware, so meters return synthetic documented values.
//   - tools/firmware_analysis/Pro40d3-DCP.md: unknown command categories
//     answer status 4; the transport FSM re-arms per request.
//
// This device knows nothing about the engine: it implements ISPro40D3Transport
// and pushes notifications through a NotifySink the test wires to
// SPro40D3MailboxEngine::OnNotification.
//
// REQUEST-LIFETIME TRIPWIRE (Stage 1 review, reentrant-transport finding):
// HandleRequest takes the WriteBlock span WITHOUT copying it and TOUCHES it
// again AFTER notifySink_ returns (post-notify counter/opcode re-read). If an
// engine ever frees or moves the request buffer while the transport call is
// still executing - the exact defect the pump model exists to prevent - the
// post-notify touch reads freed/poisoned memory and ASan flags it. The
// re-read also detects in-call mutation (requestMutatedDuringNotify()).

#pragma once

#include "Audio/Protocols/DICE/Focusrite/SPro40D3MailboxEngine.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace ASFW::Testing::DICE {

/// The live 48-byte descriptor, verbatim (Stage 0 capture, 2026-11-03).
inline constexpr std::array<uint8_t, 48> kSPro40D3DescriptorBytes{
    // Verbatim capture; 64-bit fields store the LOW quadlet first
    // (spro40d3.rs deserialize_address; Stage 0 capture 2026-11-03).
    0x04, 0x06, 0x19, 0x73, 0x18, 0x10, 0x19, 0x66, // +0x00 magic
    0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, // +0x08 owner: none
    0x00, 0x00, 0x00, 0x02,                          // +0x10 (2, purpose unknown)
    0x00, 0x00, 0x00, 0x02,                          // +0x14 version 2
    0x00, 0x00, 0x04, 0x00,                          // +0x18 max request 1024
    0x00, 0x00, 0x01, 0x90,                          // +0x1C response area 400
    0xE0, 0x40, 0x01, 0xC0, 0x00, 0x00, 0xFF, 0xFF,  // +0x20 write address
    0xE0, 0x40, 0x00, 0x30, 0x00, 0x00, 0xFF, 0xFF,  // +0x28 read address
};

namespace D3 = ::ASFW::Audio::DICE::Focusrite::SPro40D3;

class SPro40D3SimulatedDevice final : public D3::ISPro40D3Transport {
public:
    using NotifySink = std::function<void(std::span<const uint8_t> frame)>;

    struct Options {
        /// Deliver the response notification inside the request WriteBlock
        /// (what the real firmware does: the DCP FSM answers before the write
        /// transaction completes - Pro40d3-DCP.md transport FSM 0x1B688).
        bool notifyDuringWrite{true};
        /// Nonzero rstatus for the next N command responses, then back to 0.
        /// Models the transient errors that make the reference run its
        /// counter-reset ritual (spro40d3.rs send_command rstatus path).
        uint32_t failNextResponses{0};
        uint32_t forcedResponseStatus{4}; // status 4 = unknown category (firmware)
        /// Drop notifications for the next N commands (timeout path).
        uint32_t dropNextNotifications{0};
        /// Echo this offset added to the request counter in the response
        /// header (counter desync -> kCounterMismatch). Use an offset that
        /// cannot collide with the recent-counter window (>= 5).
        uint16_t counterEchoOffset{0};
        // -- review-added fault knobs -----------------------------------
        /// Serve these 48 bytes instead of the Stage 0 capture on descriptor
        /// reads (models a different unit / firmware revision after a bus
        /// reset -> kBadDescriptor on revalidation).
        std::optional<std::array<uint8_t, 48>> descriptorOverride{};
        /// Fail the Nth command request's WriteBlock (1-based; the counter
        /// counts command requests only - ritual reset frames excluded) with
        /// kIOReturnNotReady. The request never lands on the device.
        uint32_t failWriteBlockAtRequestIndex{0};
        /// One-shot: the NEXT notification carries this rlen instead of the
        /// real response payload length (oversized / wrong-shape ingress).
        uint16_t nextNotificationLengthOverride{0};
        /// Offset added to the response header's payloadLength ECHO only
        /// (actual payload untouched) -> kMalformedResponse.
        int16_t responseLengthEchoOffset{0};
        /// One-shot: corrupt the response header's opcode echo (byte 1
        /// flipped) -> kMalformedResponse.
        bool responseOpcodeCorrupt{false};
        /// One-shot: a concurrent host's ownership CAS lands between our
        /// descriptor read and our own CAS. The next CompareSwap64 observes
        /// this value as the current owner (their claim committed; ours
        /// fails atomically) - models the register race window.
        uint64_t ownerRaceValue{0};
    };

    explicit SPro40D3SimulatedDevice(NotifySink sink, Options options)
        : notifySink_{std::move(sink)}, options_{options} {}
    explicit SPro40D3SimulatedDevice(NotifySink sink); // defined below the class

    // -- ISPro40D3Transport ------------------------------------------------

    std::expected<void, IOReturn>
    WriteBlock(uint64_t address, std::span<const uint8_t> bytes) override {
        if (address == D3::kDcpBaseAddress + 8) return std::unexpected(kIOReturnBadArgument);
        if (address != writeAddress()) return std::unexpected(kIOReturnBadArgument);
        if (bytes.size() > kDcpMaxRequest) return std::unexpected(kIOReturnMessageTooLarge);
        if (bytes.size() < D3::kMessageHeaderSize) return std::unexpected(kIOReturnBadArgument);

        if (bytes[0] == 0x80 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 0 &&
            bytes.size() == D3::kCounterResetFrameSize) {
            // Counter-reset frame: re-arms the transport, no notification.
            sawResetFrame_ = true;
            return {};
        }
        ++requestIndex_;
        if (options_.failWriteBlockAtRequestIndex != 0 &&
            requestIndex_ == options_.failWriteBlockAtRequestIndex) {
            // Scripted transport failure: the request never lands.
            return std::unexpected(kIOReturnNotReady);
        }
        HandleRequest(bytes);
        return {};
    }

    std::expected<std::vector<uint8_t>, IOReturn>
    ReadBlock(uint64_t address, uint32_t length) override {
        if (address == D3::kDcpBaseAddress) {
            if (length != D3::kDcpDescriptorSize) return std::unexpected(kIOReturnBadArgument);
            // Honest descriptor: the owner field (+0x08) always reports the
            // CURRENT owner storage, never a static snapshot (review round
            // 3, finding A: a descriptor read decoupled from the CAS's view of
            // the owner hid the takeover hazard from the engine).
            std::array<uint8_t, 48> bytes = options_.descriptorOverride
                                                ? *options_.descriptorOverride
                                                : kSPro40D3DescriptorBytes;
            // 64-bit fields use "quadlet 0 low" word order (Stage 0 capture:
            // owner none = 00 00 00 00 FF FF 00 00 = 0xFFFF000000000000).
            ::ASFW::FW::WriteBE32(bytes.data() + 8, static_cast<uint32_t>(owner_));
            ::ASFW::FW::WriteBE32(bytes.data() + 12, static_cast<uint32_t>(owner_ >> 32));
            return std::vector<uint8_t>(bytes.begin(), bytes.end());
        }
        if (address == readAddress()) {
            if (!responseReady_) {
                ++unpairedReads_;
                return std::unexpected(kIOReturnNotReady);
            }
            responseReady_ = false; // one paired read per response (Stage 0 §7a)
            ++responseAreaReads_;
            return std::vector<uint8_t>(responseArea_.begin(),
                                        responseArea_.begin() + length);
        }
        return std::unexpected(kIOReturnBadArgument);
    }

    std::expected<uint64_t, IOReturn>
    CompareSwap64(uint64_t address, uint64_t expected, uint64_t desired) override {
        if (address != D3::kDcpBaseAddress + 8) return std::unexpected(kIOReturnBadArgument);
        ++casCalls_;
        // Race model (one-shot): a concurrent claim commits between the
        // engine's descriptor read and its CAS. Their claim wins; ours
        // fails atomically against the observed value.
        if (options_.ownerRaceValue != 0) {
            owner_ = options_.ownerRaceValue;
            options_.ownerRaceValue = 0;
            return owner_; // observed current; expected (none) does not match
        }
        const uint64_t prev = owner_;
        if (prev == expected) owner_ = desired;
        return prev;
    }

    // -- Device behaviour ---------------------------------------------------

    /// Bus reset: firmware drops the owner (TCAT re-claims every generation;
    /// SimulatedDiceDevice.hpp busResetClearsOwner rationale) and the
    /// transport re-arms.
    void BusReset() {
        owner_ = D3::kDcpOwnerNone;
        responseReady_ = false;
        pendingNotifications_.clear();
    }

    /// Test-only: (re)arm the drop-next-N-notifications knob mid-scenario.
    void ArmDropNotifications(uint32_t n) { options_.dropNextNotifications = n; }

    /// Test-only: swap the descriptor served on reads (revalidation tests).
    void SetDescriptorOverride(std::optional<std::array<uint8_t, 48>> bytes) {
        options_.descriptorOverride = std::move(bytes);
    }

    /// Test-only: force the device's owner storage to an arbitrary value -
    /// models any pre-existing claimant (TCAT, another host, or a stale
    /// value that happens to equal the engine's intended address).
    void ForceOwner(uint64_t owner) { owner_ = owner; }

    /// Test-only: arm the register race (one-shot, see ownerRaceValue).
    void ArmOwnerRace(uint64_t racer) { options_.ownerRaceValue = racer; }

    /// Test-only: write the response area out of band (models a late device
    /// re-send landing after the current command's response). The next
    /// paired read returns these bytes.
    void InjectResponse(std::span<const uint8_t> frame) {
        std::copy(frame.begin(), frame.end(), responseArea_.begin());
        responseReady_ = true;
    }

    /// Number of response-area reads NOT paired with a completed request.
    /// Invariant observer (never asserts; tests assert).
    [[nodiscard]] uint32_t UnpairedResponseReads() const noexcept { return unpairedReads_; }
    /// Total reads served from the response area (paired + would-be-unpaired).
    [[nodiscard]] uint32_t ResponseAreaReads() const noexcept { return responseAreaReads_; }
    /// Command requests accepted (ritual reset frames excluded).
    [[nodiscard]] uint32_t RequestCount() const noexcept { return requestIndex_; }
    /// CompareSwap64 calls (ownership lifecycle assertions).
    [[nodiscard]] uint32_t CasCalls() const noexcept { return casCalls_; }
    [[nodiscard]] bool SawResetFrame() const noexcept { return sawResetFrame_; }
    [[nodiscard]] uint64_t Owner() const noexcept { return owner_; }
    /// True if the request frame changed while the notification callback ran
    /// (engine-side corruption; should never happen).
    [[nodiscard]] bool RequestMutatedDuringNotify() const noexcept {
        return requestMutatedDuringNotify_;
    }
    /// Last accepted request frame (for golden-trace comparisons).
    [[nodiscard]] const std::vector<uint8_t>& LastRequest() const noexcept { return lastRequest_; }
    [[nodiscard]] const std::unordered_map<uint32_t, uint32_t>& ControlStore() const noexcept {
        return controlStore_;
    }
    [[nodiscard]] int32_t MixerVolume(uint8_t channel, uint8_t mix) const noexcept {
        return mixerVolumes_[channel][mix];
    }

    static constexpr uint64_t writeAddress() noexcept {
        return 0xFFFFE0400000ULL + 0x1C0;
    }
    static constexpr uint64_t readAddress() noexcept {
        return 0xFFFFE0400000ULL + 0x30;
    }

private:
    static constexpr uint32_t kDcpMaxRequest = 1024;
    static constexpr uint32_t kResponseAreaBytes = 400;

    /// Takes the WriteBlock span WITHOUT copying: the request-lifetime
    /// tripwire (see file header). The span is touched again AFTER
    /// notifySink_ returns.
    void HandleRequest(std::span<const uint8_t> request) {
        lastRequest_.assign(request.begin(), request.end());
        const uint16_t counter = static_cast<uint16_t>((request[4] << 8) | request[5]);
        const D3::Opcode op{{request[0], request[1], request[2], request[3]}};

        // Build the response frame: echo header (counter per knob), payload per command.
        std::vector<uint8_t> response;
        uint32_t rstatus = 0;

        if (op == D3::kOpcodeSetVolume) {
            const int32_t vol = static_cast<int16_t>((request[16] << 8) | request[17]);
            mixerVolumes_[request[18]][request[19]] = vol;
            response.assign(D3::kMessageHeaderSize, 0);
        } else if (op == D3::kOpcodeGetMeter) {
            const uint8_t count = request[17];
            const uint8_t pos = request[19];
            response.assign(D3::kMessageHeaderSize + count * 4, 0);
            // Synthetic meters (encoding unknown on hardware): value = 0x1000
            // + position*0x10 + index, documented for round-trip tests only.
            for (uint8_t i = 0; i < count; ++i)
                ::ASFW::FW::WriteBE32(response.data() + D3::kMessageHeaderSize + i * 4,
                              0x1000u + pos * 0x10u + i);
        } else if (op == D3::kOpcodeGetControl) {
            const uint32_t pos = ::ASFW::FW::ReadBE32(request.data() + 16);
            const uint32_t len = ::ASFW::FW::ReadBE32(request.data() + 20);
            response.assign(D3::kMessageHeaderSize + len, 0);
            const auto it = controlStore_.find(pos);
            ::ASFW::FW::WriteBE32(response.data() + D3::kMessageHeaderSize,
                          it != controlStore_.end() ? it->second : 0);
        } else if (op == D3::kOpcodeSetControl) {
            const uint32_t pos = ::ASFW::FW::ReadBE32(request.data() + 16);
            const uint32_t len = ::ASFW::FW::ReadBE32(request.data() + 20);
            if (len == 4) controlStore_[pos] = ::ASFW::FW::ReadBE32(request.data() + 24);
            response.assign(D3::kMessageHeaderSize, 0);
        } else if (op == D3::kOpcodeWriteSwNotice) {
            response.assign(D3::kMessageHeaderSize, 0);
        } else if (op == D3::kOpcodeSetRouting) {
            // Accept either table; store the raw frame for inspection.
            lastRoutingFrame_.assign(request.begin(), request.end());
            response.assign(D3::kMessageHeaderSize, 0);
        } else {
            rstatus = options_.forcedResponseStatus; // unknown category (firmware: 4)
            response.assign(D3::kMessageHeaderSize, 0);
        }

        // Response header: echo opcode + counter (+ knob offset), payload length.
        response[0] = op[0]; response[1] = op[1]; response[2] = op[2]; response[3] = op[3];
        const uint16_t echo = static_cast<uint16_t>(counter + options_.counterEchoOffset);
        response[4] = static_cast<uint8_t>(echo >> 8);
        response[5] = static_cast<uint8_t>(echo);
        // The TRUE payload length drives the notification; the header echo
        // is where the fault knobs diverge from it (a device that echoes an
        // inconsistent header must not also corrupt the notification).
        const uint16_t truePlen =
            static_cast<uint16_t>(response.size() - D3::kMessageHeaderSize);
        response[6] = static_cast<uint8_t>(truePlen >> 8);
        response[7] = static_cast<uint8_t>(truePlen);
        if (options_.responseLengthEchoOffset != 0) {
            const uint16_t plen = static_cast<uint16_t>(
                static_cast<int32_t>(truePlen) + options_.responseLengthEchoOffset);
            response[6] = static_cast<uint8_t>(plen >> 8);
            response[7] = static_cast<uint8_t>(plen);
        }
        if (options_.responseOpcodeCorrupt) {
            options_.responseOpcodeCorrupt = false; // one-shot
            response[1] = static_cast<uint8_t>(response[1] ^ 0xFF);
        }

        if (options_.failNextResponses > 0) {
            --options_.failNextResponses;
            rstatus = options_.forcedResponseStatus;
        }

        std::copy(response.begin(), response.end(), responseArea_.begin());
        responseReady_ = true;

        if (options_.dropNextNotifications > 0) {
            --options_.dropNextNotifications;
            return; // request lands, response sits unread, no notification
        }

        // Notification frame: [6..8] = response payload length, [8..12] = rstatus
        // (spro40d3.rs resp.connect_requested).
        std::array<uint8_t, 12> notif{};
        notif[6] = static_cast<uint8_t>(truePlen >> 8);
        notif[7] = static_cast<uint8_t>(truePlen);
        if (options_.nextNotificationLengthOverride != 0) {
            // One-shot adversarial rlen (oversized / wrong-shape ingress).
            const uint16_t rlen = options_.nextNotificationLengthOverride;
            options_.nextNotificationLengthOverride = 0;
            notif[6] = static_cast<uint8_t>(rlen >> 8);
            notif[7] = static_cast<uint8_t>(rlen);
        }
        ::ASFW::FW::WriteBE32(notif.data() + 8, rstatus);
        if (options_.notifyDuringWrite) {
            notifySink_(notif);
            // REQUEST-LIFETIME TRIPWIRE: the notification callback has run
            // (engine code executed re-entrantly inside this WriteBlock).
            // Touch the request span again: if the engine freed/moved the
            // buffer during the callback, this reads poisoned memory (ASan).
            const uint16_t postCounter =
                static_cast<uint16_t>((request[4] << 8) | request[5]);
            const bool unchanged =
                postCounter == counter && request[0] == op[0] && request[1] == op[1] &&
                request[2] == op[2] && request[3] == op[3];
            requestMutatedDuringNotify_ = requestMutatedDuringNotify_ || !unchanged;
        } else {
            pendingNotifications_.push_back(notif);
        }
    }

public:
    /// For notifyDuringWrite=false rigs: deliver queued notifications.
    void DeliverPendingNotifications() {
        auto queued = std::move(pendingNotifications_);
        pendingNotifications_.clear();
        for (auto& n : queued) notifySink_(n);
    }

private:
    NotifySink notifySink_;
    Options options_;
    uint64_t owner_{D3::kDcpOwnerNone};
    std::array<uint8_t, kResponseAreaBytes> responseArea_{};
    bool responseReady_{false};
    bool sawResetFrame_{false};
    bool requestMutatedDuringNotify_{false};
    uint32_t unpairedReads_{0};
    uint32_t responseAreaReads_{0};
    uint32_t requestIndex_{0};
    uint32_t casCalls_{0};
    std::deque<std::array<uint8_t, 12>> pendingNotifications_;
    std::vector<uint8_t> lastRequest_;
    std::vector<uint8_t> lastRoutingFrame_;
    std::unordered_map<uint32_t, uint32_t> controlStore_{};
    std::array<std::array<int32_t, 16>, 18> mixerVolumes_{};
};

// Defined out of line: an in-class `Options options = {}` default argument
// would need Options' default member initializers before the class is
// complete.
inline SPro40D3SimulatedDevice::SPro40D3SimulatedDevice(NotifySink sink)
    : SPro40D3SimulatedDevice(std::move(sink), Options{}) {}

} // namespace ASFW::Testing::DICE
