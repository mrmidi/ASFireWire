// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <optional>
#include <array>
namespace NativePackets {
// Append records in execution order; stream ordinals need not alternate.
template<class Sample> struct ReceiveHandoffRecord {
    unsigned stream=0,ordinal=0;
    Sample sample{};
};
// Fixed state, no per-handoff allocation. The archive is diagnostic evidence;
// this policy needs only the count of records already copied out of DMA.
template<unsigned Bank=1024,unsigned Records=9216> class ReceiveBankHandoffs {
    static_assert(Bank%64==0 && Records%Bank==0 && Records>=2*Bank);
    unsigned completed_=0;
    bool pending_=false,fault_=false,stoppedUnpublished_=false;
public:
    static constexpr unsigned count=Records/Bank-2;
    static constexpr bool handoffPending(unsigned ordinal,unsigned progress) {
        return ordinal<count && progress>=(ordinal+1)*Bank;
    }
    static constexpr bool pollingMustWait(const std::array<unsigned,2> &ordinals,
                                          unsigned first,unsigned second) {
        return handoffPending(ordinals[0],first) || handoffPending(ordinals[1],second);
    }
    struct Plan {unsigned ordinal,copied,cutBank,liveBank,stopOffset;};
    bool fault() const {return fault_;}
    unsigned completed() const {return completed_;}
    // No new links after this snapshot: the existing acyclic graph ends here.
    // Never infer an endpoint while a Notify sequence is pending or failed.
    std::optional<unsigned> terminalProgress() const {
        if(fault_ || pending_)return {};
        return (completed_+2)*Bank;
    }
    std::optional<Plan> begin(unsigned copied) {
        if(fault_ || stoppedUnpublished_)return {};
        if(pending_){fault_=true;return {};}
        if(completed_==count)return {};
        const auto required=(completed_+1)*Bank;
        if(copied<required)return {};
        if(copied>=(completed_+2)*Bank-64){fault_=true;return {};}
        pending_=true;
        return Plan{completed_,required,completed_%2,1-completed_%2,(completed_+2)*Bank-1};
    }
    bool finish(bool success) {
        if(fault_ || !pending_ || !success){fault_=true;return false;}
        pending_=false;++completed_;return true;
    }
    // Only before ANY payload/range/branch mutation or native Notify. An
    // explicit stop abandons preparation, leaving the published graph intact.
    // Terminal draining is allowed; this owner can never prepare another bank.
    // This cannot clear an uncertain publication or an earlier fault.
    bool stopBeforePublication() {
        if(fault_ || !pending_ || stoppedUnpublished_){fault_=true;return false;}
        pending_=false;stoppedUnpublished_=true;return true;
    }
};
}
