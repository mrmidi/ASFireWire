// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioWindowPublication.hpp"
namespace Pro40Audio {
// Offline native-content cursor, not a HAL callback or clock owner. Serialize all
// reader calls. Publication outlives this object and every call. The caller sets
// first to the next unread frame after handling startup samples; no implicit skip.
template<size_t Capacity> class SequentialInputReader {
    using Publication=AudioWindowPublication<Capacity>;
    using Lease=typename Publication::Lease;
public:
    enum class Result {ok,waiting,unavailable,tail,end,failed};
    enum class Fault {none,identity,overwritten,range,backward,invalidEnd,cancelled};
    SequentialInputReader(Publication &publication,uint64_t epoch,uint64_t first)
        :publication_(publication),epoch_(epoch),next_(first){if(!epoch)fail(Fault::identity);}
    SequentialInputReader(const SequentialInputReader &)=delete;
    SequentialInputReader &operator=(const SequentialInputReader &)=delete;
    uint64_t next() const noexcept {return next_;}
    Fault fault() const noexcept {return fault_;}
    std::optional<uint64_t> endFrame() const noexcept {return end_;}
    // Call only after the producer has published its final window and stopped.
    // Pin that window while draining. END cannot point beyond/before publication.
    Result finish(uint64_t end) noexcept {
        if(fault_!=Fault::none)return Result::failed;
        if(end_)return fail(Fault::invalidEnd);
        auto lease=publication_.tryAcquire();if(!lease)return Result::unavailable;
        if(!validate(*lease))return Result::failed;
        if(end!=lease->next()||end<next_)return fail(Fault::invalidEnd);
        end_=end;
        if(end==next_)return Result::end;
        final_=std::move(lease);return Result::ok;
    }
    Result read(std::span<int32_t> output) noexcept {
        std::fill(output.begin(),output.end(),0);
        if(fault_!=Fault::none)return Result::failed;
        if(output.empty()||output.size()>Capacity||output.size()>UINT64_MAX-next_)return fail(Fault::range);
        if(end_){
            if(next_==*end_)return Result::end;
            // No mixed audio/padding: request the exact remainder on tail.
            if(output.size()>*end_-next_)return Result::tail;
            const auto result=copy(*final_,output);
            if(result==Result::ok&&next_==*end_)final_.reset();
            return result;
        }
        auto lease=publication_.tryAcquire();if(!lease)return Result::unavailable;
        if(!validate(*lease))return Result::failed;
        return copy(*lease,output);
    }
    void stop() noexcept {fail(Fault::cancelled);}
private:
    Result fail(Fault why) noexcept {if(fault_==Fault::none)fault_=why;final_.reset();return Result::failed;}
    bool validate(const Lease &lease) noexcept {
        if(lease.epoch()!=epoch_){fail(Fault::identity);return false;}
        if(lease.next()<seenNext_){fail(Fault::backward);return false;}
        if(next_<lease.oldest()){fail(Fault::overwritten);return false;}
        seenNext_=lease.next();return true;
    }
    Result copy(const Lease &lease,std::span<int32_t> output) noexcept {
        const auto r=lease.read(epoch_,next_,output);
        if(r==WindowResult::notYetAvailable)return Result::waiting;
        if(r!=WindowResult::ok)return fail(r==WindowResult::stale?Fault::overwritten:Fault::range);
        next_+=output.size();return Result::ok;
    }
    Publication &publication_;const uint64_t epoch_;uint64_t next_,seenNext_=0;
    Fault fault_=Fault::none;std::optional<uint64_t> end_;
    std::optional<Lease> final_;
};
}
