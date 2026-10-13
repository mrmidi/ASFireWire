// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "HalSilenceModel.hpp"
#include <atomic>
#include <mutex>
namespace Pro40Audio {
// Control calls serialize. IO gets a bounded, coherent local value snapshot.
// Every shared field is atomic (no plain-data seqlock races). Sequential
// consistency provides a single order across sequence and payload fields.
// A concurrent update causes a failed read, not a spin/wait. Process-local only.
class HalSilenceState {
    std::mutex control_;
    HalSilenceModel model_;
    std::atomic<uint64_t> sequence_{0},anchor_{0},seed_{0},ioHolds_{0};
    std::array<std::atomic<uint64_t>,HalSilenceModel::clientCapacity> clients_{};
    const uint32_t numer_,denom_;
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    void publish() {
        const auto s=model_.snapshot();
        sequence_.fetch_add(1);
        anchor_.store(s.anchor);seed_.store(s.seed);ioHolds_.store(s.ioHolds);
        for(size_t n=0;n<clients_.size();++n)clients_[n].store(s.clients[n]);
        sequence_.fetch_add(1);
    }
public:
    enum class Change {add,remove,start,stop};
    HalSilenceState(uint32_t numer,uint32_t denom):model_(numer,denom),numer_(numer),denom_(denom) {}
    bool change(Change change,uint32_t id,uint64_t now=0,bool *runningChanged=nullptr) {
        std::lock_guard lock(control_);
        if(runningChanged)*runningChanged=false;
        if(sequence_.load()>UINT64_MAX-2)return false;
        const bool wasRunning=model_.snapshot().running();
        bool ok=false;
        switch(change) {
            case Change::add:ok=model_.add(id);break;
            case Change::remove:ok=model_.remove(id);break;
            case Change::start:ok=model_.start(id,now);break;
            case Change::stop:ok=model_.stop(id);break;
        }
        if(ok) {
            publish();
            if(runningChanged)*runningChanged=wasRunning!=model_.snapshot().running();
        }
        return ok;
    }
    bool read(HalSilenceModel::Snapshot &out) const noexcept {
        const auto before=sequence_.load();if(before&1)return false;
        HalSilenceModel::Snapshot copy;copy.numer=numer_;copy.denom=denom_;
        copy.anchor=anchor_.load();copy.seed=seed_.load();copy.ioHolds=ioHolds_.load();
        for(size_t n=0;n<clients_.size();++n)copy.clients[n]=clients_[n].load();
        if(sequence_.load()!=before)return false;
        out=copy;return true;
    }
};
}
