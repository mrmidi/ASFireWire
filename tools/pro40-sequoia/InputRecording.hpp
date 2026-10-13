// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <memory>
#include <new>
#include <span>
namespace Pro40Packets {
// Content-side bounded storage, allocated and touched before native setup.
// Worst case eight frames per cycle in the largest supported finite session.
// Sample position/byte order checked against references/linux-dice/
// amdtp-am824.c:179-207 and the retained Pro40 microphone capture.
class InputRecording {
    const unsigned channels_;
    const size_t capacity_;
    std::unique_ptr<uint8_t[]> bytes_;
    size_t used_=0;
    bool fault_=false;
public:
    explicit InputRecording(unsigned channels=1)
        : channels_(channels),capacity_((channels==1 || channels==8)?262144*8*3*size_t(channels):0),
          bytes_(capacity_?new(std::nothrow) uint8_t[capacity_]():nullptr) {}
    bool valid() const {return bool(bytes_);}
    bool fault() const {return fault_;}
    unsigned channels() const {return channels_;}
    size_t frames() const {return valid()?used_/(channels_*3):0;}
    std::span<const uint8_t> pcm() const {return {bytes_.get(),used_};}
    void observe(std::span<const uint8_t> payload) noexcept {
        if(fault_)return;
        if(!valid() || (payload.size()!=8 && payload.size()!=360)) {fault_=true;return;}
        const size_t needed=(payload.size()-8)/44*3*channels_;
        if(needed>capacity_-used_){fault_=true;return;}
        // Validate the complete packet before appending any samples.
        for(size_t offset=8;offset<payload.size();offset+=44)
            for(unsigned channel=0;channel<channels_;++channel)
                if(payload[offset+channel*4]!=0x40){fault_=true;return;}
        for(size_t offset=8;offset<payload.size();offset+=44)for(unsigned channel=0;channel<channels_;++channel) {
            const auto at=offset+channel*4;
            bytes_[used_++]=payload[at+3];
            bytes_[used_++]=payload[at+2];
            bytes_[used_++]=payload[at+1];
        }
    }
};
}
