// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
namespace Pro40Audio {
// Serial, hardware-free contract model; NOT production callback state.
// Device-wide IO holds are separate from registered clients. Sequoia's remote
// host can start under A, query/use B, remove A, and finally stop under B.
// Cross-checked against SDK StartIO contract and diagnostic-repeat capture.
class HalSilenceModel {
public:
    // Real HAL discovery registered 16 observers before the capture client.
    // Keep a bounded lab registry with headroom; this is not an unlimited
    // production admission policy. IO snapshots never allocate or wait.
    static constexpr size_t clientCapacity=256;
private:
    struct Client {uint32_t id=0;};
    std::array<std::optional<Client>,clientCapacity> clients_{};
    uint64_t running_=0;
    uint64_t anchor_=0,seed_=0,lastObserved_=0;
    uint32_t numer_,denom_;
    Client *find(uint32_t id) {
        for(auto &client:clients_)if(client && client->id==id)return &*client;
        return nullptr;
    }
public:
    // Installed SDK AudioServerPlugIn.h: ZeroTimeStampPeriod minimum 10923.
    // Quarter-second synthetic clock anchors; independent of IO buffer size.
    static constexpr uint32_t timestampPeriod=12000;
    static_assert(timestampPeriod>=10923);
    struct Stamp {double sample=0;uint64_t host=0,seed=0;};
    struct Snapshot {
        std::array<uint64_t,clientCapacity> clients{}; // bit32=registered; low32=ID.
        uint64_t anchor=0,seed=0,ioHolds=0;
        uint32_t numer=0,denom=0;
        bool active(uint32_t id) const {return running() && known(id);}
        bool known(uint32_t id) const {for(auto c:clients)if((c&0x100000000ULL) && uint32_t(c)==id)return true;return false;}
        bool running() const {return ioHolds!=0;}
    };
    Snapshot snapshot() const {
        Snapshot s;s.anchor=anchor_;s.seed=seed_;s.numer=numer_;s.denom=denom_;s.ioHolds=running_;
        for(size_t n=0;n<clients_.size();++n)if(clients_[n])s.clients[n]=uint64_t(clients_[n]->id)|0x100000000ULL;
        return s;
    }
    HalSilenceModel(uint32_t numer,uint32_t denom):numer_(numer),denom_(denom) {}
    bool add(uint32_t id) {
        if(find(id))return false;
        for(auto &client:clients_)if(!client){client=Client{id};return true;}
        return false;
    }
    bool remove(uint32_t id) {
        for(auto &client:clients_)if(client && client->id==id){
            client.reset();return true;
        }
        return false;
    }
    bool start(uint32_t id,uint64_t now) {
        auto *client=find(id);if(!client || !numer_ || !denom_)return false;
        if(running_==UINT64_MAX)return false;
        if(!running_) {
            if(seed_==UINT64_MAX)return false;
            anchor_=lastObserved_=now;++seed_;
        }
        ++running_;return true;
    }
    bool stop(uint32_t id) {
        auto *client=find(id);if(!client)return false;
        if(!running_)return false;
        --running_;return true;
    }
    bool active(uint32_t id) {return running_ && find(id);}
    bool known(uint32_t id) {return find(id)!=nullptr;}
    bool stamp(uint32_t id,uint64_t now,Stamp &out) {
        if(!active(id) || now<lastObserved_)return false;
        if(!timestamp(snapshot(),now,out))return false;
        lastObserved_=now;return true;
    }
    static bool timestamp(const Snapshot &s,uint64_t now,Stamp &out) {
        if(!s.numer || !s.denom || !s.running() || now<s.anchor)return false;
        // Synthetic 48kHz clock ONLY. Never use this nominal host-time clock for
        // the Saffire: its independently drifting hardware needs correlation.
        // Integer rational arithmetic avoids rounding accumulation per period.
        using Wide=__uint128_t;
        const Wide periods=Wide(now-s.anchor)*s.numer*48000/(Wide(s.denom)*1000000000*timestampPeriod);
        const Wide frames=periods*timestampPeriod;
        if(frames>(Wide(1)<<53))return false;
        const Wide offset=frames*1000000000*s.denom/(Wide(s.numer)*48000);
        if(offset>UINT64_MAX-s.anchor)return false;
        out={double(frames),s.anchor+uint64_t(offset),s.seed};return true;
    }
};
}
