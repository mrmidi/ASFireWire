// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <span>
namespace Pro40Audio {
enum class StartupSampleComparison {unavailable,invalidRecording,mismatch,match};
// Post-run only. Compare retained signed samples by encoding them to PCM24 bytes,
// independently of the adapter's byte-to-sample conversion. No PCM is exported.
inline StartupSampleComparison compareStartupSamples(std::span<const int32_t> samples,
                                                     std::span<const uint8_t> pcm,bool valid) noexcept {
    if(!valid||samples.size()!=512||pcm.size()<1536||pcm.size()%3)return StartupSampleComparison::invalidRecording;
    for(size_t n=0;n<samples.size();++n){
        if(samples[n]<-8388608||samples[n]>8388607)return StartupSampleComparison::mismatch;
        const auto bits=static_cast<uint32_t>(samples[n]);
        for(size_t byte=0;byte<3;++byte)
            if(pcm[n*3+byte]!=uint8_t(bits>>(byte*8)))return StartupSampleComparison::mismatch;
    }
    return StartupSampleComparison::match;
}
}
