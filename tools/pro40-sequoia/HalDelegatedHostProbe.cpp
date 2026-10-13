// SPDX-License-Identifier: Apache-2.0
// CoreAudio client only. Explicit hardware build reads real microphones and
// writes silence; ordinary build validates generated8/6 markers. No native APIs.
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include "HalSilenceProperties.hpp"
#include "SustainedInputFixture.hpp"
namespace {
AudioObjectPropertyAddress address(UInt32 selector,UInt32 scope=kAudioObjectPropertyScopeGlobal) {
    return {selector,scope,kAudioObjectPropertyElementMain};
}
template<class T> bool read(AudioObjectID id,AudioObjectPropertyAddress a,T &v) {
    UInt32 size=sizeof(v);const auto s=AudioObjectGetPropertyData(id,&a,0,nullptr,&size,&v);
    if(s)std::fprintf(stderr,"property %08x status=%d\n",a.mSelector,int(s));
    return !s && size==sizeof(v);
}
bool defaults() {
    for(auto selector:{kAudioHardwarePropertyDefaultInputDevice,kAudioHardwarePropertyDefaultOutputDevice,kAudioHardwarePropertyDefaultSystemOutputDevice}) {
        AudioObjectID id=0;if(!read(kAudioObjectSystemObject,address(selector),id))return false;
        CFStringRef uid=nullptr;std::array<char,1024> text{};
        if(id && (!read(id,address(kAudioDevicePropertyDeviceUID),uid) || !uid))return false;
        const bool ok=!id || CFStringGetCString(uid,text.data(),text.size(),kCFStringEncodingUTF8);
        if(uid)CFRelease(uid);if(!ok)return false;
        std::printf("default %08x %s\n",selector,id?text.data():"<none>");
    }
    return true;
}
bool find(AudioObjectID &id) {
    // SDK AudioHardware.h:481-487: CFString qualifier, AudioObjectID result.
    CFStringRef uid=Pro40Audio::HalSilenceProperties::deviceUID();
    auto a=address(kAudioHardwarePropertyTranslateUIDToDevice);UInt32 size=sizeof(id);
    const auto status=AudioObjectGetPropertyData(kAudioObjectSystemObject,&a,sizeof(uid),&uid,&size,&id);
    if(status)std::fprintf(stderr,"translate UID status=%d\n",int(status));
    return !status && size==sizeof(id);
}
bool inspect(AudioObjectID id) {
    Float64 rate=0;if(!read(id,address(kAudioDevicePropertyNominalSampleRate),rate) || rate!=48000)return false;
    for(auto scope:{kAudioObjectPropertyScopeInput,kAudioObjectPropertyScopeOutput}) {
        AudioObjectID stream=0;AudioStreamBasicDescription f{};
        if(!read(id,address(kAudioDevicePropertyStreams,scope),stream) ||
           !read(stream,address(kAudioStreamPropertyVirtualFormat),f))return false;
        const UInt32 channels=scope==kAudioObjectPropertyScopeInput?8:6;
        if(f.mSampleRate!=48000 || f.mChannelsPerFrame!=channels || f.mBytesPerFrame!=4*channels ||
           f.mFramesPerPacket!=1 || f.mBytesPerPacket!=4*channels || f.mBitsPerChannel!=32 ||
           f.mFormatID!=kAudioFormatLinearPCM || f.mFormatFlags!=kAudioFormatFlagsNativeFloatPacked)return false;
        UInt32 latency=99,safety=99;
        const auto expectedSafety=scope==kAudioObjectPropertyScopeInput?
            Pro40Audio::HalSilenceProperties::inputSafetyOffset:Pro40Audio::HalSilenceProperties::outputSafetyOffset;
        if(!read(id,address(kAudioDevicePropertyLatency,scope),latency) ||
           !read(id,address(kAudioDevicePropertySafetyOffset,scope),safety) || latency || safety!=expectedSafety)return false;
    }
#ifdef PRO40_SESSION_PROFILE
    std::array<UInt32,3> layout{};std::array<UInt32,2> stereo{};
    if(!read(id,address(kAudioDevicePropertyPreferredChannelLayout,kAudioObjectPropertyScopeOutput),layout) ||
       layout!=std::array<UInt32,3>{kAudioChannelLayoutTag_MPEG_5_1_A,0,0} ||
       !read(id,address(kAudioDevicePropertyPreferredChannelsForStereo,kAudioObjectPropertyScopeOutput),stereo) ||
       stereo!=std::array<UInt32,2>{1,2})return false;
    std::puts("PASS: CoreAudio preferred output layout L R C LFE LS RS; stereo 1 2");
#endif
    std::printf("PASS: delegated discovery, 8/6 Float32 48kHz, zero latency, safety input=%u output=%u\n",
        Pro40Audio::HalSilenceProperties::inputSafetyOffset,Pro40Audio::HalSilenceProperties::outputSafetyOffset);return true;
}
struct Progress {
    uint64_t frames=0;double next=0;UInt64 lastHost=0;bool have=false;
    bool accept(const AudioTimeStamp *t,UInt32 n) {
        constexpr UInt32 flags=kAudioTimeStampSampleTimeValid|kAudioTimeStampHostTimeValid;
        if(!t || (t->mFlags&flags)!=flags || !std::isfinite(t->mSampleTime) || std::trunc(t->mSampleTime)!=t->mSampleTime)return false;
        if(have && (t->mSampleTime!=next || t->mHostTime<=lastHost))return false;
        have=true;next=t->mSampleTime+n;lastHost=t->mHostTime;frames+=n;return true;
    }
};
struct State {
    std::atomic_flag busy=ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> calls{0},failures{0};
    Progress input,output;
    std::array<uint64_t,8> nonzero{};
    std::array<float,8> peaks{};
};
State state; // Lives until process exit even if host shutdown fails.
#ifdef PRO40_HAL_SOURCE_LOSS_FIXTURE
std::atomic<unsigned> aliveNotices{0};
OSStatus aliveChanged(AudioObjectID,UInt32,const AudioObjectPropertyAddress *,void *) {++aliveNotices;return noErr;}
std::atomic<unsigned> deviceListNotices{0};
OSStatus devicesChanged(AudioObjectID,UInt32,const AudioObjectPropertyAddress *,void *) {++deviceListNotices;return noErr;}
#endif
bool buffer(const AudioBufferList *list,UInt32 channels,UInt32 &frames) {
    if(!list || list->mNumberBuffers!=1)return false;
    const auto &b=list->mBuffers[0];
    if(b.mNumberChannels!=channels || !b.mData || !b.mDataByteSize ||
       b.mDataByteSize%(sizeof(float)*channels) || b.mDataByteSize>4096*sizeof(float)*channels)return false;
    frames=b.mDataByteSize/(sizeof(float)*channels);return true;
}
OSStatus io(AudioObjectID,const AudioTimeStamp *,const AudioBufferList *input,const AudioTimeStamp *inputTime,
            AudioBufferList *output,const AudioTimeStamp *outputTime,void *) {
    UInt32 inFrames=0,outFrames=0;
    const bool outOK=buffer(output,6,outFrames);
    // Silence on every output callback, including an unexpected overlap.
    if(outOK)std::fill_n(static_cast<float *>(output->mBuffers[0].mData),outFrames*6,0.f);
    if(state.busy.test_and_set(std::memory_order_acquire)){++state.failures;return noErr;}
    bool ok=buffer(input,8,inFrames) && outOK;
    if(ok) {
        ok=state.input.accept(inputTime,inFrames) && state.output.accept(outputTime,outFrames);
        if(ok && inputTime->mSampleTime>=0) {
            const auto first=uint64_t(inputTime->mSampleTime);
            const auto *samples=static_cast<const float *>(input->mBuffers[0].mData);
            for(unsigned f=0;f<inFrames;++f)for(unsigned c=0;c<8;++c) {
#if defined(PRO40_HAL_DELEGATED_HARDWARE) || defined(PRO40_HAL_SOURCE_LOSS_FIXTURE)
                (void)first;
                const float value=samples[f*8+c];
                if(!std::isfinite(value) || std::abs(value)>1.f)ok=false;
                if(value!=0)++state.nonzero[c];
                state.peaks[c]=std::max(state.peaks[c],std::abs(value));
#else
                if(samples[f*8+c]!=float(Pro40Audio::Fixture::sample(first+f,c))/8388608.f)ok=false;
#endif
            }
        }else ok=false;
#if !defined(PRO40_HAL_DELEGATED_HARDWARE) && !defined(PRO40_HAL_SOURCE_LOSS_FIXTURE)
        if(ok)for(unsigned f=0;f<outFrames;++f)for(unsigned c=0;c<6;++c)
            static_cast<float *>(output->mBuffers[0].mData)[f*6+c]=float(100000*(c+1))/8388608.f;
#endif
    }
    ++state.calls;if(!ok)++state.failures;
    state.busy.clear(std::memory_order_release);return noErr;
}
}
int main(int argc,char **argv) {
    if(argc!=2)return 64;
#ifdef PRO40_SESSION_PROFILE
    // Explicit installer upgrade restoration only; ordinary inspection and
    // capture never change the user's default-device selections.
    UInt32 selection=0;
    if(!std::strcmp(argv[1],"--select-default-input"))selection=kAudioHardwarePropertyDefaultInputDevice;
    if(!std::strcmp(argv[1],"--select-default-output"))selection=kAudioHardwarePropertyDefaultOutputDevice;
    if(!std::strcmp(argv[1],"--select-default-system"))selection=kAudioHardwarePropertyDefaultSystemOutputDevice;
    if(selection) {
        AudioObjectID id=0;if(!find(id) || !id || !inspect(id))return 1;
        auto a=address(selection);
        return AudioObjectSetPropertyData(kAudioObjectSystemObject,&a,0,nullptr,sizeof(id),&id) || !defaults()?1:0;
    }
#endif
    const bool endurance=std::strcmp(argv[1],"--capture-90")==0;
    const bool longEndurance=std::strcmp(argv[1],"--capture-600")==0;
    const bool startup=std::strcmp(argv[1],"--capture-startup")==0;
    const bool session=std::strcmp(argv[1],"--capture-session")==0;
#ifndef PRO40_SESSION_PROFILE
    if(session)return 64;
#endif
#ifndef PRO40_ENDURANCE_TRIAL
    if(longEndurance)return 64;
#endif
#ifndef PRO40_HAL_DELEGATED_HARDWARE
    if(endurance || longEndurance || startup)return 64;
#endif
    const bool capture=endurance || longEndurance || startup || session || std::strcmp(argv[1],"--capture")==0,absent=std::strcmp(argv[1],"--absent")==0;
    if(!capture && !absent && std::strcmp(argv[1],"--inspect"))return 64;
    if(!defaults())return 1;
    AudioObjectID id=0;if(!find(id))return 1;
    if(!id){std::puts("Delegated device absent");return absent?0:1;}
    if(absent || !inspect(id))return 1;
    if(!capture)return 0;
    // Fix the test request to512; report and verify what the host accepts.
    UInt32 frames=512;auto a=address(kAudioDevicePropertyBufferFrameSize);
    const auto set=AudioObjectSetPropertyData(id,&a,0,nullptr,sizeof(frames),&frames);
    if(set || !read(id,a,frames) || frames!=512){std::fprintf(stderr,"buffer size status=%d actual=%u\n",int(set),frames);return 1;}
    AudioDeviceIOProcID proc=nullptr;
#ifdef PRO40_HAL_SOURCE_LOSS_FIXTURE
    auto aliveAddress=address(kAudioDevicePropertyDeviceIsAlive);
    auto devicesAddress=address(kAudioHardwarePropertyDevices);
    if(AudioObjectAddPropertyListener(id,&aliveAddress,aliveChanged,nullptr))return 1;
    if(AudioObjectAddPropertyListener(kAudioObjectSystemObject,&devicesAddress,devicesChanged,nullptr))return 1;
#endif
    const auto created=AudioDeviceCreateIOProcID(id,io,nullptr,&proc);
    if(created || !proc)return 1;
    const auto beforeStart=std::chrono::steady_clock::now();
    const auto started=AudioDeviceStart(id,proc);
    const auto afterStart=std::chrono::steady_clock::now();
#ifdef PRO40_HAL_DELEGATED_HARDWARE
    const unsigned seconds=session?180:startup?1:longEndurance?600:endurance?90:24;
#else
    const unsigned seconds=session?60:1;
#endif
#ifdef PRO40_HAL_SOURCE_LOSS_FIXTURE
    bool lossObserved=false,callbacksStopped=false;AudioObjectID published=id;
    if(!started) {
        const auto deadline=afterStart+std::chrono::seconds(8);
        while(std::chrono::steady_clock::now()<deadline) {
            if(find(published) && !published){lossObserved=true;break;}
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto calls=state.calls.load();std::this_thread::sleep_for(std::chrono::milliseconds(200));
        callbacksStopped=state.calls.load()==calls;
    }
    std::printf("source_loss published=%u device_list_notices=%u alive_notices=%u callbacks_stopped=%u before_client_StopIO=1\n",published,deviceListNotices.load(),aliveNotices.load(),unsigned(callbacksStopped));
#else
    if(!started)std::this_thread::sleep_for(std::chrono::seconds(seconds));
#endif
    const auto beforeStop=std::chrono::steady_clock::now();
    const auto stopped=started?noErr:AudioDeviceStop(id,proc);
    const auto afterStop=std::chrono::steady_clock::now();
    const auto destroyed=AudioDeviceDestroyIOProcID(id,proc);
#ifdef PRO40_HAL_SOURCE_LOSS_FIXTURE
    const auto listenerRemoved=AudioObjectRemovePropertyListener(id,&aliveAddress,aliveChanged,nullptr);
    const auto listListenerRemoved=AudioObjectRemovePropertyListener(kAudioObjectSystemObject,&devicesAddress,devicesChanged,nullptr);
#endif
    // Read non-atomic progress only after successful host stop/destroy.
#ifdef PRO40_HAL_SOURCE_LOSS_FIXTURE
    const auto removedStatus=[&](OSStatus status){return !status || (lossObserved && (status==kAudioHardwareBadDeviceError || status==kAudioHardwareBadObjectError));};
    if(started || !removedStatus(stopped) || !removedStatus(destroyed) || !removedStatus(listenerRemoved) || listListenerRemoved)return 1;
    std::printf("removed_lifecycle stop=%d destroy=%d listener=%d\n",int(stopped),int(destroyed),int(listenerRemoved));
    // A removed device may return BadDevice rather than joining an already
    // deleted IOProc. Pin this diagnostic snapshot against any in-flight IO.
    while(state.busy.test_and_set(std::memory_order_acquire))std::this_thread::yield();
#else
    if(started || stopped || destroyed){std::fprintf(stderr,"lifecycle start=%d stop=%d destroy=%d\n",int(started),int(stopped),int(destroyed));return 1;}
#endif
    std::printf("callbacks=%llu input_frames=%llu output_frames=%llu failures=%llu\n",
        (unsigned long long)state.calls.load(),(unsigned long long)state.input.frames,
        (unsigned long long)state.output.frames,(unsigned long long)state.failures.load());
    bool ok=state.calls>=50*seconds && state.input.frames>=24000*seconds && state.output.frames>=24000*seconds && !state.failures;
#ifdef PRO40_HAL_SOURCE_LOSS_FIXTURE
    ok=ok && lossObserved && callbacksStopped && deviceListNotices.load()>0;
#endif
#ifdef PRO40_HAL_DELEGATED_HARDWARE
    ok=ok && state.nonzero[0]>0;
    std::printf("hardware-input nonzero=");
    for(unsigned c=0;c<8;++c)std::printf("%s%llu",c?",":"",(unsigned long long)state.nonzero[c]);
    std::printf(" peaks=");for(unsigned c=0;c<8;++c)std::printf("%s%.9g",c?",":"",state.peaks[c]);
    std::puts(" output=silence");
#endif
    std::printf("{\"prepared\":true,\"callbacks\":%llu,\"error\":%llu,\"cleanup\":true,\"input_channels\":8,\"output_channels\":6,\"input_frames\":%llu,\"output_frames\":%llu,\"start_ms\":%lld,\"stop_ms\":%lld}\n",
        (unsigned long long)state.calls.load(),(unsigned long long)state.failures.load(),(unsigned long long)state.input.frames,
        (unsigned long long)state.output.frames,(long long)std::chrono::duration_cast<std::chrono::milliseconds>(afterStart-beforeStart).count(),
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(afterStop-beforeStop).count());
    std::puts(ok?"PASS: delegated8/6 host callbacks":"FAIL: delegated host callbacks");return ok?0:1;
}
