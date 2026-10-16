// SPDX-License-Identifier: Apache-2.0
#pragma once
#if !defined(PRO40_HAL_SOURCE_LOSS_FIXTURE) || !defined(PRO40_HAL_SOURCE_LOSS) || !defined(PRO40_HAL_DUPLEX_AUDIO)
#error This nominal-clock fixture is only for the isolated source-loss host test.
#endif
#if defined(PRO40_HAL_DELEGATED_AUDIO) || defined(PRO40_HAL_SYNTHETIC_AUDIO) || defined(PRO40_HAL_DUPLEX_TIMING)
#error Source-loss fixture cannot be combined with another audio profile.
#endif
#include "HalDuplexDriverState.hpp"
namespace Pro40Audio {
// No hardware, IPC or worker. It supplies silent nominal-clock audio for two
// seconds, then ends independently of the application. Tests only the real
// CoreAudio configuration contract; Mach/native cleanup is tested separately.
class HalSourceLossFixture final:public HalDuplexSession {
    const uint64_t deadline_;
    mutable std::atomic<bool> stopped_{false};
public:
    explicit HalSourceLossFixture(const NativeDuplexDescriptor &d):HalDuplexSession(d),
        deadline_(d.clock.origin+uint64_t(__uint128_t(2000000000)*d.clock.identity.denom/d.clock.identity.numer)){}
    static HalDuplexPreparation prepare(uint64_t seed,uint32_t numer,uint32_t denom) {
        NativeDuplexDescriptor d{};d.inputChannels=8;d.outputChannels=6;
        d.clock.identity={seed,seed,1,numer,denom};d.clock.origin=mach_absolute_time();
        return {std::make_unique<HalSourceLossFixture>(d),true};
    }
    bool ended()const noexcept override{return stopped_.load() || mach_absolute_time()>=deadline_;}
    bool ready(uint64_t)const noexcept override{return !ended();}
    void requestStop()const noexcept override{stopped_=true;}
    bool stopAndJoin()override{requestStop();return true;}
    bool timestamp(uint64_t now,SourceFrameClock::Stamp &out)const noexcept override {
        HalSilenceModel::Snapshot s;s.ioHolds=1;s.anchor=descriptor.clock.origin;s.seed=descriptor.clock.identity.seed;
        s.numer=descriptor.clock.identity.numer;s.denom=descriptor.clock.identity.denom;
        HalSilenceModel::Stamp stamp;
        if(ended() || !HalSilenceModel::timestamp(s,now,stamp))return false;
        out={stamp.sample,stamp.host,stamp.seed};return true;
    }
    bool readInput(uint64_t,const AudioServerPlugInIOCycleInfo &,UInt32,std::span<float> out)const noexcept override {
        std::fill(out.begin(),out.end(),0.f);return !ended();
    }
    bool writeOutput(uint64_t,const AudioServerPlugInIOCycleInfo &,UInt32,std::span<const float>)const noexcept override{return !ended();}
};
}
