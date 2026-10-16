// SPDX-License-Identifier: Apache-2.0
// Signed generated-audio client, no CoreAudio device or FireWire API calls.
#include "DuplexProvisionClient.hpp"
#include "DuplexProvisionClientPins.hpp"
#include "HalDelegatedSession.hpp"
#include "NativeDuplexPolicy.hpp"
#include "SustainedInputFixture.hpp"
#include <cstdio>
using namespace Pro40Audio;
int main(int argc,char **argv) {
    if(argc!=2 || std::strcmp(argv[1],"--run"))return 64;
    SecRequirementRef raw=nullptr;
    if(SecRequirementCreateWithString(CFSTR(PRO40_PROVISION_BROKER_REQUIREMENT),0,&raw)!=errSecSuccess)return 65;
    IdentityDetail::Owned<SecRequirementRef> broker(raw);
    mach_timebase_info_data_t tb{};if(mach_timebase_info(&tb)!=KERN_SUCCESS)return 74;
    auto controller=ProvisionClient::lookup(PRO40_PROVISION_SERVICE,broker.get());if(!controller)return 74;
    auto prepared=HalDelegatedSession::prepare(std::move(controller),{ProvisionClient::nonce(),1,tb.numer,tb.denom},nativeDuplexClockPolicy(tb.numer,tb.denom));
    if(!prepared.session){std::printf("{\"prepared\":false,\"cleanup\":%s}\n",prepared.cleanupOK?"true":"false");return 2;}
    auto &host=*prepared.session;const auto first=host.descriptor.inputFirst+37;
    uint64_t written=host.descriptor.outputFirst+host.descriptor.primedOutputFrames;
    std::array<float,137*8> input{};std::array<float,137*6> output{};
    auto due=std::chrono::steady_clock::now();unsigned blocks=0,error=0;
    for(;blocks<400;++blocks) {
        AudioServerPlugInIOCycleInfo cycle{};cycle.mInputTime.mSampleTime=double(first+blocks*137);cycle.mInputTime.mFlags=kAudioTimeStampSampleTimeValid;
        if(!host.readInput(mach_absolute_time(),cycle,137,input)){error=1;break;}
        for(unsigned f=0;f<137;++f)for(unsigned c=0;c<8;++c)
            if(input[f*8+c]!=float(Fixture::sample(first+blocks*137+f,c))/8388608.f)error=2;
        if(error)break;
        for(unsigned f=0;f<137;++f)for(unsigned c=0;c<6;++c)output[f*6+c]=float(100000*(c+1))/8388608.f;
        cycle.mOutputTime.mSampleTime=double(written+host.descriptor.outputTimeline.callbackSample);cycle.mOutputTime.mFlags=kAudioTimeStampSampleTimeValid;
        if(!host.writeOutput(mach_absolute_time(),cycle,137,output)){error=3;break;}
        written+=137;due+=std::chrono::nanoseconds(2854167);std::this_thread::sleep_until(due);
    }
    const bool clean=host.stopAndJoin();
    std::printf("{\"prepared\":true,\"callbacks\":%u,\"error\":%u,\"cleanup\":%s,\"input_channels\":8,\"output_channels\":6}\n",blocks,error,clean?"true":"false");
    return !error && blocks==400 && clean?0:2;
}
