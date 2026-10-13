// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeStartupConsumer.hpp"
#include "NativePeerSessionFinalizer.hpp"
#include <IOKit/IOReturn.h>
#include <exception>
namespace Pro40Audio {
// Single-use caller-owned composition, not an endpoint provisioner. Consumer,
// peer, mailboxes and Resources outlive this object AND any unsuccessful join.
// Resources.prepare() runs only in the runner's post-open preparation hook and
// must start the supplied peer with bounded IO and authenticated provisioning.
// It must not drive source callbacks. cancel() releases cooperative waits, while
// confirmRemote()/closeControl() run only after join. All are supervisor methods.
// Source callbacks and finish/END supervision remain serialized by the caller.
template<class Consumer,class HostClock,class Resources> class NativeConsumerBridge {
    using Startup=NativeStartupConsumer<Consumer,NativePeerSession,HostClock>;
    Consumer &consumer_;NativePeerSession &peer_;Resources &resources_;
    Startup startup_;
    struct Cleanup {
        NativeConsumerBridge &bridge;
        void cancel(){
            // END already drained is a terminal success; do not turn it into a
            // producer fault merely because the runner is closing resources.
            if(!bridge.consumer_.ended())bridge.startup_.cancel();
            bridge.resources_.cancel();
        }
        bool confirmRemote(){return bridge.resources_.confirmRemote();}
        bool closeControl(){return bridge.resources_.closeControl();}
    } cleanup_{*this};
    NativePeerSessionFinalizer<Cleanup> finalizer_;
    bool prepared_=false;
    int32_t prepare() noexcept {
        if(prepared_||!finalizer_.arm())return kIOReturnNotPermitted;
        prepared_=true; // No retry after partial resource acquisition.
        const auto status=resources_.prepare();
        if(status)return status;
        if(!peer_.joinable()||peer_.failed()||!startup_.enable())return kIOReturnNotReady;
        return 0;
    }
public:
    NativeConsumerBridge(Consumer &consumer,NativePeerSession &peer,NativeStartMailbox &mailbox,
                         SourceFrameClock::Identity id,uint64_t nonce,Resources &resources,
                         NativePeerSession::Wall::duration joinBudget)
        :consumer_(consumer),peer_(peer),resources_(resources),startup_(consumer,peer,mailbox,id,nonce),
         finalizer_(peer,cleanup_,joinBudget){}
    NativeConsumerBridge(const NativeConsumerBridge &)=delete;
    ~NativeConsumerBridge(){
        // Fail before destroying callback/admission views, not later when the
        // external peer's thread destructor happens to notice the live worker.
        if(peer_.joinable())std::terminate();
    }
    Pro40::PeerPreparation preparation() noexcept {
        return {[](void *p) noexcept{return static_cast<NativeConsumerBridge *>(p)->prepare();},this};
    }
    Pro40::PeerCleanupFinalizer finalization() noexcept {return finalizer_.hook();}
    Pro40::NativeTimingEvidence::Consumer callbacks() noexcept {return startup_.callbacks();}
    typename Startup::State state()const noexcept {return startup_.state();}
    void cancelSource() noexcept {startup_.cancel();}
};
}
