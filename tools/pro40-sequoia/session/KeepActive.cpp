// SPDX-License-Identifier: Apache-2.0
// An ordinary, silent CoreAudio output client. No FireWire/device-control API.
// CoreAudio mixes each client's output, so zeroes leave other apps unchanged.
#include <CoreAudio/CoreAudio.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stopping=0;
void stop(int) {stopping=1;}
std::atomic<uint64_t> callbacks{0},failures{0};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
AudioObjectPropertyAddress address(UInt32 selector,UInt32 scope=kAudioObjectPropertyScopeGlobal) {
    return {selector,scope,kAudioObjectPropertyElementMain};
}
template<class T> bool read(AudioObjectID id,AudioObjectPropertyAddress a,T &value) {
    UInt32 bytes=sizeof(value);
    return !AudioObjectGetPropertyData(id,&a,0,nullptr,&bytes,&value) && bytes==sizeof(value);
}
AudioObjectID find() {
#ifdef PRO40_KEEP_ACTIVE_FIXTURE
    CFStringRef uid=CFSTR("local.pro40.session-fixture.device");
#else
    CFStringRef uid=CFSTR("local.pro40.session.device");
#endif
    AudioObjectID id=0;UInt32 bytes=sizeof(id);
    auto a=address(kAudioHardwarePropertyTranslateUIDToDevice);
    if(AudioObjectGetPropertyData(kAudioObjectSystemObject,&a,sizeof(uid),&uid,&bytes,&id) || bytes!=sizeof(id))return 0;
    return id;
}
bool inspect(AudioObjectID id) {
    Float64 rate=0;
    if(!read(id,address(kAudioDevicePropertyNominalSampleRate),rate) || rate!=48000)return false;
    for(auto scope:{kAudioObjectPropertyScopeInput,kAudioObjectPropertyScopeOutput}) {
        AudioObjectID stream=0;AudioStreamBasicDescription format{};
        if(!read(id,address(kAudioDevicePropertyStreams,scope),stream) ||
           !read(stream,address(kAudioStreamPropertyVirtualFormat),format))return false;
        const UInt32 channels=scope==kAudioObjectPropertyScopeInput?8:6;
        if(format.mSampleRate!=48000 || format.mChannelsPerFrame!=channels ||
           format.mFormatID!=kAudioFormatLinearPCM || format.mFormatFlags!=kAudioFormatFlagsNativeFloatPacked ||
           format.mBytesPerFrame!=4*channels || format.mBytesPerPacket!=4*channels ||
           format.mFramesPerPacket!=1 || format.mBitsPerChannel!=32)return false;
    }
    return true;
}
OSStatus io(AudioObjectID,const AudioTimeStamp *,const AudioBufferList *,const AudioTimeStamp *,
            AudioBufferList *output,const AudioTimeStamp *,void *) {
    // Input usage is disabled at setup. Sequoia15.7.9 nevertheless supplies a
    // non-NULL input pointer (KeepActiveHostChecks, recovery-001). Ignore the
    // entire input list; neither pointer presence nor PCM is output evidence.
    bool valid=true;
    if(!output || output->mNumberBuffers!=1)valid=false;
    else {
        auto &b=output->mBuffers[0];
        if(b.mNumberChannels!=6 || !b.mData || !b.mDataByteSize ||
           b.mDataByteSize%(6*sizeof(float)) || b.mDataByteSize>4096*6*sizeof(float))valid=false;
        else std::memset(b.mData,0,b.mDataByteSize);
    }
    if(!valid)failures.fetch_add(1,std::memory_order_relaxed);
    callbacks.fetch_add(1,std::memory_order_relaxed);
    return noErr;
}
bool disableInput(AudioObjectID id,AudioDeviceIOProcID proc) {
    auto a=address(kAudioDevicePropertyIOProcStreamUsage,kAudioObjectPropertyScopeInput);
    AudioHardwareIOProcStreamUsage usage{reinterpret_cast<void *>(proc),1,{0}};
    if(AudioObjectSetPropertyData(id,&a,0,nullptr,sizeof(usage),&usage))return false;
    usage.mStreamIsOn[0]=1;
    return read(id,a,usage) && usage.mNumberStreams==1 && usage.mStreamIsOn[0]==0;
}
}
int main(int argc,char **argv) {
    if(argc!=2 || std::strcmp(argv[1],"--run"))return 64;
    std::setvbuf(stdout,nullptr,_IOLBF,0);
    struct sigaction action{};action.sa_handler=stop;sigemptyset(&action.sa_mask);
    if(sigaction(SIGTERM,&action,nullptr) || sigaction(SIGINT,&action,nullptr))return 1;
    std::printf("{\"keep_active\":\"waiting\",\"pid\":%d}\n",getpid());
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
    AudioObjectID id=0;
    while(!stopping && !(id=find()) && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if(stopping)return 0;
    if(!id || !inspect(id)){std::puts("{\"keep_active\":\"unavailable\"}");return 1;}
    AudioDeviceIOProcID proc=nullptr;
    OSStatus created=AudioDeviceCreateIOProcID(id,io,nullptr,&proc);
    if(created || !proc)return 1;
    if(!disableInput(id,proc)) {
        (void)AudioDeviceDestroyIOProcID(id,proc);
        std::puts("{\"keep_active\":\"input-disable-failed\"}");return 1;
    }
    const auto began=std::chrono::steady_clock::now();
    const auto started=stopping?OSStatus(kAudioHardwareNotRunningError):AudioDeviceStart(id,proc);
    bool healthy=!started,ready=false;
    uint64_t previous=0;
    auto progressAt=std::chrono::steady_clock::now();
    while(healthy && !stopping) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto count=callbacks.load(std::memory_order_relaxed);
        const auto now=std::chrono::steady_clock::now();
        UInt32 alive=0;
        healthy=!failures.load(std::memory_order_relaxed) && find()==id &&
            read(id,address(kAudioDevicePropertyDeviceIsAlive),alive) && alive==1;
        if(count!=previous){previous=count;progressAt=now;}
        if(now-progressAt>std::chrono::seconds(3))healthy=false;
        if(healthy && count && !ready) {
            ready=true;
            std::printf("{\"keep_active\":\"running\",\"pid\":%d,\"input_disabled\":true,\"startup_ms\":%lld}\n",
                getpid(),(long long)std::chrono::duration_cast<std::chrono::milliseconds>(now-began).count());
        }
    }
    // Callback state is process-lifetime storage, including on host failure.
    const auto stopped=started?noErr:AudioDeviceStop(id,proc);
    const auto destroyed=AudioDeviceDestroyIOProcID(id,proc);
    const bool ok=healthy && !stopped && !destroyed && !failures.load();
    std::printf("{\"keep_active\":\"stopped\",\"pid\":%d,\"healthy\":%s,\"start\":%d,\"stop\":%d,\"destroy\":%d,\"callbacks\":%llu,\"failures\":%llu}\n",
        getpid(),ok?"true":"false",int(started),int(stopped),int(destroyed),
        (unsigned long long)callbacks.load(),(unsigned long long)failures.load());
    // No reconnect loop. A device/stream fault requires inspected recovery.
    return ok?0:1;
}
