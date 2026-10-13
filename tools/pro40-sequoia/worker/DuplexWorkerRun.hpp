// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ManagedWorkerAdmission.hpp"
#include "DeviceLease.hpp"
#include "DeviceLeasePath.hpp"
#include "RecordingExport.hpp"
#include "../NativeDuplexBridge.hpp"
#include "../NativeDuplexPolicy.hpp"
#include "../DuplexMachRendezvous.hpp"
#include "../InputMonitor.hpp"
#include <servers/bootstrap.h>
namespace Pro40 {
// Experimental, explicitly selected profile. Its playback mapping uses the
// packet presentation timeline; analogue latency remains uncalibrated. No automatic default
// route, loopback monitoring, fallback profile or environment-based admission.
inline NativeSessionResult runDuplexWorker(int argc,char **argv,StopQuery external,DeviceLease &lease,
                                          const ManagedWorkerPeer &admission) {
    if(argc!=10 || std::string_view(argv[1])!="--duplex-session" || std::string_view(argv[7])!="--guid" || !admission.parent)
        return {64,{}};
    const auto parse=[](const char *text,uint64_t &value,int base=10) {
        const std::string_view s(text);const auto r=std::from_chars(s.data(),s.data()+s.size(),value,base);
        return !s.empty() && r.ec==std::errc{} && r.ptr==s.data()+s.size();
    };
    Pro40Audio::DuplexLaunchBinding binding;binding.requestID=admission.requestID;
    uint64_t seed=0,generation=0;NativeSessionOptions options;
    if(!parse(argv[3],binding.epoch) || !parse(argv[4],binding.nonce) || !parse(argv[5],seed) || !seed ||
       !parse(argv[6],generation) || generation>UINT32_MAX || !parse(argv[8],options.guid,16) || !options.guid || !binding.valid())return {64,{}};
    const std::string_view serviceName(argv[2]);
    if(serviceName.empty() || serviceName.size()>=BOOTSTRAP_MAX_NAME_LEN || argv[9][0]!='/')return {64,{}};
    if(external.requested())return {2,{}};
    // This executable handles one session and exits; this is its own native
    // owner thread, not a dispatch queue borrowed from another client.
    const auto nativeScheduling=Pro40Audio::AudioThreadScheduling::request();
    if(!nativeScheduling.accepted())return {74,{}};
    // Synchronous bootstrap RPC is confined to the supervised worker BEFORE
    // device admission or native entry. The launcher must retain its watchdog.
    Pro40Audio::NativeMachPort service(MACH_PORT_RIGHT_SEND);mach_port_t raw=MACH_PORT_NULL;
    if(bootstrap_look_up(bootstrap_port,argv[2],&raw)!=KERN_SUCCESS)return {74,{}};
    if(service.adopt(raw)!=KERN_SUCCESS)return {74,{}};
    Pro40Audio::DuplexMachConnector connector;
    if(!connector.start(service.get(),admission.parent->token(),binding,std::chrono::steady_clock::now()+std::chrono::seconds(2)))return {74,{}};
    if(service.close()!=KERN_SUCCESS)return {74,{}};
    auto state=Pro40Audio::DuplexMachConnector::State::waiting;
    while((state=connector.poll(external.requested()))==Pro40Audio::DuplexMachConnector::State::waiting)
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    if(state!=Pro40Audio::DuplexMachConnector::State::connected)return {external.requested()?2:74,{}};
    mach_timebase_info_data_t tb{};if(mach_timebase_info(&tb)!=KERN_SUCCESS)return {1,{}};
    const auto ticks=[&](uint64_t ns){return uint64_t(__uint128_t(ns)*tb.denom/tb.numer);};
    Pro40Audio::NativeDuplexBridgePolicy policy{
        {binding.epoch,seed,uint32_t(generation),tb.numer,tb.denom},
        Pro40Audio::nativeDuplexClockPolicy(tb.numer,tb.denom),
        ticks(40000000),Pro40Audio::nativeDuplexLeadFrames,Pro40Audio::nativeDuplexPrimeFrames,std::chrono::seconds(12)};
    auto bridge=std::make_unique<Pro40Audio::NativeDuplexBridge>(connector.takeChannel(),policy,external);
    if(bridge->fault()!=Pro40Audio::NativeDuplexBridgeFault::none)return {64,{}};
    const auto directory=deviceLeaseDirectory();if(!directory)return {1,{}};
    const auto admitted=lease.acquire(directory->c_str(),options.guid);
    if(admitted!=DeviceAdmission::acquired)return {admitted==DeviceAdmission::ioError?1:75,{}};
    if(!bridge->start()) {
        if(!lease.confirmClean()){SessionOutcome failed;failed.cleanup=kIOReturnIOError;return {2,failed};}
        return {1,{}};
    }
    struct Stop {StopQuery external;Pro40Audio::NativeDuplexBridge *bridge;} stop{external,bridge.get()};
    options.stop={[](void *p)noexcept{auto &s=*static_cast<Stop *>(p);return s.external.requested() || s.bridge->requested();},&stop};
    Pro40Packets::InputMonitor monitor;
    options.expectedGeneration=uint32_t(generation);
    options.sustained=true;options.retainHistory=false;options.inputMonitor=&monitor;
    options.inputHandoff=&bridge->handoff;options.livePlayback=&bridge->source;options.logPath=argv[9];
    auto result=runNativeSession(options);
    const bool nativeClean=result.outcome?result.outcome->cleanup==0 && (result.exitCode==0 || result.exitCode==2):
        (result.exitCode==1 || result.exitCode==64 || result.exitCode==75);
    const bool clean=bridge->finish(nativeClean);
    if(!clean || !lease.confirmClean()) {
        if(!result.outcome)result.outcome=SessionOutcome{};
        result.outcome->cleanup=kIOReturnIOError;result.exitCode=2;
    }
    // Bridge faults request the same native cancellation path as a user STOP.
    // Preserve that teardown result but do not report the fault as an ordinary
    // interruption merely because the native runner returned exit2/aborted.
    if(bridge->fault()!=Pro40Audio::NativeDuplexBridgeFault::none) {
        if(!result.outcome)result.outcome=SessionOutcome{};
        if(!result.outcome->evidence)result.outcome->evidence=kIOReturnError;
        result.exitCode=2;
    }
    const auto &positions=bridge->playback.positions();
    const auto &offered=bridge->offeredClock();const auto &controller=bridge->offeredController();
    const auto &snapshot=bridge->offeredSnapshot();
    const auto &presentation=bridge->offeredPresentation();
    const bool haveClock=offered.valid(); // Preserve admitted evidence even after a terminal receiver fault.
    const auto &startup=bridge->startupObservation();
    const auto &bridgeScheduling=bridge->schedulingAfterJoin();
    char report[4096];const int size=std::snprintf(report,sizeof(report),
        "{\"schema\":2,\"bridge_fault\":%u,\"startup_fault\":%u,\"playback_fault\":%u,\"cutover\":%llu,\"started_at\":%llu,"
        "\"prepared_frames\":%llu,\"application_frames\":%llu,\"input_frames\":%llu,\"native_cleanup\":%s,\"timing_calibrated\":false,"
        "\"buffering\":{\"lead_frames\":%llu,\"prime_frames\":%llu,\"input_history_frames\":%llu,\"queue_low_water_frames\":%llu,\"queue_high_water_frames\":%llu},"
        "\"output_presentation\":{\"applied\":%s,\"offset_frames\":%lld,\"unrounded_offset_frames\":%.17Lg,\"sample_at_cutover\":%llu,\"first_elapsed_bus_ticks\":%llu},"
        "\"native_qos\":{\"accepted\":%s,\"before\":%u,\"after\":%u},\"bridge_qos\":{\"accepted\":%s,\"before\":%u,\"after\":%u},"
        "\"have_clock\":%s,\"input_clock_origin\":%llu,\"ticks_numerator\":%llu,\"ticks_denominator\":%llu,"
        "\"controller_origin_host\":%llu,\"controller_first_bus_ticks\":%llu,\"timebase_numer\":%u,\"timebase_denom\":%u,\"bus_slope\":%.17Lg,\"bus_intercept\":%.17Lg,\"bus_fit_count\":%llu,\"generation\":%u,\"startup_observation\":{\"rejection\":%u,\"packet_slope\":%.17Lg,\"bus_slope\":%.17Lg,\"packet_age_ticks\":%llu,\"evidence_host\":%llu,\"now\":%llu,\"prefix\":%llu,\"oldest\":%llu,\"next\":%llu},\"tx_positions\":["
        "{\"frame\":%llu,\"packet\":%llu,\"cycle\":%u,\"syt\":%u,\"frames\":%u},"
        "{\"frame\":%llu,\"packet\":%llu,\"cycle\":%u,\"syt\":%u,\"frames\":%u},"
        "{\"frame\":%llu,\"packet\":%llu,\"cycle\":%u,\"syt\":%u,\"frames\":%u}]}\n",
        unsigned(bridge->fault()),unsigned(bridge->playback.fault()),unsigned(bridge->output.fault()),
        (unsigned long long)bridge->offeredFirst(),(unsigned long long)bridge->playback.startedAt(),
        (unsigned long long)bridge->playback.nextNativeFrame(),(unsigned long long)bridge->output.consumed(),
        (unsigned long long)bridge->receiver.frames(),clean?"true":"false",
        (unsigned long long)policy.leadFrames,(unsigned long long)policy.primeFrames,
        (unsigned long long)Pro40Audio::nativeDuplexInputHistoryFrames,
        (unsigned long long)bridge->output.lowWater(),(unsigned long long)bridge->output.highWater(),
        haveClock?"true":"false",(long long)presentation.offsetFrames,presentation.unroundedOffsetFrames,
        (unsigned long long)presentation.sampleAtCutover,(unsigned long long)presentation.firstElapsedBusTicks,
        nativeScheduling.accepted()?"true":"false",unsigned(nativeScheduling.before),unsigned(nativeScheduling.after),
        bridgeScheduling.accepted()?"true":"false",unsigned(bridgeScheduling.before),unsigned(bridgeScheduling.after),
        haveClock?"true":"false",(unsigned long long)offered.origin,
        (unsigned long long)offered.ticksNumerator,(unsigned long long)offered.ticksDenominator,
        (unsigned long long)controller.originHost,(unsigned long long)controller.firstBusTicks,tb.numer,tb.denom,
        snapshot.bus.slope,snapshot.bus.intercept,(unsigned long long)snapshot.bus.count,uint32_t(generation),
        startup.rejection,startup.snapshot.packet.slope,startup.snapshot.bus.slope,(unsigned long long)startup.snapshot.maxPacketAgeTicks,
        (unsigned long long)startup.snapshot.controller.lastHost,(unsigned long long)startup.now,(unsigned long long)startup.prefix,
        (unsigned long long)startup.oldest,(unsigned long long)startup.next,
        (unsigned long long)positions[0].frame,(unsigned long long)positions[0].packetOrdinal,positions[0].cycle,positions[0].syt,positions[0].frames,
        (unsigned long long)positions[1].frame,(unsigned long long)positions[1].packetOrdinal,positions[1].cycle,positions[1].syt,positions[1].frames,
        (unsigned long long)positions[2].frame,(unsigned long long)positions[2].packetOrdinal,positions[2].cycle,positions[2].syt,positions[2].frames);
    const bool saved=size>0 && size_t(size)<sizeof(report) && publishRecordingFile(options.logPath+".duplex.json",
        {reinterpret_cast<const uint8_t *>(report),size_t(size)});
    if(!saved) {
        if(!result.outcome)result.outcome=SessionOutcome{};
        if(!result.outcome->evidence)result.outcome->evidence=kIOReturnError;
        result.exitCode=2;
    }
    std::fprintf(stderr,"duplex-worker: cleanup=%u report=%u\n",clean,saved);
    return result;
}
}
