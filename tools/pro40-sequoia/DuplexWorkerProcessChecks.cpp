// SPDX-License-Identifier: Apache-2.0
// Controlled checkout lab, actual spawned managed worker, generated audio only.
// IdentityHelperLab has a documented helper-path TOCTOU limit; this fixture is
// not the signed packaged controller and never opens FireWire or an audio device.
#include "DuplexMachRendezvous.hpp"
#include "DuplexGeneratedSource.hpp"
#include "HalDuplexMachSession.hpp"
#include "NativeDuplexSourceConnection.hpp"
#include "NativeIdentityHelperLab.hpp"
#include "NativeIdentityDecision.hpp"
#include "worker/ManagedWorkerAdmission.hpp"
#include "worker/WorkerSpawnPreparation.hpp"
#include "worker/WorkerResultCollector.hpp"
#include "worker/ParentWatch.hpp"
#include <servers/bootstrap.h>
#include <filesystem>
#include <fstream>
#include <thread>
using namespace Pro40Audio;
using namespace std::chrono_literals;
using Wall=std::chrono::steady_clock;
static int result(int code,bool entered=false) {
    std::printf("{\"version\":1,\"type\":\"result\",\"exit\":%d,\"outcome\":",code);
    if(entered)std::printf("{\"reason\":\"%s\",\"transfer\":0,\"evidence\":0,\"cleanup\":0}",code?"interrupted":"completed");
    else std::printf("null");std::puts("}");std::fflush(stdout);return code;
}
static int32_t sample(uint64_t frame,unsigned channel) {return int32_t((frame-512)*6+channel)-400;}
static int worker(int argc,char **argv) {
    Pro40::ParentWatch parent;assert(parent.start(0));
    const Pro40::StopQuery stop{[](void *p)noexcept{return static_cast<Pro40::ParentWatch *>(p)->requested();},&parent};
    std::puts("{\"version\":1,\"type\":\"ready\"}");std::fflush(stdout);
    Pro40::ManagedWorkerPeer admission;
    if(auto refusal=Pro40::managedWorkerAdmission(argc,argv,stop,Wall::now()+8s,&admission))return result(refusal->exitCode);
    assert(argc==4 && admission.parent);
    const std::string mode=argv[3];const DuplexLaunchBinding binding{19,73,admission.requestID};
    NativeMachPort service(MACH_PORT_RIGHT_SEND);mach_port_t raw=MACH_PORT_NULL;
    assert(bootstrap_look_up(bootstrap_port,argv[2],&raw)==KERN_SUCCESS && service.adopt(raw)==KERN_SUCCESS);
    auto token=admission.parent->token();if(mode=="wrong-parent")token.val[0]^=1;
    DuplexMachConnector connector;assert(connector.start(service.get(),token,binding,Wall::now()+700ms));
    assert(service.close()==KERN_SUCCESS);
    auto state=DuplexMachConnector::State::waiting;
    while((state=connector.poll(stop.requested()))==DuplexMachConnector::State::waiting)std::this_thread::sleep_for(100us);
    if(state!=DuplexMachConnector::State::connected)return result(64);
    auto channel=connector.takeChannel();assert(channel && !connector.takeChannel());
    // Evidence marker must not exist before capture + verification + release +
    // the authenticated private endpoint exchange all succeed.
    std::fprintf(stderr,"duplex-entered\n");std::fflush(stderr);
    auto source=std::make_unique<DuplexGeneratedSource>();auto d=source->descriptor();
    auto output=std::make_unique<PlaybackPcmQueue<>>(19);
    auto peer=std::make_unique<NativeDuplexSourceConnection<SustainedInputReceiver<>>>(
        std::move(channel),*source->receiver,*output,d,source->policy(),mach_absolute_time());
    assert(!peer->failed());auto next=Wall::now()+16ms,deadline=Wall::now()+3s;bool confirmed=false;uint64_t checked=0;
    while(!peer->closed() && Wall::now()<deadline) {
        if(stop.requested())break;
        if(Wall::now()>=next && !peer->stopRequested()){source->step();next+=16ms;}
        peer->poll(mach_absolute_time());
        if(peer->primed()) {
            std::array<int32_t,8*6> samples;
            while(output->written()-output->consumed()>=8) {
                const auto first=output->consumed();assert(output->read(first,samples));
                for(unsigned n=0;n<8;++n)for(unsigned c=0;c<6;++c)assert(samples[n*6+c]==(first+n<512?0:sample(first+n,c)));
                assert(output->commit(first,8));checked=first+8;
            }
        }
        if(peer->stopRequested() && !confirmed){assert(peer->confirmStopped(mode!="unclean"));confirmed=true;}
        std::this_thread::sleep_for(100us);
    }
    // The generated source has no DMA users or producer thread. Real native
    // teardown must finish before confirmStopped; a result alone cannot prove it.
    assert(confirmed && peer->closed());
    std::fprintf(stderr,"duplex-finished frames=%llu clean=%u\n",(unsigned long long)checked,peer->cleanupOK());
    assert(checked==55312 && peer->cleanupOK()==(mode!="unclean"));
    return result(0,true);
}
struct Listener {
    int fd=-1;std::string folder,path;
    Listener() {
        char tmp[]="/tmp/pro40-duplex-intro-XXXXXX";assert(mkdtemp(tmp));folder=tmp;path=folder+"/peer";
        fd=socket(AF_UNIX,SOCK_STREAM,0);assert(fd>=0);
        assert(fcntl(fd,F_SETFD,FD_CLOEXEC)==0 && fcntl(fd,F_SETFL,O_NONBLOCK)==0);
        sockaddr_un address{};address.sun_family=AF_UNIX;std::memcpy(address.sun_path,path.c_str(),path.size()+1);
        assert(bind(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 && listen(fd,1)==0);
    }
    ~Listener(){assert(close(fd)==0 && unlink(path.c_str())==0 && rmdir(folder.c_str())==0);}
};
static void round(mach_port_t service,const char *name,const char *executable,const std::string &workerPin,
                  const char *helper,const char *helperPin,const std::filesystem::path &folder,const std::string &mode) {
    Listener intro;std::array<uint8_t,16> nonce{};arc4random_buf(nonce.data(),nonce.size());
    char encoded[33]{};for(size_t i=0;i<nonce.size();++i)std::snprintf(encoded+2*i,3,"%02x",nonce[i]);
    std::vector<std::string> storage{executable,"--managed",encoded,intro.path};
#ifdef PRO40_NATIVE_DUPLEX_RUNNER_CHECK
    const auto session=(folder/(mode+".session")).string();
    const auto leaseFolder=folder/(mode+"-leases");assert(std::filesystem::create_directory(leaseFolder));assert(chmod(leaseFolder.c_str(),0700)==0);
    assert(setenv("PRO40_WORKER_TEST_LOCK_DIR",leaseFolder.c_str(),1)==0);
    if(mode=="unclean")assert(setenv("PRO40_DUPLEX_FIXTURE_DIRTY","1",1)==0);else assert(unsetenv("PRO40_DUPLEX_FIXTURE_DIRTY")==0);
    if(mode=="underrun")assert(setenv("PRO40_DUPLEX_FIXTURE_UNDERRUN","1",1)==0);else assert(unsetenv("PRO40_DUPLEX_FIXTURE_UNDERRUN")==0);
    const std::vector<std::string> tail{"--duplex-session",name,"19","73","1","7","--guid","00130e0400000001",session};
#else
    const std::vector<std::string> tail{"--child",name,mode};
#endif
    storage.insert(storage.end(),tail.begin(),tail.end());std::vector<char*> argv;
    for(auto &s:storage)argv.push_back(s.data());argv.push_back(nullptr);
    const auto errors=folder/(mode+".err");const int errorFD=open(errors.c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);assert(errorFD>=0);
    Pro40::WorkerSpawnPreparation preparation;
    assert(preparation.prepare(MACH_PORT_NULL,errorFD,Pro40::WorkerSpawnPreparation::Namespace::inherit)==0);
    assert(preparation.launch(executable,argv.data(),environ)==0);
    auto child=preparation.takeWorker();assert(child && child->child().pid()!=getpid());
    Pro40::WorkerResultCollector collector(child,preparation.takeResultReader(),Wall::now()+10s);
    assert(preparation.close()==0 && close(errorFD)==0);
    std::optional<Pro40::SocketPeerCapture> capture;const auto captureDeadline=Wall::now()+2s;
    while(!capture && Wall::now()<captureDeadline){
        collector.poll();const int fd=accept(intro.fd,nullptr,nullptr);
        if(fd>=0){capture=Pro40::SocketPeerCapture::capture(fd,child->child().pid());assert(close(fd)==0 && capture);}
        else assert(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR);
        std::this_thread::sleep_for(100us);
    }
    assert(capture && child->child().state()==NativeChildProcess::State::running);
    NativeIdentityRequest request;request.role=1;request.guid=0x00130e0400000001;request.epoch=19;request.requestID=nonce;request.token=capture->token();
    for(size_t n=0;n<20;++n)request.policyHash[n]=std::stoul(workerPin.substr(2*n,2),nullptr,16);
    if(mode=="reject")request.policyHash.fill(1);
    NativeIdentityDecision decision(identityLabVerify(helper,helperPin,request));
    assert(decision.helperCleanupConfirmed() && decision.permitsAdmission()==(mode!="reject"));
    assert(std::filesystem::file_size(errors)==0); // No duplex entry before release.
    unsigned released=0;std::unique_ptr<DuplexMachChannel> channel;
    if(!decision.permitsAdmission())child->denyRelease();
    else if(mode=="cancel")child->cancel();
    else {
        auto ticket=nonce;if(mode=="wrong-release")ticket[0]^=1;
        assert(child->release(ticket)==Pro40::WorkerChildOwner::Release::sent);released=1;
        if(mode!="wrong-release") {
            const auto deadline=Wall::now()+500ms;DuplexMachAcceptor acceptor;bool received=false;
            while(Wall::now()<deadline && !received) {
                collector.poll();NativeMachReceived<512> message;
                if(message.poll(service,nativeAuditReceiveOptions)) {
                    received=true;auto token=capture->token();DuplexLaunchBinding binding{19,73,nonce};
                    if(mode=="wrong-worker")token.val[0]^=1;
                    if(mode=="wrong-session")++binding.epoch;
                    channel=acceptor.accept(message,service,token,binding,deadline);
                    assert(!acceptor.accept(message,service,token,binding,deadline)); // One shot.
                }else assert(message.status()==MACH_RCV_TIMED_OUT);
                std::this_thread::sleep_for(100us);
            }
            assert(received);
        }
    }
    const bool audio=mode=="normal" || mode=="unclean" || mode=="parent-stop" || mode=="underrun";
    if(audio) {
        assert(channel);std::atomic<bool> cancel{false};DuplexGeneratedSource fixture;
        const auto d=fixture.descriptor();
        auto prepared=HalDuplexMachSession::prepare(std::move(channel),d.clock.identity,fixture.policy(),
#ifdef PRO40_NATIVE_DUPLEX_RUNNER_CHECK
            2000ms,2000ms,
#else
            400ms,300ms,
#endif
            cancel);
        assert(prepared.session && prepared.cleanupOK);auto &host=*prepared.session;
        const auto inputFirst=host.descriptor.inputFirst+37;
        AudioServerPlugInIOCycleInfo cycle{};cycle.mInputTime.mSampleTime=double(inputFirst);cycle.mInputTime.mFlags=kAudioTimeStampSampleTimeValid;
        std::array<float,137*8> input;assert(host.readInput(mach_absolute_time(),cycle,137,input));
        for(unsigned n=0;n<137;++n)for(unsigned c=0;c<8;++c)assert(input[n*8+c]==float(Fixture::sample(inputFirst+n,c))/8388608.f);
        uint64_t written=host.descriptor.outputFirst+host.descriptor.primedOutputFrames;std::array<float,137*6> samples;
#ifdef PRO40_NATIVE_DUPLEX_RUNNER_CHECK
        auto callbackDue=Wall::now();
#endif
        for(unsigned block=0;block<400;++block) {
            collector.poll();assert(!collector.finished() && collector.failure()==Pro40::WorkerResultCollector::Fault::none);
#ifdef PRO40_NATIVE_DUPLEX_RUNNER_CHECK
            // Continuous capture, not just the initial retained input window.
            const auto first=inputFirst+block*137;
            cycle.mInputTime.mSampleTime=double(first);
            if(!host.readInput(mach_absolute_time(),cycle,137,input)){
                host.stopAndJoin();const auto detail=static_cast<HalDuplexMachSession &>(host).diagnosticAfterJoin();
                std::fprintf(stderr,"input failure mode=%s block=%u first=%llu oldest=%llu next=%llu reason=%u clock=%u\n",
                    mode.c_str(),block,(unsigned long long)first,(unsigned long long)detail.inputOldest,
                    (unsigned long long)detail.inputNext,detail.inputFailure,detail.inputClockFailure);assert(false);
            }
            for(unsigned n=0;n<137;++n)for(unsigned c=0;c<8;++c)
                assert(input[n*8+c]==float(Fixture::sample(first+n,c))/8388608.f);
#endif
            for(unsigned n=0;n<137;++n)for(unsigned c=0;c<6;++c)samples[n*6+c]=float(
#ifdef PRO40_NATIVE_DUPLEX_RUNNER_CHECK
                Fixture::sample(written+n,c)
#else
                sample(written+n,c)
#endif
                )/8388608.f;
            cycle.mOutputTime.mSampleTime=double(written+host.descriptor.outputTimeline.callbackSample);cycle.mOutputTime.mFlags=kAudioTimeStampSampleTimeValid;
            assert(host.writeOutput(mach_absolute_time(),cycle,137,samples));written+=137;
#ifdef PRO40_NATIVE_DUPLEX_RUNNER_CHECK
            callbackDue+=2854167ns;std::this_thread::sleep_until(callbackDue);
#else
            std::this_thread::sleep_for(1ms);
#endif
        }
#ifndef PRO40_NATIVE_DUPLEX_RUNNER_CHECK
        std::this_thread::sleep_for(30ms);
#endif
        // A finite normal producer requests STOP at its final callback. The
        // native underrun scenario below deliberately withholds further PCM;
        // an unconditional30ms empty tail made normal STOP depend on bank phase.
        SourceFrameClock::Stamp stamp;assert(host.timestamp(mach_absolute_time(),stamp));
#ifdef PRO40_NATIVE_DUPLEX_RUNNER_CHECK
        if(mode=="parent-stop" || mode=="underrun") {
            if(mode=="parent-stop")child->cancel(); // Native teardown BEFORE the audio STOP.
            // UNDERRUN deliberately stops writing while native consumption
            // continues. Its fault must survive the native cancellation path.
            const auto deadline=Wall::now()+(mode=="parent-stop"?350ms:1000ms);bool closed=false;
            while(!closed && Wall::now()<deadline) {
                std::ifstream log(session);const std::string contents{std::istreambuf_iterator<char>(log),{}};
                closed=contents.find("generated-native-closed\n")!=std::string::npos;
                if(!closed)std::this_thread::sleep_for(1ms);
            }
            assert(closed);
        }
#endif
        assert(host.stopAndJoin()==(mode!="unclean"));
    }else if(channel){assert(mode=="wrong-parent" && channel->close());}
    const auto cleanupDeadline=Wall::now()+2s;
    while(!collector.finished() && Wall::now()<cleanupDeadline){collector.poll();std::this_thread::sleep_for(1ms);}
    assert(collector.cleanupComplete() && collector.failure()==Pro40::WorkerResultCollector::Fault::none);
    const auto outcome=collector.result();assert(outcome);
#ifdef PRO40_NATIVE_DUPLEX_RUNNER_CHECK
    if(audio)assert(outcome->outcome && outcome->exitCode==2 && bool(outcome->outcome->cleanup)==(mode=="unclean") &&
        bool(outcome->outcome->evidence)==(mode=="underrun"));
    else assert(!outcome->outcome);
#else
    assert(outcome->accepted()==audio);
#endif
    // Separate private STOPPED and retained child's EOF/reap result are both
    // required; a positive worker result cannot repair a negative STOPPED.
    std::printf("PASS mode=%s released=%u audio=%u child-reaped=1 helper-reaped=1 bootstrap-inherited=1\n",mode.c_str(),released,audio);std::fflush(stdout);
}
int main(int argc,char **argv) {
    signal(SIGPIPE,SIG_IGN);
    if(argc>1 && std::string_view(argv[1])=="--managed")return worker(argc,argv);
    assert(argc==7);mach_port_t raw=MACH_PORT_NULL;
    assert(bootstrap_check_in(bootstrap_port,argv[1],&raw)==KERN_SUCCESS);
    NativeMachPort service(MACH_PORT_RIGHT_RECEIVE);assert(service.adopt(raw)==KERN_SUCCESS);
    for(const std::string mode:{"normal","reject","cancel","wrong-release",
#ifndef PRO40_NATIVE_DUPLEX_RUNNER_CHECK
        "wrong-worker","wrong-parent","wrong-session",
#else
        "parent-stop","underrun",
#endif
        "unclean"})
        round(service.get(),argv[1],argv[2],argv[3],argv[4],argv[5],argv[6],mode);
    assert(service.close()==KERN_SUCCESS);std::puts("PASS all managed duplex process checks");
}
