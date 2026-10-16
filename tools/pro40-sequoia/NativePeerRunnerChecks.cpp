// SPDX-License-Identifier: Apache-2.0
// Real runner and peer owner, fail-fast native boundary. No FireWire implementation
// is linked. A test peer is provisioned ONLY by the runner preparation hook.
#include "NativePeerSessionFinalizer.hpp"
#include "NativeIdentityBridgeResources.hpp"
#include "NativeStartupConsumer.hpp"
#include "NativeRunnerSavedInput.hpp"
#include "NativeFireWire.h"
#include <IOKit/IOReturn.h>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>
using namespace Pro40Audio;
using namespace std::chrono_literals;
static bool nativeEntered=false,nativeClosed=false;
static unsigned opens=0,closes=0;
// Synthetic admission evidence only; no audio/DMA emulation. Exercise callback
// adapter lifetime inside the real runner's prepare/finalize envelope.
struct CallbackClock {static inline uint64_t value=4200010;static uint64_t now(){return value;}};
struct CallbackProducer {
    NativeStartDescriptor descriptor{{{7,9,2,1,1},100,1001,1,2880000},8,4096,4200000,4200010,4200110,512};
    bool healthy=true,accepted=false;
    bool ready(){return healthy;}uint64_t produced(){return 4608;}
    bool ended(){return false;} // These fail-fast scenarios never deliver an audio END.
    bool prepareAt(uint64_t,NativeStartDescriptor &out){out=descriptor;return healthy;}
    bool acceptAt(const NativeStartDescriptor &in,uint64_t){accepted=in==descriptor;return healthy&&accepted;}
    bool pollAt(uint64_t){return healthy;}void cancel(){healthy=false;}
    Pro40::NativeTimingEvidence::Consumer callbacks(){return {this,
        [](void *p,size_t,const auto &) noexcept{return static_cast<CallbackProducer *>(p)->healthy;},
        [](void *p,const auto &) noexcept{return static_cast<CallbackProducer *>(p)->healthy;},
        [](void *p) noexcept{return static_cast<CallbackProducer *>(p)->healthy;}};}
};
struct Fixture {
    NativeStartMailbox startup;NativeFeedMailbox feed;NativePeerSession session{startup,feed};
    std::atomic<bool> running{false},release{false};
    std::atomic<unsigned> acquisitions{0};
    mach_port_t data=MACH_PORT_NULL;std::string mode;unsigned cancelCalls=0,confirmCalls=0,closeCalls=0,hookCalls=0,prepareCalls=0,completionCalls=0;
    struct Channel {
        bool enabled=false;NativeStartMessage message;unsigned sends=0;
        bool send(const NativeStartMailbox::Bytes &b){assert(enabled);++sends;return NativeStartPacket::decode(b,message);}
        bool receive(NativeStartMailbox::Bytes &b){
            assert(enabled);message.kind=message.kind==NativeStartMessage::Kind::offer?NativeStartMessage::Kind::echo:NativeStartMessage::Kind::ready;
            return NativeStartPacket::encode(message,b);
        }
    } channel;
    CallbackProducer producer;
    NativeStartupConsumer<CallbackProducer,NativePeerSession,CallbackClock> consumer{producer,session,startup,producer.descriptor.clock.identity,11};
    std::unique_ptr<NativeRunnerSavedInput<Fixture>> saved;
    unsigned verificationCalls=0;
    struct Verify {
        Fixture &self;
        IdentityLabOutcome operator()()noexcept{
            assert(nativeEntered&&!nativeClosed);++self.verificationCalls;
            IdentityLabOutcome result{0,"verified",true,0};
            NativeChildProcess child(42,{[](pid_t pid,int *status,int)->pid_t{*status=0;return pid;},::kill});
            assert(child.poll()==NativeChildProcess::State::reaped);result.process.emplace(std::move(child));
            if(self.mode=="identity_rejected"){result.status=-67050;result.reason="rejected";}
            if(self.mode=="identity_blocked")result.resourceError=EIO;
            if(self.mode=="identity_pending"){
                result.reaped=false;result.pendingPID=42;result.status.reset();result.process.reset();
                result.process.emplace(42,NativeChildCalls{[](pid_t pid,int *status,int)->pid_t{*status=0;return pid;},::kill});
            }
            return result;
        }
    } verifier{*this};
    NativeIdentityBridgeResources<Fixture,Verify> identityResources{*this,verifier};
    std::unique_ptr<NativeConsumerBridge<CallbackProducer,CallbackClock,decltype(identityResources)>> identity;
    bool identityDenied()const{return mode=="identity_rejected"||mode=="identity_blocked"||mode=="identity_pending";}
    bool helperBlocked()const{return mode=="identity_blocked"||mode=="identity_pending";}
    bool callbacksMode()const{return mode.starts_with("callbacks_");}
    bool savedMode()const{return mode.starts_with("saved_");}
    bool unknownRemote()const{return mode=="unknown"||mode=="callbacks_unknown"||mode=="saved_unknown"||mode=="identity_remote_failure";}
    bool failedClose()const{return mode=="close_failure"||mode=="callbacks_close_failure"||mode=="identity_close_failure";}
    NativePeerSessionFinalizer<Fixture> finalizer{session,*this,5s};
    static int32_t finish(void *context) noexcept {
        auto &self=*static_cast<Fixture *>(context);assert(nativeClosed&&self.completionCalls==(self.mode=="no_completion"?0u:1u));++self.hookCalls;
        if(self.identity)return self.identity->finalization().status();
        return self.saved?self.saved->finalization().status():self.finalizer.hook().status();
    }
    static int32_t complete(void *context,bool accepted,Pro40::StopQuery) noexcept {
        auto &self=*static_cast<Fixture *>(context);
        assert(nativeClosed&&!accepted&&!self.hookCalls);++self.completionCalls;
        if(self.mode=="completion_delay")std::this_thread::sleep_for(20ms);
        return self.mode=="completion_failure"?kIOReturnTimeout:0;
    }
    static int32_t prepare(void *context) noexcept {
        auto &self=*static_cast<Fixture *>(context);assert(nativeEntered&&!nativeClosed);++self.prepareCalls;
        if(self.mode=="prepare_empty")return kIOReturnNoDevice;
        if(self.identity){
            const auto status=self.identity->preparation().status();
            // Fail-fast native boundary after actual identity/resource preparation.
            // No FireWire functions beyond open/close may run in this fixture.
            return status?status:kIOReturnNoDevice;
        }
        if(self.saved){
            const auto status=self.saved->preparation().status();
            if(status)return status;
            // Test-only replay AFTER the bridge's actual preparation returns.
            // Production preparation never drives callbacks or parses a tape.
            self.saved->drive(self.mode=="saved_cancel");return kIOReturnNoDevice;
        }
        self.provision();
        return self.mode=="stopped_prepare"?0:kIOReturnNoDevice;
    }
    int32_t prepare(){provision();return mode=="identity_prepare_failure"?kIOReturnNoDevice:0;} // Shared bridge resource policy.
    void provision(){
        assert(nativeEntered&&!nativeClosed);
        if(mode=="none")return;
        if(!saved&&!identity)assert(finalizer.arm()&&!finalizer.arm());
        assert(mach_port_allocate(mach_task_self(),MACH_PORT_RIGHT_RECEIVE,&data)==KERN_SUCCESS);
        if(!callbacksMode()&&!savedMode())assert(mach_port_insert_right(mach_task_self(),data,data,MACH_MSG_TYPE_MAKE_SEND)==KERN_SUCCESS);
        channel.enabled=callbacksMode()||savedMode();
        assert(session.start(channel,NativePeerSession::Wall::now()+5s,saved?saved->epoch():1,100,[&]{
            acquisitions.fetch_add(1,std::memory_order_relaxed);
            if(callbacksMode()||savedMode())assert(mach_port_insert_right(mach_task_self(),data,data,MACH_MSG_TYPE_MAKE_SEND)==KERN_SUCCESS);
            return data;
        },[&](NativeFeedMachSender &){
            running.store(true,std::memory_order_release);const auto deadline=NativePeerSession::Wall::now()+5s;
            if(saved){saved->sender();return;}
            while(!release.load(std::memory_order_acquire)){
                assert(NativePeerSession::Wall::now()<deadline);std::this_thread::yield();
            }
        }));
        if(saved)return; // Bridge enables callbacks after this method returns.
        if(callbacksMode()){
            assert(consumer.enable());const auto cb=consumer.callbacks();using State=decltype(consumer)::State;
            assert(cb.record(cb.context,0,{})&&consumer.state()==State::empty);
            assert(cb.clock(cb.context,{})&&consumer.state()==State::offered);
            if(mode=="callbacks_cancel"){consumer.cancel();assert(!cb.poll(cb.context));return;}
            if(mode=="callbacks_expire"){
                CallbackClock::value=4200111;assert(!cb.poll(cb.context)&&consumer.state()==State::failed);return;
            }
            const auto deadline=NativePeerSession::Wall::now()+5s;
            while(!running.load(std::memory_order_acquire)){
                assert(NativePeerSession::Wall::now()<deadline&&cb.poll(cb.context));
                std::this_thread::sleep_for(100us); // Fixture pacing outside callback.
            }
            assert(consumer.state()==State::active&&producer.accepted);return;
        }
        // This contract test bypasses packet admission; it checks runner teardown,
        // not wire startup. Full admission is covered by saved process replay.
        session.activate();const auto deadline=NativePeerSession::Wall::now()+5s;
        while(!running.load(std::memory_order_acquire)){assert(NativePeerSession::Wall::now()<deadline);std::this_thread::yield();}
    }
    void cancel(){assert(nativeClosed);++cancelCalls;if(callbacksMode())consumer.cancel();release.store(true,std::memory_order_release);}
    bool confirmRemote(){
        assert(nativeClosed&&!session.joinable()&&cancelCalls==1);++confirmCalls;
        mach_port_urefs_t refs=99;const auto r=mach_port_get_refs(mach_task_self(),data,MACH_PORT_RIGHT_SEND,&refs);
        assert((r==KERN_SUCCESS&&refs==0)||r==KERN_INVALID_RIGHT);
        return !unknownRemote();
    }
    bool closeControl(){
        assert(nativeClosed&&confirmCalls==1);++closeCalls;
        assert(mach_port_mod_refs(mach_task_self(),data,MACH_PORT_RIGHT_RECEIVE,-1)==KERN_SUCCESS);data=MACH_PORT_NULL;
        return !failedClose(); // Inject status after releasing the real test port.
    }
};
static Fixture *fixture=nullptr;
extern "C" {
int32_t pro40_open(uint64_t){assert(fixture);++opens;nativeEntered=true;nativeClosed=false;return fixture->mode=="none"?kIOReturnNoDevice:0;}
int32_t pro40_state(uint32_t *,uint16_t *){std::abort();}
int32_t pro40_admit_read(uint64_t,uint32_t){std::abort();}
int32_t pro40_read(uint32_t,uint64_t,uint32_t,void *){std::abort();}
void pro40_begin_cleanup(){std::abort();}
void pro40_close(){assert(nativeEntered);++closes;nativeClosed=true;}
int32_t pro40_clock_state(uint32_t,Pro40ClockState *){std::abort();}
void *pro40_native_interface(){std::abort();}
}
int main(int argc,char **argv){
    assert(argc==2);Fixture f;fixture=&f;f.mode=argv[1];
    int delayMs=-1;
    if(f.mode=="saved_delay_0")delayMs=0;
    if(f.mode=="saved_delay_20")delayMs=20;
    if(f.mode=="saved_delay_200")delayMs=200;
    if(f.mode=="saved_delay_1100")delayMs=1100;
    if(f.mode=="saved_end_hold")delayMs=0;
    if(f.mode=="saved_measured")delayMs=-2;
    if(f.mode=="saved_model_expired")delayMs=-3;
    assert(f.mode=="identity_valid"||f.mode=="identity_rejected"||f.mode=="identity_blocked"||f.mode=="identity_pending"||f.mode=="identity_close_failure"||f.mode=="identity_remote_failure"||f.mode=="identity_prepare_failure"||delayMs!=-1||f.mode=="none"||f.mode=="confirmed"||f.mode=="unknown"||f.mode=="close_failure"||f.mode=="stopped_open"||f.mode=="prepare_empty"||f.mode=="stopped_prepare"||f.mode=="callbacks_active"||f.mode=="callbacks_cancel"||f.mode=="callbacks_expire"||f.mode=="callbacks_unknown"||f.mode=="callbacks_close_failure"||f.mode=="saved_valid"||f.mode=="saved_cancel"||f.mode=="saved_unknown"||f.mode=="completion_failure"||f.mode=="completion_delay"||f.mode=="no_completion");
    if(f.mode.starts_with("identity_"))f.identity=std::make_unique<NativeConsumerBridge<CallbackProducer,CallbackClock,decltype(f.identityResources)>>(
        f.producer,f.session,f.startup,f.producer.descriptor.clock.identity,11,f.identityResources,5s);
    if(f.savedMode())f.saved=std::make_unique<NativeRunnerSavedInput<Fixture>>(f.feed,f.startup,f.session,f,delayMs,f.mode=="saved_end_hold");
    namespace fs=std::filesystem;char pattern[]="build/pro40-sequoia/peer-runner-XXXXXX";
    const auto directory=mkdtemp(pattern);assert(directory);const fs::path root(directory);
    Pro40::NativeSessionOptions options;options.guid=0x00130e0400000001;options.retainHistory=false;
    options.logPath=(root/"run.log").string();options.peerCleanup={Fixture::finish,&f};options.peerPreparation={Fixture::prepare,&f};
    options.sourceCompletion={Fixture::complete,&f};
    auto invalid=options;invalid.logPath.clear();assert(Pro40::runNativeSession(invalid).exitCode==64);
    auto noCleanup=options;noCleanup.peerCleanup={};assert(Pro40::runNativeSession(noCleanup).exitCode==64);
    noCleanup.peerPreparation={};assert(Pro40::runNativeSession(noCleanup).exitCode==64);
    auto cancelled=options;cancelled.stop={[](void *) noexcept{return true;},nullptr};
    assert(Pro40::runNativeSession(cancelled).exitCode==2);
    const auto sentinel=root/"existing.log";{std::ofstream file(sentinel);file<<"unchanged";}
    auto prep=options;prep.logPath=sentinel.string();assert(Pro40::runNativeSession(prep).exitCode==1);
    assert(f.verificationCalls==0);
    assert(opens==0&&closes==0&&f.cancelCalls==0&&f.hookCalls==0&&f.prepareCalls==0&&f.completionCalls==0&&!f.session.joinable());
    if(f.mode=="stopped_open")options.stop={[](void *) noexcept{return nativeEntered;},nullptr};
    if(f.mode=="stopped_prepare")options.stop={[](void *p) noexcept{return static_cast<Fixture *>(p)->prepareCalls!=0;},&f};
    if(f.mode=="no_completion")options.sourceCompletion={};
    const auto result=Pro40::runNativeSession(options);
    const int expected=f.unknownRemote()?-2:(f.failedClose()||f.helperBlocked())?-1:0;
    assert(result.exitCode==2&&result.outcome&&result.outcome->cleanup==expected&&!result.accepted());
    assert(result.outcome->evidence==(f.mode=="completion_failure"?kIOReturnTimeout:0));
    assert(opens==1&&closes==1&&f.hookCalls==1&&!f.session.joinable());
    const unsigned prepares=(f.mode=="none"||f.mode=="stopped_open")?0:1;
    assert(f.prepareCalls==prepares);
    if(f.mode=="stopped_open"||f.mode=="stopped_prepare")assert(result.outcome->transfer==kIOReturnAborted);
    const unsigned calls=(f.mode=="none"||f.mode=="stopped_open"||f.mode=="prepare_empty"||f.identityDenied())?0:1;
    assert(f.cancelCalls==calls&&f.confirmCalls==calls&&f.closeCalls==calls);
    if(f.saved){
        assert(f.running.load()&&f.acquisitions.load()==1&&f.channel.sends==2);
        f.saved->verifyAfterJoin(f.mode=="saved_cancel");
    }
    if(f.callbacksMode()){
        assert(!f.producer.healthy);
        const bool active=f.mode!="callbacks_cancel"&&f.mode!="callbacks_expire";
        assert(f.running.load()==active&&f.acquisitions.load()==unsigned(active));
        if(active)assert(f.channel.sends==2);
    }
    if(f.saved){
        assert(f.saved->finalization().status()==expected&&f.cancelCalls==calls);
        assert(f.saved->preparation().status()!=0); // Cannot reopen after cleanup.
    }else if(f.identity){
        assert(f.verificationCalls==1 && f.identity->finalization().status()==expected);
        assert(f.identity->preparation().status()!=0 && f.verificationCalls==1);
        assert(f.acquisitions.load()==(f.identityDenied()?0u:1u));
        assert(f.running.load()==!f.identityDenied());
        if(f.mode=="identity_pending"){
            assert(f.identityResources.observeRetainedChild()==NativeChildProcess::State::reaped);
            assert(f.identity->finalization().status()==-1 && f.identity->preparation().status()!=0);
        }
    }else assert(f.finalizer.finish()==expected&&!f.finalizer.arm()&&f.cancelCalls==calls);
    options.logPath=(root/"next.log").string();options.stop={[](void *) noexcept{return true;},nullptr};
    const auto next=Pro40::runNativeSession(options);
    assert(next.exitCode==(expected?75:2)&&opens==1&&closes==1&&!fs::exists(options.logPath));
    assert(f.closeCalls==calls&&f.hookCalls==1);
    {
        std::ifstream log(root/"run.log");std::string line;unsigned rows=0,ageRows=0;
        while(std::getline(log,line))if(line.starts_with("duplex-clock-evidence-age ")){
            unsigned state=99;unsigned long long lower=99,upper=99;
            assert(std::sscanf(line.c_str(),"duplex-clock-evidence-age state=%u lower-us=%llu upper-us=%llu",&state,&lower,&upper)==3);
            assert(state==0&&!lower&&!upper);++ageRows;std::puts(line.c_str());
        }else if(line.starts_with("duplex-shutdown-timing ")){
            unsigned capture=99,hook=99;unsigned long long captureUs=99,closeUs=99,sourceUs=99,peerUs=99;
            assert(std::sscanf(line.c_str(),"duplex-shutdown-timing capture-exit=%u source-hook=%u capture-to-source-us=%llu native-close-to-source-us=%llu source-us=%llu peer-cleanup-us=%llu",
                &capture,&hook,&captureUs,&closeUs,&sourceUs,&peerUs)==6);
            assert(!capture&&!captureUs&&hook==(f.mode=="no_completion"?0u:1u));
            if(f.mode=="completion_delay")assert(sourceUs>=20000);
            ++rows;std::puts(line.c_str());
        }
        assert(rows==1&&ageRows==1);
    }
    fs::remove(root/"run.log");fs::remove(sentinel);fs::remove(root);
    std::printf("PASS: real runner/session finalizer mode=%s cleanup=%d; post-close ordering, no pre-entry peer, cached cleanup and admission gate\n",argv[1],expected);
}
