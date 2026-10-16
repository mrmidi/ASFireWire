// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <optional>
#include "NativeFireWire.h"
#include "ClockPolicy.hpp"
#include "OwnershipLease.hpp"
#include "StartupNotificationTrace.hpp"
#include "ExistingClockCheck.hpp"
#include <IOKit/firewire/IOFireWireLib.h>
#include <libkern/OSByteOrder.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unistd.h>

// Shared native DICE ownership lifecycle with a verified generation-bound refill lease.
static uint32_t ownedBe32(const void *data) {
    const auto *p = static_cast<const unsigned char *>(data);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
struct Pro40OwnedSession {
    static constexpr uint64_t noOwner = 0xffff000000000000ULL;
    IOFireWireLibDeviceRef device = nullptr;
    IOFireWireLibPseudoAddressSpaceRef mailbox = nullptr;
    FILE *log;
    uint32_t generation = 0, originalSelected = 0, bits = 0, notifications = 0;
    uint64_t lastConfigurationMicros=0;
    uint16_t remote = 0;
    uint64_t global = 0, owner = 0;
    FWAddress address{};
    bool dispatcher = false, claimAttempted = false, clockAttempted = false, invalidPacket = false;
    bool quietNotifications = false;
    bool requireRestoredClock=false; // Diagnostic excursion: verify even if ownership was lost.
    // Same run-loop owner as setup/rate/finish: Apple reference
    // IOFireWireLib.CFPlugInProj/IOFireWireLibDevice.cpp:331-424 attaches the
    // notification source to the supplied loop. No isoch callback writes here.
    Pro40::StartupNotificationTrace startupTrace;
    Pro40Resources::OwnershipLease streamingLease;
    IOReturn streamGuard() {
        uint32_t current=0;uint16_t node=0;
        const auto status=pro40_state(&current,&node);
        if(!streamingLease.check(current,!status && !invalidPacket))
            return status?status:current!=generation?kIOFireWireBusReset:kIOReturnNotPermitted;
        return 0;
    }
    void event(const char *label, uint32_t value = 0) {
        std::fprintf(log, "%s: 0x%08x\n", label, value); std::fflush(log); fsync(fileno(log));
        std::printf("%s: 0x%08x\n", label, value);
    }
    static UInt32 notification(IOFireWireLibPseudoAddressSpaceRef space, FWClientCommandID command,
        UInt32 size, void *packet, UInt16 source, UInt32 hi, UInt32 lo, void *refcon) {
        auto &self = *static_cast<Pro40OwnedSession *>(refcon);
        const auto delivered = self.startupTrace.now();
        const bool valid = packet && size == 4 && (source & 63) == (self.remote & 63) &&
            hi == self.address.addressHi && lo == self.address.addressLo;
        const uint32_t received = valid ? ownedBe32(packet) : 0;
        if (valid) {
            self.bits |= received; ++self.notifications;
            if(received&3)self.lastConfigurationMicros=delivered;
        }
        else self.invalidPacket = true;
        (*space)->ClientCommandIsComplete(space, command, valid ? kFWResponseComplete : kFWResponseTypeError);
        self.startupTrace.at(delivered, valid ? Pro40::StartupNotificationTrace::Kind::notification :
            Pro40::StartupNotificationTrace::Kind::invalid, valid ? received : size);
        // Completion returns queue ownership to the library; never touch packet after it.
        if (!self.quietNotifications)
            self.event(valid ? "notification-bits" : "invalid-notification", valid ? received : size);
        return kFWResponseComplete;
    }
    static void skipped(IOFireWireLibPseudoAddressSpaceRef space, FWClientCommandID command, UInt32 count) {
        auto &self = *static_cast<Pro40OwnedSession *>((*space)->GetRefCon(space));
        const auto delivered = self.startupTrace.now();
        self.invalidPacket = true;
        (*space)->ClientCommandIsComplete(space, command, kFWResponseComplete);
        self.startupTrace.at(delivered, Pro40::StartupNotificationTrace::Kind::skipped, count);
        if (!self.quietNotifications) self.event("notification-queue-dropped", count);
    }
    IOReturn stable() {
        uint32_t current = 0; uint16_t node = 0;
        const auto status = pro40_state(&current, &node);
        return status ? status : current == generation ? 0 : kIOFireWireBusReset;
    }
    IOReturn state(Pro40ClockState &out) { return pro40_clock_state(generation, &out); }
    static uint64_t ownerOf(const Pro40ClockState &s) { return uint64_t(s.owner_high) << 32 | s.owner_low; }
    IOReturn exchange(uint64_t expected, uint64_t desired) {
        auto status = stable();
        if (status) return status;
        // The API casts these pointers to UInt64*: align them, preserve wire bytes.
        alignas(8) std::array<uint32_t, 2> before{}, after{}, found{};
        const auto a = OSSwapHostToBigInt64(expected), b = OSSwapHostToBigInt64(desired);
        std::memcpy(before.data(), &a, 8); std::memcpy(after.data(), &b, 8);
        FWAddress target{}; target.nodeID = remote;
        target.addressHi = UInt16(global >> 32); target.addressLo = UInt32(global);
        event("enter-owner-CAS");
        status = (*device)->CompareSwap64(device, 0, &target, before.data(), after.data(), found.data(), 8, true, generation);
        event("leave-owner-CAS", unsigned(status));
        return status;
    }
    IOReturn setup(std::optional<uint32_t> expectedGeneration=std::nullopt) {
        device = static_cast<IOFireWireLibDeviceRef>(pro40_native_interface());
        auto status = pro40_state(&generation, &remote);
        // Generation is queried through Apple's existing user-client method
        // (IOFireWireLibDevice.cpp:1032-1043). Reject a stale supervisor binding
        // before register reads, owner CAS, or clock/stream writes.
        if(!status && expectedGeneration && generation!=*expectedGeneration)return kIOFireWireBusReset;
        Pro40ClockState initial{};
        if (!status) status = state(initial);
        if (!status && (initial.selected != 0x20c || initial.measured != 48000 ||
            initial.status != 0x201 || initial.enabled || ownerOf(initial) != noOwner)) status = kIOReturnNotPermitted;
        if (status) return status;
        originalSelected = initial.selected;
        unsigned char table[8]{};
        status = pro40_read(generation, 0xffffe0000000ULL, 8, table);
        const uint64_t offset = uint64_t(ownedBe32(table)) * 4, size = uint64_t(ownedBe32(table + 4)) * 4;
        if (!status && (offset < 40 || size < 104 || offset + size > 65536)) status = kIOReturnBadArgument;
        if (status) return status;
        global = 0xffffe0000000ULL + offset;
        status = (*device)->AddCallbackDispatcherToRunLoop(device, CFRunLoopGetCurrent());
        if (status) return status;
        dispatcher = true;
        event("enter-mailbox-create");
        mailbox = (*device)->CreatePseudoAddressSpace(device, 4, this, 4096, nullptr,
            kFWAddressSpaceNoReadAccess, CFUUIDGetUUIDBytes(kIOFireWirePseudoAddressSpaceInterfaceID));
        if (!mailbox) return kIOReturnNoMemory;
        (*mailbox)->GetFWAddress(mailbox, &address);
        UInt16 local = 0;
        status = (*device)->GetLocalNodeIDWithGeneration(device, generation, &local);
        if (status || (address.addressLo & 3)) return status ? status : kIOReturnBadArgument;
        owner = uint64_t(local) << 48 | uint64_t(address.addressHi) << 32 | address.addressLo;
        std::fprintf(log, "notification-address=%04x:%04x:%08x owner=%016llx generation=%u\n",
            local, address.addressHi, address.addressLo, (unsigned long long)owner, generation);
        (*mailbox)->SetWriteHandler(mailbox, notification);
        (*mailbox)->SetSkippedPacketHandler(mailbox, skipped);
        if (!(*mailbox)->TurnOnNotification(mailbox)) return kIOReturnError;
        // Check again immediately before claiming. Open alone is not DICE ownership.
        Pro40ClockState current{}; status = state(current);
        if (!status && (current.enabled || ownerOf(current) != noOwner)) status = kIOReturnNotPermitted;
        if (status) return status;
        claimAttempted = true;
        status = exchange(noOwner, owner);
        if (!status) status = state(current);
        if (!status && ownerOf(current) != owner) status = kIOReturnCannotLock;
        if(!streamingLease.grant(generation,!status && !invalidPacket) && !status)status=kIOReturnNotPermitted;
        return status;
    }
    template<class Cancelled>
    IOReturn verifyExisting48k(Cancelled cancelled) {
        startupTrace.record(Pro40::StartupNotificationTrace::Kind::existingClockCheck,0x20c);
        IOReturn io=0;
        const auto result=Pro40::verifyExisting48k(owner,[&](Pro40::ExistingClockSample &out){
            Pro40ClockState current{};io=state(current);
            if(io)return false;
            out={ownerOf(current),current.enabled,current.selected,current.status,current.measured,current.capabilities};
            return true;
        },[&]{io=stable();if(!io && invalidPacket)io=kIOReturnNotPermitted;return !io;},
        cancelled,[&]{return startupTrace.now();},
        [&]{CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.02,false);});
        switch(result){
        case Pro40::ExistingClockResult::verified:
            startupTrace.record(Pro40::StartupNotificationTrace::Kind::existingClockVerified,0x20c);
            return kIOReturnSuccess;
        case Pro40::ExistingClockResult::cancelled:return kIOReturnAborted;
        case Pro40::ExistingClockResult::io:return io?io:kIOReturnError;
        case Pro40::ExistingClockResult::mismatch:return kIOReturnNotPermitted;
        case Pro40::ExistingClockResult::deadline:return kIOReturnTimeout;
        }
        return kIOReturnError;
    }
    IOReturn rate(uint32_t hz) { return rate(hz,[]{return false;}); }
    template<class Cancelled>
    IOReturn rate(uint32_t hz,Cancelled cancelled) {
        if(cancelled())return kIOReturnAborted;
        Pro40ClockState current{};
        auto status = state(current);
        if (status) return status;
        if(cancelled())return kIOReturnAborted;
        const auto i = Pro40Clock::index(hz);
        if (ownerOf(current) != owner || current.enabled || (current.selected & 255) != 12 ||
            (hz != 44100 && hz != 48000) || !(current.capabilities & (1u << i))) return kIOReturnNotPermitted;
        const auto selected = Pro40Clock::select(hz, current.selected);
        const bool unchanged = selected == current.selected;
        const uint32_t bytes = OSSwapHostToBigInt32(selected);
        FWAddress target{}; target.nodeID = remote;
        target.addressHi = UInt16((global + 0x4c) >> 32); target.addressLo = UInt32(global + 0x4c);
        bits = 0;
        const auto previousNotifications = notifications;
        UInt32 length = 4;
        status = stable(); if (status) return status;
        if(cancelled())return kIOReturnAborted;
        event("enter-owned-clock-write-Hz", hz);
        if(cancelled())return kIOReturnAborted;
        clockAttempted = true;
        startupTrace.record(Pro40::StartupNotificationTrace::Kind::clockWriteStart, selected);
        status = (*device)->Write(device, 0, &target, &bytes, &length, true, generation);
        startupTrace.record(Pro40::StartupNotificationTrace::Kind::clockWriteEnd, uint32_t(status));
        event("leave-owned-clock-write", unsigned(status));
        if (status || length != 4) return status ? status : kIOReturnUnderrun;
        const auto began = std::chrono::steady_clock::now();
        const auto deadline = began + std::chrono::seconds(3);
        unsigned agreeing = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if(cancelled())return kIOReturnAborted;
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
            if(cancelled())return kIOReturnAborted;
            status = state(current); if (status) return status;
            if(cancelled())return kIOReturnAborted;
            if (ownerOf(current) != owner || current.enabled) return kIOReturnNotPermitted;
            const bool clockOK = current.selected == selected && current.measured == hz &&
                current.status == ((i << 8) | 1);
            agreeing = clockOK ? agreeing + 1 : 0;
            const bool notified = notifications > previousNotifications && (bits & 0x20);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - began).count();
            if (Pro40Clock::accepted(unchanged, notified, invalidPacket, agreeing, uint64_t(elapsed))) {
                startupTrace.record(Pro40::StartupNotificationTrace::Kind::clockAccepted, selected);
                // Control-thread observation of existing reads only; no new transaction.
                std::fprintf(log,"clock-acceptance-detail unchanged=%u notified=%u stable-reads=%u elapsed-ms=%llu\n",
                    unsigned(unchanged),unsigned(notified),agreeing,static_cast<unsigned long long>(elapsed));
                event(notified ? "clock-and-notification-verified-Hz" : "unchanged-clock-readback-verified-Hz", hz);
                return 0;
            }
        }
        return kIOReturnTimeout;
    }
    IOReturn finish() {
        streamingLease.revoke();
        IOReturn status = 0;
        if (claimAttempted) {
            Pro40ClockState current{}; status = state(current);
            if (!status && ownerOf(current) == owner) {
                // Restore even after an interrupted/failed test. Avoid generating
                // a redundant notification when the original clock already agrees.
                if (clockAttempted && (current.selected != originalSelected || current.measured != 48000 || current.status != 0x201))
                    status = rate(48000);
                const auto released = exchange(owner, noOwner);
                event("owner-release", unsigned(released));
                if (!status) status = released;
                const auto read = state(current);
                if (!status) status = read;
                if (!status && (ownerOf(current) != noOwner || current.enabled || current.selected != originalSelected ||
                    current.measured != 48000 || current.status != 0x201)) status = kIOReturnError;
            } else if (!status && ownerOf(current) != noOwner) status = kIOReturnCannotLock;
            if(requireRestoredClock && !status) {
                // A lost owner must not turn a half-completed rate excursion
                // into a clean lease. Read only; never restore another owner.
                status=state(current);
                if(!status && (ownerOf(current)!=noOwner || current.enabled ||
                    current.selected!=originalSelected || current.measured!=48000 || current.status!=0x201))
                    status=kIOReturnError;
            }
            event("restored-idle-state", unsigned(status));
        }
        if (mailbox) {
            // Unregister the DICE destination first, then consume already queued
            // messages while both callback state and the pseudo-space still live.
            for (unsigned n = 0; n < 5; ++n) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
            (*mailbox)->TurnOffNotification(mailbox);
            (*device)->RemoveCallbackDispatcherFromRunLoop(device); dispatcher = false;
            event("enter-mailbox-release");
            (*mailbox)->Release(mailbox); mailbox = nullptr;
            event("leave-mailbox-release");
        }
        if (dispatcher) (*device)->RemoveCallbackDispatcherFromRunLoop(device);
        return status;
    }
};
