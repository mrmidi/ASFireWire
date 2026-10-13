// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <mach/mach.h>
#include <array>
#include <span>
#include <utility>

namespace Pro40Audio {
// Single control-thread ownership; no concurrent close/IO. Adopt exactly one
// owned reference, never a borrowed name. No authentication or protocol policy.
class NativeMachPort {
    mach_port_t name_=MACH_PORT_NULL;
    mach_port_right_t kind_;
public:
    explicit NativeMachPort(mach_port_right_t kind):kind_(kind){}
    ~NativeMachPort(){(void)close();}
    NativeMachPort(const NativeMachPort &)=delete;
    NativeMachPort &operator=(const NativeMachPort &)=delete;
    NativeMachPort(NativeMachPort &&other)noexcept:
        name_(std::exchange(other.name_,MACH_PORT_NULL)),kind_(other.kind_){}
    NativeMachPort &operator=(NativeMachPort &&)=delete;
    mach_port_t get()const noexcept{return name_;}
    // Transfer this one owned reference; the new owner must adopt the same kind.
    [[nodiscard]] mach_port_t release()noexcept{return std::exchange(name_,MACH_PORT_NULL);}
    [[nodiscard]] kern_return_t adopt(mach_port_t name)noexcept{
        if(MACH_PORT_VALID(name_) || !MACH_PORT_VALID(name) ||
           (kind_!=MACH_PORT_RIGHT_SEND && kind_!=MACH_PORT_RIGHT_RECEIVE))return KERN_INVALID_ARGUMENT;
        name_=name;return KERN_SUCCESS;
    }
    [[nodiscard]] kern_return_t allocate()noexcept{
        if(kind_!=MACH_PORT_RIGHT_RECEIVE || MACH_PORT_VALID(name_))return KERN_INVALID_ARGUMENT;
        return mach_port_allocate(mach_task_self(),MACH_PORT_RIGHT_RECEIVE,&name_);
    }
    // Retain a borrowed SEND name while its original owner is still alive.
    // Publish ownership only after the kernel grants the extra user reference.
    [[nodiscard]] kern_return_t retainSend(mach_port_t borrowed)noexcept{
        if(kind_!=MACH_PORT_RIGHT_SEND || MACH_PORT_VALID(name_) || !MACH_PORT_VALID(borrowed))
            return KERN_INVALID_ARGUMENT;
        const auto result=mach_port_mod_refs(mach_task_self(),borrowed,MACH_PORT_RIGHT_SEND,1);
        if(result==KERN_SUCCESS)name_=borrowed;
        return result;
    }
    [[nodiscard]] kern_return_t close()noexcept{
        if(!MACH_PORT_VALID(name_))return KERN_SUCCESS;
        // deallocate handles a send reference that became a dead name; a fixed
        // MACH_PORT_RIGHT_SEND mod_refs would fail after the receiver dies.
        const auto result=kind_==MACH_PORT_RIGHT_SEND?mach_port_deallocate(mach_task_self(),name_):
            mach_port_mod_refs(mach_task_self(),name_,MACH_PORT_RIGHT_RECEIVE,-1);
        if(result==KERN_SUCCESS)name_=MACH_PORT_NULL;
        return result; // On error preserve ownership for explicit diagnosis/retry.
    }
};

// One receive attempt and immutable borrowed bytes; cannot accidentally duplicate
// descriptor/reply-right ownership. Owner must outlive every view. Destruction
// handles only successful kernel receives, never caller-fabricated byte buffers.
template<size_t Capacity=256> class NativeMachReceived {
    static_assert(Capacity>=sizeof(mach_msg_header_t)+sizeof(mach_msg_max_trailer_t));
    static_assert(Capacity<=UINT32_MAX);
    alignas(mach_msg_header_t) std::array<std::byte,Capacity> bytes_{};
    bool attempted_=false,received_=false;
    kern_return_t status_=KERN_SUCCESS;
public:
    NativeMachReceived()=default;
    NativeMachReceived(const NativeMachReceived &)=delete;
    NativeMachReceived &operator=(const NativeMachReceived &)=delete;
    ~NativeMachReceived(){if(received_)mach_msg_destroy(reinterpret_cast<mach_msg_header_t *>(bytes_.data()));}
    [[nodiscard]] bool read(mach_port_t port,mach_msg_timeout_t timeout,mach_msg_option_t trailerOptions=0)noexcept{
        return receive(port,timeout,trailerOptions,false);
    }
    // One zero-wait receive; MACH_RCV_TIMEOUT with timeout 0 never waits for a
    // future message. Use a fresh owner for each poll, preserving right ownership.
    [[nodiscard]] bool poll(mach_port_t port,mach_msg_option_t trailerOptions=0)noexcept{
        return receive(port,0,trailerOptions,true);
    }
private:
    bool receive(mach_port_t port,mach_msg_timeout_t timeout,mach_msg_option_t trailerOptions,bool zeroWait)noexcept{
        if(attempted_)return false; // Existing bytes/rights/status remain intact.
        attempted_=true;
        if(!MACH_PORT_VALID(port) || (!timeout && !zeroWait) || (trailerOptions&~(MACH_RCV_TRAILER_TYPE(15)|MACH_RCV_TRAILER_ELEMENTS(15)))){
            status_=KERN_INVALID_ARGUMENT;return false;
        }
        status_=mach_msg(reinterpret_cast<mach_msg_header_t *>(bytes_.data()),
            MACH_RCV_MSG|MACH_RCV_TIMEOUT|trailerOptions,0,bytes_.size(),port,timeout,MACH_PORT_NULL);
        received_=status_==KERN_SUCCESS;return received_;
    }
public:
    bool received()const noexcept{return received_;}
    kern_return_t status()const noexcept{return status_;}
    std::span<const std::byte> bytes()const noexcept{
        return received_?std::span<const std::byte>(bytes_):std::span<const std::byte>{};
    }
};
}
