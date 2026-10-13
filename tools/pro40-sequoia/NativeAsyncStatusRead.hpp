// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeFireWire.h"
#include "StagedLockPoll.hpp"
#include <IOKit/firewire/IOFireWireLib.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdlib>

// One run-loop owner, one outstanding read. Buffer/refcon never move. The
// callback only records completion; the outer loop consumes it after DMA work.
// References: Apple IOFireWireLibCommand.cpp:435-569,660-678 (submit/cancel/
// completion); IOFireWireLib.h:3104-3106 selects quadlet transactions for
// four bytes without ForceBlock (also IOFWReadCommand.cpp:130-173). Linux dice-interface.h:
// 134-168 requires separate reads, particularly read-to-clear ARX slip bits.
// CreateReadCommand uses an ordinary four-byte buffer, avoiding the legacy
// ReadQuadCmd inline-completion ABI. No reference code is copied.
class NativeAsyncStatusRead final {
    using Clock=std::chrono::steady_clock;
    IOFireWireLibCommandRef command_=nullptr;
    std::array<uint8_t,4> bytes_{};
    uint64_t global_=0;
    unsigned index_=0;
    bool pending_=false,ready_=false,cancelled_=false;
    IOReturn status_=0;
    Clock::time_point began_{};
    uint64_t completedMicros_=0;
    static void completed(void *refcon,IOReturn status) {
        auto &s=*static_cast<NativeAsyncStatusRead *>(refcon);
        if(!s.pending_)std::_Exit(3); // Unexpected duplicate native completion.
        s.status_=status;
        if(!status && (*s.command_)->GetTransferredBytes(s.command_)!=4)s.status_=kIOReturnUnderrun;
        s.completedMicros_=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-s.began_).count());
        s.pending_=false;s.ready_=true;
    }
public:
    unsigned submitted=0,completionsConsumed=0;
    uint64_t maximumSubmitMicros=0,maximumCompletionMicros=0;
    NativeAsyncStatusRead()=default;
    NativeAsyncStatusRead(const NativeAsyncStatusRead &)=delete;
    ~NativeAsyncStatusRead() {
        if(pending_)std::_Exit(3); // Caller must drain, or retain the whole owner.
        if(command_)(*command_)->Release(command_);
    }
    IOReturn prepare(IOFireWireLibDeviceRef device,uint32_t generation,uint64_t global) {
        if(command_ || !device || global<0xffffe0000000ULL || global>0xffffe000ff9cULL || global%4)return kIOReturnBadArgument;
        global_=global;
        FWAddress address{};address.addressHi=UInt16(global>>32);address.addressLo=UInt32(global+0x54);
        command_=(*device)->CreateReadCommand(device,(*device)->GetDevice(device),&address,bytes_.data(),4,
            completed,true,generation,this,CFUUIDGetUUIDBytes(kIOFireWireCommandInterfaceID));
        return command_?kIOReturnSuccess:kIOReturnNoMemory;
    }
    bool pending()const {return pending_;}
    uint64_t completionMicros()const {return completedMicros_;}
    Pro40Packets::StagedLockPoll::Result step(unsigned index,uint32_t &value) {
        using Result=Pro40Packets::StagedLockPoll::Result;
        if(!command_ || cancelled_ || index>=3)return kIOReturnBadArgument;
        if(pending_ || ready_) {
            if(index!=index_)return kIOReturnBadArgument;
            if(pending_)return Result::pending();
            ready_=false;++completionsConsumed;
            maximumCompletionMicros=std::max(maximumCompletionMicros,completedMicros_);
            if(!status_)value=uint32_t(bytes_[0])<<24|uint32_t(bytes_[1])<<16|uint32_t(bytes_[2])<<8|bytes_[3];
            return status_;
        }
        completedMicros_=0;
        const auto address=global_+0x54+index*4;
        auto status=pro40_admit_read(address,4);if(status)return status;
        FWAddress target{};target.addressHi=UInt16(address>>32);target.addressLo=UInt32(address);
        (*command_)->SetTarget(command_,&target);
        index_=index;bytes_.fill(0);began_=Clock::now();pending_=true;
        status=(*command_)->Submit(command_);
        maximumSubmitMicros=std::max(maximumSubmitMicros,uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-began_).count()));
        if(status){pending_=false;return status;}
        ++submitted;return Result::pending();
    }
    // Cancellation is not completion. Keep pumping the ORIGINAL callback loop
    // after host DMA Stop until pending()==false, before releasing this owner.
    IOReturn cancel() {
        if(cancelled_)return kIOReturnSuccess;
        cancelled_=true;
        return pending_?(*command_)->Cancel(command_,kIOReturnAborted):kIOReturnSuccess;
    }
    template<class Time,class Now,class Pump>
    bool drainUntil(Time deadline,Now now,Pump pump) {
        while(pending_ && now()<deadline)pump();
        return !pending_; // A timeout leaves the owner/buffer intact.
    }
    struct Shutdown {
        bool wasPending=false,natural=false,cancelAttempted=false,drained=false;
        IOReturn cancelStatus=0,completionStatus=0;
    };
    // Called only after DMA Stop, with no further step() calls. First drain
    // the one already-submitted read, then fall back to Cancel within the SAME
    // overall deadline. Apple Cmd::Cancel (IOFireWireLibCommand.cpp:556-568)
    // is a separate RPC from callback delivery (:660-678); IOFireWireUserClient
    // :1752-1763 can reject that RPC. Neither its success nor failure proves
    // quiescence. Preserve every cancel error if fallback was actually needed.
    template<class Time,class Now,class Pump>
    Shutdown stopAndDrain(Time naturalDeadline,Time deadline,Now now,Pump pump) {
        Shutdown result;result.wasPending=pending_;
        result.natural=drainUntil(std::min(naturalDeadline,deadline),now,pump);
        result.cancelAttempted=pending_;
        result.cancelStatus=cancel(); // Also closes admission after natural completion.
        result.drained=drainUntil(deadline,now,pump);
        result.completionStatus=pending_?kIOReturnNotReady:status_;
        return result;
    }
};
