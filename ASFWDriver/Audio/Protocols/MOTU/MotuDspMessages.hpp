// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuDspMessages.hpp - where a command-DSP unit's messages land.
//
// The unit block-writes its DSP state, and then every front-panel change, to
// the address the host registers in 0x0b38/0x0b3c (MotuCommandDsp.hpp). Each
// registered unit owns one 256-byte slot in kMessageRegionStart.., inside the
// region the reference reserves for it (snd-firewire-ctl-services
// command_dsp.rs: MSG_DST_OFFSET_BEGIN 0xffffe0000000 + 0x10000000, 8 + 248
// bytes per destination), past the quadlet slots of NotificationMailbox.
//
// Lifetime and threading follow NotificationMailbox: the dispatch holds weak
// references, Deliver runs on the receive path, and the mixer state is read
// and written only under the mailbox's lock; it holds every parameter of the
// table (MotuDspParameters.hpp) the unit has reported. A message that changed the state
// is announced to control listeners as "re-read" (kind 4), outside the lock,
// from the route the mailbox holds, so nothing here points at the protocol.

#pragma once

#include "MotuCommandDsp.hpp"
#include "MotuDspParameters.hpp"
#include "MotuRegisters.hpp"
#include "../../Controls/ControlNotifications.hpp"
#include "../../../Async/Rx/LocalRequestDispatch.hpp"
#include "../../../Discovery/DeviceRouteToken.hpp"

#include <DriverKit/IOLib.h>

#include <array>
#include <memory>
#include <span>
#include <utility>

namespace ASFW::Audio::Motu::CommandDsp {

inline constexpr uint64_t kMessageRegionStart = kAsyncMessageRegionStart + 0x10000;
inline constexpr uint64_t kMessageSlotBytes = 0x100;
inline constexpr size_t kMessageSlots = 16;

class MessageMailbox final {
public:
    MessageMailbox(Discovery::DeviceRouteToken route, ModelDspPorts ports)
        : lock_(IOLockAlloc()), route_(route), state_(ports) {}
    ~MessageMailbox() { if (lock_) IOLockFree(lock_); }
    MessageMailbox(const MessageMailbox&) = delete;
    MessageMailbox& operator=(const MessageMailbox&) = delete;

    [[nodiscard]] bool Valid() const noexcept { return lock_ != nullptr; }

    /// A new route: frames in flight belong to the old one, and the unit sends
    /// its whole state again once messaging is restarted.
    void UpdateRoute(Discovery::DeviceRouteToken route) {
        IOLockLock(lock_);
        if (!(route_ == route)) {
            route_ = route;
            reassembler_ = {};
            state_ = DspState(*state_.Ports());
            ++version_;
        }
        IOLockUnlock(lock_);
    }

    struct Delivery {
        bool accepted{false};
        uint32_t commands{0};
        bool changed{false};
    };

    /// One frame from the unit. Accepted only from this unit's node in the
    /// current generation.
    Delivery Deliver(uint32_t generation, uint16_t source, std::span<const uint8_t> frame) {
        Delivery out{};
        Discovery::DeviceRouteToken announce{};
        IOLockLock(lock_);
        if (route_ && route_.generation.value == generation &&
            (route_.nodeId & 0x3f) == (source & 0x3f)) {
            out.accepted = true;
            if (auto message = reassembler_.Feed(frame)) {
                std::vector<Command> commands;
                (void)Decode(*message, commands);
                out.commands = static_cast<uint32_t>(commands.size());
                const DspState before = state_;
                for (const auto& command : commands) (void)state_.Apply(command);
                out.changed = !(state_ == before);
                if (out.changed) { ++version_; announce = route_; }
            }
        }
        IOLockUnlock(lock_);
        if (out.changed)
            Controls::ControlNotifications::Shared().Publish(
                {announce.guid, announce.routeEpoch, 0, 0, 4, 0});
        return out;
    }

    /// A value the host wrote and the unit acknowledged. The unit reports only
    /// its own (front-panel) changes, so the host records its writes itself.
    void ApplyLocal(const Command& command) {
        IOLockLock(lock_);
        if (state_.Apply(command)) ++version_;
        IOLockUnlock(lock_);
    }

    [[nodiscard]] DspState Snapshot(uint64_t* version = nullptr) const {
        IOLockLock(lock_);
        DspState copy = state_;
        if (version) *version = version_;
        IOLockUnlock(lock_);
        return copy;
    }

    [[nodiscard]] uint64_t DroppedFrames() const {
        IOLockLock(lock_);
        const auto dropped = reassembler_.DroppedFrames();
        IOLockUnlock(lock_);
        return dropped;
    }

private:
    IOLock* lock_;
    Discovery::DeviceRouteToken route_;
    MessageReassembler reassembler_{};
    DspState state_;
    uint64_t version_{0};
};

namespace Messages {
inline auto& Slots() { static std::array<std::weak_ptr<MessageMailbox>, kMessageSlots> slots; return slots; }
inline IOLock* Lock() { static IOLock* lock = IOLockAlloc(); return lock; }

[[nodiscard]] constexpr bool Owns(uint64_t offset) noexcept {
    return offset >= kMessageRegionStart && offset < kMessageRegionStart + kMessageSlots * kMessageSlotBytes;
}

/// The destination address of a new slot for `mailbox`, or 0 when none is free.
[[nodiscard]] inline uint64_t Register(const std::shared_ptr<MessageMailbox>& mailbox) {
    auto* lock = Lock();
    if (!lock || !mailbox || !mailbox->Valid()) return 0;
    IOLockLock(lock);
    for (size_t i = 0; i < kMessageSlots; ++i) if (Slots()[i].expired()) {
        Slots()[i] = mailbox;
        IOLockUnlock(lock);
        return kMessageRegionStart + i * kMessageSlotBytes;
    }
    IOLockUnlock(lock);
    return 0;
}

[[nodiscard]] inline Async::LocalRequestResult Handle(const Async::LocalRequestContext& ctx) {
    if (!Owns(ctx.destOffset)) return Async::LocalRequestResult::NotMine();
    // A block write (tCode 1) of one frame, to the start of a slot.
    if (ctx.tCode != 1 && ctx.tCode != 0) return Async::LocalRequestResult::Write(Async::ResponseCode::TypeError);
    if ((ctx.destOffset - kMessageRegionStart) % kMessageSlotBytes != 0)
        return Async::LocalRequestResult::Write(Async::ResponseCode::AddressError);
    if (ctx.writePayload.size() < kFramePrefixBytes || ctx.writePayload.size() > kMaxFrameBytes)
        return Async::LocalRequestResult::Write(Async::ResponseCode::DataError);
    auto* lock = Lock();
    if (!lock) return Async::LocalRequestResult::Write(Async::ResponseCode::AddressError);
    IOLockLock(lock);
    const auto held = Slots()[(ctx.destOffset - kMessageRegionStart) / kMessageSlotBytes].lock();
    IOLockUnlock(lock);
    if (held) (void)held->Deliver(ctx.generation, ctx.sourceID, ctx.writePayload);
    return Async::LocalRequestResult::Write(Async::ResponseCode::Complete);
}
} // namespace Messages

/// The two quadlets that tell the unit where to send its messages: the host's
/// node ID in the top 16 bits, then the 48-bit offset (command_dsp.rs
/// register_message_destination_address). MOTU's driver writes them as one
/// 8-byte block to 0x0b38, and all ones to stop (MOTUFireWireAudio 1.6 build
/// 89555, BoxWithFxDSP::SendCueMixPacketReadAddress); it also sends its
/// CueMix packets, 1 to 255 bytes, to 0xffff'0001'0000 (SetCueMixPacket).
struct MessageDestination final {
    uint32_t hi;
    uint32_t lo;
};
[[nodiscard]] constexpr MessageDestination EncodeMessageDestination(uint16_t hostNodeId,
                                                                    uint64_t offset) noexcept {
    return {(uint32_t{hostNodeId} << 16) | static_cast<uint32_t>((offset >> 32) & 0xffffU),
            static_cast<uint32_t>(offset & 0xffffffffU)};
}
[[nodiscard]] constexpr std::array<uint8_t, 8> DestinationBytes(MessageDestination d) noexcept {
    return {static_cast<uint8_t>(d.hi >> 24), static_cast<uint8_t>(d.hi >> 16), static_cast<uint8_t>(d.hi >> 8),
            static_cast<uint8_t>(d.hi), static_cast<uint8_t>(d.lo >> 24), static_cast<uint8_t>(d.lo >> 16),
            static_cast<uint8_t>(d.lo >> 8), static_cast<uint8_t>(d.lo)};
}

} // namespace ASFW::Audio::Motu::CommandDsp
