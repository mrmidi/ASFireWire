// SPDX-License-Identifier: Apache-2.0
#define main keepActiveEntry
#include "KeepActive.cpp"
#undef main
#include <array>
#include <cassert>
int main() {
    std::array<float,6*512+2> samples;
    samples.fill(.5f);
    AudioBufferList out{1,{{6,6*512*sizeof(float),samples.data()+1}}};
    AudioBufferList in{1,{{8,8*512*sizeof(float),nullptr}}};
    assert(!io(0,nullptr,&in,nullptr,&out,nullptr,nullptr));
    assert(callbacks==1 && failures==0);
    assert(samples.front()==.5f && samples.back()==.5f);
    for(unsigned i=1;i<samples.size()-1;++i)assert(samples[i]==0.f);
    // Sequoia supplies a microphone pointer despite disabled input usage.
    // Ignore it entirely and keep output silent.
    float microphone=.75f;in.mBuffers[0].mData=&microphone;samples[1]=.9f;
    io(0,nullptr,&in,nullptr,&out,nullptr,nullptr);
    assert(failures==0 && microphone==.75f && samples[1]==0.f);
    in.mBuffers[0].mData=nullptr;
    for(unsigned bytes:{0u,1u,6*4*4096+24u}) {
        out.mBuffers[0].mDataByteSize=bytes;samples[1]=.5f;
        io(0,nullptr,&in,nullptr,&out,nullptr,nullptr);
        assert(samples[1]==.5f && samples.front()==.5f && samples.back()==.5f);
    }
    io(0,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr);
    assert(callbacks==6 && failures==4);
    std::puts("PASS silent callback bounds/canaries, disabled inputs, malformed output and unchanged microphone data; no audio device opened");
}
