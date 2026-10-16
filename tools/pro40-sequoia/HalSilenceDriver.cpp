// SPDX-License-Identifier: Apache-2.0
#include "HalSilenceDriver.hpp"
#include "HalSilenceProperties.hpp"
#include "HalSilenceState.hpp"
#include <mach/mach_time.h>
#include <algorithm>
#include <new>
#include <os/log.h>
#ifdef PRO40_HAL_DELEGATED_AUDIO
#define PRO40_HAL_SOURCE_LOSS 1
#endif
#ifdef PRO40_HAL_SOURCE_LOSS
#ifndef PRO40_HAL_DUPLEX_AUDIO
#error Source-loss control requires the duplex HAL state.
#endif
#include "HalSourceLossControl.hpp"
#endif
#ifdef PRO40_HAL_SOURCE_LOSS_FIXTURE
#include "HalSourceLossFixture.hpp"
#endif
#ifdef PRO40_HAL_DELEGATED_AUDIO
#ifndef PRO40_HAL_DUPLEX_AUDIO
#error Delegated audio requires the duplex HAL state.
#endif
#include "HalDelegatedSession.hpp"
#include "DuplexProvisionClient.hpp"
#include "DuplexProvisionClientPins.hpp"
#include "NativeDuplexPolicy.hpp"
#include <unistd.h>
#endif
#if defined(PRO40_HAL_DUPLEX_TIMING) && (defined(PRO40_HAL_DUPLEX_AUDIO) || defined(PRO40_HAL_SYNTHETIC_AUDIO) || defined(PRO40_HAL_FEED_DIAGNOSTIC) || defined(PRO40_HAL_MACH_PROBE))
#error Duplex timing is an isolated silent host-clock experiment, never a hardware backend.
#endif
#if defined(PRO40_HAL_DUPLEX_TIMING_OLD_ORIGIN) && !defined(PRO40_HAL_DUPLEX_TIMING)
#error An aged nominal origin is only allowed in the isolated silent experiment.
#endif
#ifdef PRO40_HAL_PROVISIONING_PROBE
#ifndef PRO40_HAL_DUPLEX_TIMING
#error Provisioning identity probe is only for the isolated silent profile.
#endif
#include "HalProvisioningProbe.hpp"
#include "HalProvisioningProbePins.hpp" // Generated from the sealed test service.
#include <servers/bootstrap.h>
#include <unistd.h>
#endif
#if defined(PRO40_HAL_DUPLEX_AUDIO) && (defined(PRO40_HAL_SYNTHETIC_AUDIO) || defined(PRO40_HAL_FEED_DIAGNOSTIC) || defined(PRO40_HAL_MACH_PROBE))
#error Duplex audio must not be combined with the older audio/Initialize-time feed profiles.
#endif
#ifdef PRO40_HAL_SYNTHETIC_AUDIO
#ifdef PRO40_HAL_FINITE_AUDIO
#include "HalFiniteDriverState.hpp"
#else
#include "HalSyntheticDriverState.hpp"
#endif
#include <unistd.h>
#if defined(PRO40_HAL_FEED_DIAGNOSTIC) || defined(PRO40_HAL_MACH_PROBE)
#error Synthetic audio must not run alongside the Initialize-time feed diagnostics.
#endif
#endif
#ifdef PRO40_HAL_CALLBACK_TIMING
#include "HalCallbackTiming.hpp"
#endif
#ifdef PRO40_HAL_CALLBACK_TIMING_LOG
#include "HalTimingCollector.hpp"
#include <unistd.h>
#endif
#ifdef PRO40_HAL_CALLBACK_TIMING_SUMMARY
#if !defined(PRO40_HAL_CALLBACK_TIMING_LOG) || !defined(PRO40_HAL_DELEGATED_AUDIO)
#error Checked summaries require the delegated diagnostic profile and collector.
#endif
#include "HalTimingSummary.hpp"
#endif
#ifdef PRO40_HAL_MACH_PROBE
#include "HalMachProbe.hpp"
#include <servers/bootstrap.h>
#include <unistd.h>
#endif
#ifdef PRO40_HAL_FEED_DIAGNOSTIC
#include "HalFeedDiagnostic.hpp"
#endif
namespace {
using Properties=Pro40Audio::HalSilenceProperties;
#ifdef PRO40_HAL_DUPLEX_AUDIO
using State=Pro40Audio::HalDuplexDriverState;
#elif defined(PRO40_HAL_SYNTHETIC_AUDIO)
#ifdef PRO40_HAL_FINITE_AUDIO
using State=Pro40Audio::HalFiniteDriverState;
#else
using State=Pro40Audio::HalSyntheticDriverState;
#endif
#else
using State=Pro40Audio::HalSilenceState;
#endif
using Snapshot=Pro40Audio::HalSilenceModel::Snapshot;
CFUUIDRef factoryID() {
#if defined(PRO40_SESSION_PROFILE) && defined(PRO40_HAL_DELEGATED_HARDWARE)
    return CFUUIDGetConstantUUIDWithBytes(nullptr,0xd4,0x8d,0x50,0x72,0xb6,0x61,0x49,0x32,0x8b,0xa1,0xe2,0x7b,0xa7,0x57,0x77,0x8b);
#elif defined(PRO40_SESSION_PROFILE)
    return CFUUIDGetConstantUUIDWithBytes(nullptr,0xb4,0x58,0xd3,0x4a,0xc1,0x16,0x4d,0x8b,0x88,0xde,0xf2,0x7f,0x65,0x16,0xe0,0xa7);
#elif defined(PRO40_HAL_SOURCE_LOSS_FIXTURE)
    return CFUUIDGetConstantUUIDWithBytes(nullptr,0x13,0x9b,0x6a,0x6b,0x21,0x6f,0x48,0xfb,0x92,0x31,0x67,0x8d,0x02,0x62,0x05,0x16);
#elif defined(PRO40_HAL_DELEGATED_HARDWARE)
    return CFUUIDGetConstantUUIDWithBytes(nullptr,0xf8,0xc4,0xaa,0x9c,0x71,0x45,0x4b,0xe0,0xac,0x5a,0x40,0xd8,0xc8,0x10,0xed,0xf9);
#elif defined(PRO40_HAL_DELEGATED_AUDIO)
    return CFUUIDGetConstantUUIDWithBytes(nullptr,0xd0,0xb6,0x6b,0x29,0x54,0x5b,0x43,0xf7,0x87,0xd1,0x79,0xad,0xb1,0x66,0xb8,0xc4);
#elif defined(PRO40_HAL_DUPLEX_TIMING)
    return CFUUIDGetConstantUUIDWithBytes(nullptr,0xce,0x25,0xa6,0x83,0x74,0x13,0x4d,0x79,0x8e,0xc6,0x8f,0xc6,0xa1,0x5b,0x45,0x77);
#else
    return CFUUIDGetConstantUUIDWithBytes(nullptr,0x9e,0xc1,0x68,0x1b,0xd2,0x6e,0x4e,0x92,0xa8,0x8a,0xd5,0x4b,0x34,0x9b,0x19,0x7e);
#endif
}
struct Driver;
#ifdef PRO40_HAL_CALLBACK_TIMING_LOG
#ifdef PRO40_HAL_CALLBACK_TIMING_SUMMARY
using TimingSummary=Pro40Audio::HalTimingSummary<32>;
void timingLog(const Pro40Audio::HalCallbackTimingRecord &r,void *context) {
    static_cast<TimingSummary *>(context)->record(r);
}
void timingSummaryLog(const TimingSummary::Group &g) {
    os_log_error(OS_LOG_DEFAULT,"Pro40Lab timing summary: pid=%{public}d seed=%{public}llu client=%{public}u operation=%{public}u records=%{public}llu frames=%{public}llu faults=%{public}llu anchor=%{public}llu firstHost=%{public}llu lastHost=%{public}llu numer=%{public}u denom=%{public}u minFrames=%{public}u maxFrames=%{public}u firstSample=%{public}.17g lastSampleEnd=%{public}.17g minLead=%{public}.17g maxLead=%{public}.17g",
        getpid(),(unsigned long long)g.seed,g.client,g.operation,(unsigned long long)g.records,
        (unsigned long long)g.frames,(unsigned long long)g.faults,(unsigned long long)g.anchor,
        (unsigned long long)g.firstHost,(unsigned long long)g.lastHost,g.numer,g.denom,g.minFrames,g.maxFrames,
        g.firstSample,g.lastSampleEnd,g.minLead,g.maxLead);
}
#else
void timingLog(const Pro40Audio::HalCallbackTimingRecord &r,void *) {
#if !defined(PRO40_HAL_DUPLEX_TIMING) && !defined(PRO40_HAL_DELEGATED_AUDIO)
    os_log_error(OS_LOG_DEFAULT,"Pro40Lab timing: pid=%{public}d host=%{public}llu anchor=%{public}llu seed=%{public}llu client=%{public}u frames=%{public}u numer=%{public}u denom=%{public}u input=%{public}.17g inputHost=%{public}llu inputFlags=%{public}u current=%{public}.17g currentHost=%{public}llu currentFlags=%{public}u nominal=%{public}u",
        getpid(),(unsigned long long)r.host,(unsigned long long)r.anchor,(unsigned long long)r.seed,
        r.client,r.frames,r.numer,r.denom,r.cycle.mInputTime.mSampleTime,
        (unsigned long long)r.cycle.mInputTime.mHostTime,r.cycle.mInputTime.mFlags,
        r.cycle.mCurrentTime.mSampleTime,(unsigned long long)r.cycle.mCurrentTime.mHostTime,
        r.cycle.mCurrentTime.mFlags,r.cycle.mNominalIOBufferFrameSize);
#else
    os_log_error(OS_LOG_DEFAULT,"Pro40Lab duplex timing: pid=%{public}d host=%{public}llu anchor=%{public}llu seed=%{public}llu client=%{public}u operation=%{public}u frames=%{public}u numer=%{public}u denom=%{public}u input=%{public}.17g inputHost=%{public}llu inputFlags=%{public}u output=%{public}.17g outputHost=%{public}llu outputFlags=%{public}u current=%{public}.17g currentHost=%{public}llu currentFlags=%{public}u nominal=%{public}u",
        getpid(),(unsigned long long)r.host,(unsigned long long)r.anchor,(unsigned long long)r.seed,
        r.client,r.operation,r.frames,r.numer,r.denom,r.cycle.mInputTime.mSampleTime,
        (unsigned long long)r.cycle.mInputTime.mHostTime,r.cycle.mInputTime.mFlags,
        r.cycle.mOutputTime.mSampleTime,(unsigned long long)r.cycle.mOutputTime.mHostTime,r.cycle.mOutputTime.mFlags,
        r.cycle.mCurrentTime.mSampleTime,(unsigned long long)r.cycle.mCurrentTime.mHostTime,
        r.cycle.mCurrentTime.mFlags,r.cycle.mNominalIOBufferFrameSize);
#endif
}
#endif
#endif
AudioServerPlugInDriverInterface makeTable();
AudioServerPlugInDriverInterface table=makeTable();
struct Handle {AudioServerPlugInDriverInterface *interface;Driver *owner;};
static_assert(std::is_standard_layout_v<Handle>);
struct Driver {
    Handle handle{&table,this};
    std::atomic<ULONG> references{1};
    std::atomic<AudioServerPlugInHostRef> host{nullptr};
    std::atomic<UInt64> notificationFailures{0};
    std::atomic<UInt32> propertyErrors{0};
    std::atomic<UInt32> controlEvents{0};
    std::atomic<UInt64> clockSnapshotFailures{0},clockInactiveFailures{0},clockMathFailures{0};
#if defined(PRO40_HAL_SYNTHETIC_AUDIO) || defined(PRO40_HAL_DUPLEX_AUDIO)
    std::atomic<UInt64> inputFailures{0};
#endif
#ifdef PRO40_HAL_DUPLEX_AUDIO
    std::atomic<UInt64> outputFailures{0};
#endif
    bool registeredInstance=false; // Fixed before returning the factory result.
    State state;
#ifdef PRO40_HAL_SOURCE_LOSS
    Pro40Audio::HalSourceLossControl sourceLoss{state};
#endif
#ifdef PRO40_HAL_CALLBACK_TIMING
    Pro40Audio::HalCallbackTiming<256> timing;
    std::atomic<uint64_t> timingSkipped{0};
#ifdef PRO40_HAL_CALLBACK_TIMING_LOG
    // Declared after recorder: collector joins before recorder destruction.
#ifdef PRO40_HAL_CALLBACK_TIMING_SUMMARY
    TimingSummary timingSummary;
    Pro40Audio::HalTimingCollector<256> timingCollector{timing,timingLog,&timingSummary};
#else
    Pro40Audio::HalTimingCollector<256> timingCollector{timing,timingLog,nullptr};
#endif
#endif
#endif
#ifdef PRO40_HAL_FEED_DIAGNOSTIC
    Pro40Audio::HalFeedDiagnostic feed;
#endif
    Driver(uint32_t numer,uint32_t denom):state(numer,denom) {
#ifdef PRO40_HAL_SOURCE_LOSS_FIXTURE
        (void)state.configure(Pro40Audio::HalSourceLossFixture::prepare);
#endif
#ifdef PRO40_HAL_DELEGATED_AUDIO
        // Configure only a local callable here. Lookup, signing and worker
        // preparation occur on StartIO's control path, never during Initialize
        // or an audio callback. The absent broker yields a failed clean start.
        (void)state.configure([](uint64_t seed,uint32_t numer,uint32_t denom)->Pro40Audio::HalDuplexPreparation {
            using namespace Pro40Audio;
            SecRequirementRef raw=nullptr;
            if(SecRequirementCreateWithString(CFSTR(PRO40_PROVISION_BROKER_REQUIREMENT),0,&raw)!=errSecSuccess)return {nullptr,true};
            IdentityDetail::Owned<SecRequirementRef> requirement(raw);
            const auto began=mach_absolute_time();auto controller=ProvisionClient::awaitController(PRO40_PROVISION_SERVICE,requirement.get());
            if(!controller){os_log_error(OS_LOG_DEFAULT,"Pro40Lab delegated controller lookup failed: pid=%{public}d",getpid());return {nullptr,true};}
            auto result=HalDelegatedSession::prepare(std::move(controller),{ProvisionClient::nonce(),seed,numer,denom},nativeDuplexClockPolicy(numer,denom));
            const auto elapsed=uint64_t(__uint128_t(mach_absolute_time()-began)*numer/denom);
            os_log_error(OS_LOG_DEFAULT,"Pro40Lab delegated preparation: pid=%{public}d seed=%{public}llu admitted=%{public}u cleanup=%{public}u elapsed_ns=%{public}llu",
                getpid(),(unsigned long long)seed,unsigned(bool(result.session)),unsigned(result.cleanupOK),(unsigned long long)elapsed);
            return result;
        });
#endif
    }
    ~Driver() {
#ifdef PRO40_HAL_SOURCE_LOSS
        // Stop the monitor before any other member starts destruction. It may
        // be in a host call which reenters StopIO and the timing collector.
        sourceLoss.stopAndJoin();
#endif
    }
};
static_assert(std::atomic<AudioServerPlugInHostRef>::is_always_lock_free);
Driver *get(void *ref) {
    if(!ref)return nullptr;
    auto *handle=static_cast<Handle *>(ref);
    return handle->interface==&table?handle->owner:nullptr;
}
ULONG retain(void *ref) {
    auto *d=get(ref);if(!d)return 0;
    auto count=d->references.load();
    while(count && count<UINT32_MAX)if(d->references.compare_exchange_weak(count,count+1))return count+1;
    return 0;
}
ULONG release(void *ref) {
    auto *d=get(ref);if(!d)return 0;
    const auto count=d->references.fetch_sub(1);
    if(count==1){
        const bool registered=d->registeredInstance;
        delete d;
        // CFPlugIn.h instance accounting; no driver access after deletion.
        if(registered)CFPlugInRemoveInstanceForFactory(factoryID());
        return 0;
    }
    return count-1;
}
bool device(Driver *d,AudioObjectID object) {return d && object==Properties::device && d->host.load();}
OSStatus propertyResult(Driver *d,AudioObjectID object,const AudioObjectPropertyAddress &a,OSStatus result) {
    // Property/control path only. Bound telemetry per instance; never log IO.
    if(result && d && d->propertyErrors.load()<32 && d->propertyErrors.fetch_add(1)<32)
        os_log_error(OS_LOG_DEFAULT,"Pro40Lab property error: object=%{public}u selector=0x%{public}08x scope=0x%{public}08x element=%{public}u status=%{public}d",object,a.mSelector,a.mScope,a.mElement,int(result));
    return result;
}
OSStatus change(AudioServerPlugInDriverRef ref,AudioObjectID object,UInt32 client,State::Change action) {
    auto *d=get(ref);if(!device(d,object))return kAudioHardwareBadObjectError;
    bool runningChanged=false;
    const auto controlHost=mach_absolute_time();auto startHost=controlHost;
#ifdef PRO40_HAL_DUPLEX_TIMING_OLD_ORIGIN
    if(action==State::Change::start) {
        Snapshot before;if(!d->state.read(before) || !before.numer || !before.denom)return kAudioHardwareIllegalOperationError;
        const auto age=uint64_t(__uint128_t(150000000)*before.denom/before.numer);
        if(controlHost<=age)return kAudioHardwareIllegalOperationError;
        startHost-=age; // Fixture only: approximate observed native origin age.
    }
#endif
    const bool changed=d->state.change(action,client,startHost,&runningChanged);
#if (defined(PRO40_HAL_DUPLEX_TIMING) || defined(PRO40_HAL_DELEGATED_AUDIO)) && defined(PRO40_HAL_CALLBACK_TIMING_LOG)
    if(changed && runningChanged && action==State::Change::start) {
        Snapshot after;
        if(d->state.read(after))os_log_error(OS_LOG_DEFAULT,"Pro40Lab duplex start: pid=%{public}d seed=%{public}llu host=%{public}llu anchor=%{public}llu",getpid(),(unsigned long long)after.seed,(unsigned long long)controlHost,(unsigned long long)after.anchor);
    }
#endif
#ifdef PRO40_HAL_DELEGATED_AUDIO
    if(action==State::Change::stop)
        os_log_error(OS_LOG_DEFAULT,"Pro40Lab delegated stop: pid=%{public}d accepted=%{public}u input_failures=%{public}llu output_failures=%{public}llu",
            getpid(),unsigned(changed),(unsigned long long)d->inputFailures.load(),(unsigned long long)d->outputFailures.load());
#endif
#ifdef PRO40_HAL_SYNTHETIC_AUDIO
    if(action==State::Change::start || action==State::Change::stop)
        os_log_error(OS_LOG_DEFAULT,"Pro40Lab synthetic: pid=%{public}d action=%{public}u accepted=%{public}u transition=%{public}u inputFailures=%{public}llu",getpid(),unsigned(action),unsigned(changed),unsigned(runningChanged),(unsigned long long)d->inputFailures.load());
#endif
#ifdef PRO40_HAL_CALLBACK_TIMING_LOG
    if(changed && action==State::Change::stop) {
#ifdef PRO40_HAL_CALLBACK_TIMING_SUMMARY
        d->timingCollector.withDrained([&]{
            d->timingSummary.emit(timingSummaryLog);
            os_log_error(OS_LOG_DEFAULT,"Pro40Lab timing summary counts: pid=%{public}d records=%{public}llu invalid=%{public}llu overflow=%{public}llu",
                getpid(),(unsigned long long)d->timingSummary.records(),(unsigned long long)d->timingSummary.invalid(),(unsigned long long)d->timingSummary.overflow());
        });
#endif
        os_log_error(OS_LOG_DEFAULT,"Pro40Lab timing counts: pid=%{public}d dropped=%{public}llu skipped=%{public}llu",getpid(),(unsigned long long)d->timing.dropped(),(unsigned long long)d->timingSkipped.load());
    }
#endif
    // Real separate-process lifecycle confirmed. Only report rejected control
    // calls or accumulated clock anomalies, outside the realtime callback.
    // Change enum: add=0, remove=1, start=2, stop=3. Never called by audio IO.
    if((!changed || d->clockSnapshotFailures.load() || d->clockInactiveFailures.load() || d->clockMathFailures.load()) &&
       d->controlEvents.load()<128 && d->controlEvents.fetch_add(1)<128)
        os_log_error(OS_LOG_DEFAULT,"Pro40Lab control: action=%{public}u client=%{public}u accepted=%{public}u runningChanged=%{public}u clockSnapshot=%{public}llu clockInactive=%{public}llu clockMath=%{public}llu",unsigned(action),client,unsigned(changed),unsigned(runningChanged),(unsigned long long)d->clockSnapshotFailures.load(),(unsigned long long)d->clockInactiveFailures.load(),(unsigned long long)d->clockMathFailures.load());
    if(!changed)return kAudioHardwareIllegalOperationError;
    // SDK AudioServerPlugIn.h PropertiesChanged: report observable state.
    // State is committed and its mutex released before calling the host, which
    // may synchronously reenter property getters. These are invalidations, not
    // ordered state-value events. Never roll back IO because notification fails.
    if(runningChanged) {
        auto *host=d->host.load();
        const AudioObjectPropertyAddress running{kAudioDevicePropertyDeviceIsRunning,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
        const AudioObjectPropertyAddress active{kAudioStreamPropertyIsActive,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
        if(host->PropertiesChanged(host,Properties::device,1,&running)!=noErr)d->notificationFailures.fetch_add(1);
        if(host->PropertiesChanged(host,Properties::stream,1,&active)!=noErr)d->notificationFailures.fetch_add(1);
        if constexpr(Properties::outputChannels) {
            if(host->PropertiesChanged(host,Properties::outputStream,1,&active)!=noErr)d->notificationFailures.fetch_add(1);
        }
    }
    return noErr;
}
OSStatus ioPhase(AudioServerPlugInDriverRef ref,AudioObjectID object,UInt32 client,UInt32 operation,UInt32 frames,const AudioServerPlugInIOCycleInfo *cycle) {
    auto *d=get(ref);if(!device(d,object))return kAudioHardwareBadObjectError;
    if(operation!=kAudioServerPlugInIOOperationReadInput && operation!=kAudioServerPlugInIOOperationThread && operation!=kAudioServerPlugInIOOperationCycle &&
       !(Properties::outputChannels && operation==kAudioServerPlugInIOOperationWriteMix))return kAudioHardwareIllegalOperationError;
    Snapshot snapshot;
    return cycle && frames<=4096 && d->state.read(snapshot) && snapshot.active(client)?OSStatus(noErr):OSStatus(kAudioHardwareIllegalOperationError);
}
AudioServerPlugInDriverInterface makeTable() {
    AudioServerPlugInDriverInterface t{};
    t.QueryInterface=[](void *ref,REFIID iid,LPVOID *out)->HRESULT {
        if(!out)return E_POINTER;*out=nullptr;if(!get(ref))return E_INVALIDARG;
        CFUUIDRef uuid=CFUUIDCreateFromUUIDBytes(nullptr,iid);if(!uuid)return E_OUTOFMEMORY;
        const bool supported=CFEqual(uuid,IUnknownUUID) || CFEqual(uuid,kAudioServerPlugInDriverInterfaceUUID);CFRelease(uuid);
        if(!supported)return E_NOINTERFACE;
        if(!retain(ref))return E_FAIL;*out=ref;return S_OK;
    };
    t.AddRef=retain;t.Release=release;
    t.Initialize=[](AudioServerPlugInDriverRef ref,AudioServerPlugInHostRef host)->OSStatus {
        auto *d=get(ref);if(!d || !host || !host->PropertiesChanged)return kAudioHardwareIllegalOperationError;
#ifdef PRO40_HAL_SOURCE_LOSS
        if(!host->RequestDeviceConfigurationChange)return kAudioHardwareIllegalOperationError;
#endif
        AudioServerPlugInHostRef expected=nullptr;
        const bool first=d->host.compare_exchange_strong(expected,host);
#ifdef PRO40_HAL_SOURCE_LOSS
        if(first && !d->sourceLoss.start([](void *context,uint64_t action)->OSStatus {
            auto *owner=static_cast<Driver *>(context);auto *host=owner->host.load();
            os_log_error(OS_LOG_DEFAULT,"Pro40Lab source loss requesting device configuration");
            return host->RequestDeviceConfigurationChange(host,Properties::device,action,nullptr);
        },d)) {
            d->host.store(nullptr);return kAudioHardwareUnspecifiedError;
        }
#endif
#ifdef PRO40_HAL_PROVISIONING_PROBE
        if(first) {
            // One control-path diagnostic, no retained task/view or audio grant.
            // Bootstrap/Security API calls have no preemption guarantee. This
            // experiment must not be used as production realtime/startup policy.
            auto requirement=Pro40Audio::ProvisioningProbe::requirement(CFSTR(PRO40_PROVISION_SERVER_REQUIREMENT));
            Pro40Audio::NativeMachPort service(MACH_PORT_RIGHT_SEND);mach_port_t raw=MACH_PORT_NULL;
            const auto lookup=bootstrap_look_up(bootstrap_port,Pro40Audio::ProvisioningProbe::serviceName,&raw);
            Pro40Audio::ProvisioningProbe::Result result;
            if(lookup==KERN_SUCCESS && service.adopt(raw)==KERN_SUCCESS && requirement)
                result=Pro40Audio::ProvisioningProbe::exchange(service.get(),requirement.get(),mach_absolute_time(),2000);
            os_log_error(OS_LOG_DEFAULT,"Pro40Lab provisioning probe: pid=%{public}d lookup=%{public}d transport=%{public}d server_identity=%{public}d client_identity=%{public}d passed=%{public}u",getpid(),int(lookup),int(result.transport),int(result.serverIdentity),int(result.clientIdentity),unsigned(result.passed()));
        }
#endif
#ifdef PRO40_HAL_CALLBACK_TIMING_LOG
        if(first)os_log_error(OS_LOG_DEFAULT,"Pro40Lab timing start: pid=%{public}d started=%{public}u safety=%{public}u",getpid(),unsigned(d->timingCollector.start()),Properties::inputSafetyOffset);
#endif
#ifdef PRO40_HAL_FEED_DIAGNOSTIC
        if(first) {
#ifdef PRO40_HAL_FEED_SOURCE_LOSS
            const auto status=d->feed.start("local.pro40.hal-feed-lab",true);
#else
            const auto status=d->feed.start();
#endif
            os_log_error(OS_LOG_DEFAULT,"Pro40Lab feed start: pid=%{public}d status=%{public}d",getpid(),status);
        }
#endif
#ifdef PRO40_HAL_MACH_PROBE
        if(first) {
            // Opt-in diagnostic during Initialize, before IO. Entire operation
            // is synchronous: no callback/thread or borrowed driver view survives
            // this call. bootstrap lookup itself has no timeout API; message
            // send/receive each have 250 ms limits. Never call from audio IO.
            mach_port_t service=MACH_PORT_NULL;
            const auto lookup=bootstrap_look_up(bootstrap_port,Pro40Audio::MachProbe::serviceName,&service);
            kern_return_t exchange=KERN_FAILURE;
            kern_return_t audio=KERN_FAILURE;
            if(lookup==KERN_SUCCESS) {
                exchange=Pro40Audio::MachProbe::exchange(service,uint64_t(getpid()));
                if(exchange==KERN_SUCCESS)audio=Pro40Audio::MachProbe::exchangeAudio(service,uint64_t(getpid()));
                mach_port_deallocate(mach_task_self(),service);
            }
            os_log_error(OS_LOG_DEFAULT,"Pro40Lab Mach probe: pid=%{public}d lookup=%{public}d exchange=%{public}d",getpid(),lookup,exchange);
            os_log_error(OS_LOG_DEFAULT,"Pro40Lab Mach audio: pid=%{public}d status=%{public}d frames=%{public}u",getpid(),audio,audio==KERN_SUCCESS?512u:0u);
        }
#endif
        return first || expected==host?OSStatus(noErr):OSStatus(kAudioHardwareIllegalOperationError);
    };
    t.CreateDevice=[](AudioServerPlugInDriverRef,CFDictionaryRef,const AudioServerPlugInClientInfo *,AudioObjectID *out)->OSStatus {
        if(out)*out=kAudioObjectUnknown;return kAudioHardwareUnsupportedOperationError;
    };
    t.DestroyDevice=[](AudioServerPlugInDriverRef,AudioObjectID)->OSStatus {return kAudioHardwareUnsupportedOperationError;};
    t.AddDeviceClient=[](AudioServerPlugInDriverRef ref,AudioObjectID object,const AudioServerPlugInClientInfo *client)->OSStatus {
        return client?change(ref,object,client->mClientID,State::Change::add):OSStatus(kAudioHardwareIllegalOperationError);
    };
    t.RemoveDeviceClient=[](AudioServerPlugInDriverRef ref,AudioObjectID object,const AudioServerPlugInClientInfo *client)->OSStatus {
        return client?change(ref,object,client->mClientID,State::Change::remove):OSStatus(kAudioHardwareIllegalOperationError);
    };
#ifdef PRO40_HAL_SOURCE_LOSS
    t.PerformDeviceConfigurationChange=[](AudioServerPlugInDriverRef ref,AudioObjectID object,UInt64 action,void *info)->OSStatus {
        auto *d=get(ref);if(!device(d,object))return kAudioHardwareBadObjectError;
        const auto status=d->sourceLoss.perform(action,info);
        if(status==noErr) {
            // With IO quiesced, withdraw the failed device from the plug-in's
            // published list. IsAlive-only changes/notifications left Sequoia's
            // application value at 1 (source-loss-host-Z9XfIW and SgeKpv).
            // Existing object storage remains owned until final driver Release.
            const AudioObjectPropertyAddress removed[]{
                {kAudioPlugInPropertyDeviceList,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain},
                {kAudioObjectPropertyOwnedObjects,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain}};
            auto *host=d->host.load();
            if(host->PropertiesChanged(host,Properties::plugin,2,removed)!=noErr)d->notificationFailures.fetch_add(1);
        }
        return status;
    };
    t.AbortDeviceConfigurationChange=[](AudioServerPlugInDriverRef ref,AudioObjectID object,UInt64 action,void *info)->OSStatus {
        auto *d=get(ref);if(!device(d,object))return kAudioHardwareBadObjectError;
        return d->sourceLoss.abort(action,info);
    };
#else
    t.PerformDeviceConfigurationChange=[](AudioServerPlugInDriverRef,AudioObjectID,UInt64,void *)->OSStatus {return kAudioHardwareUnsupportedOperationError;};
    t.AbortDeviceConfigurationChange=t.PerformDeviceConfigurationChange;
#endif
    t.HasProperty=[](AudioServerPlugInDriverRef ref,AudioObjectID object,pid_t,const AudioObjectPropertyAddress *a)->Boolean {
        return get(ref) && a && Properties::lookup(object,*a).has_value();
    };
    t.IsPropertySettable=[](AudioServerPlugInDriverRef ref,AudioObjectID object,pid_t,const AudioObjectPropertyAddress *a,Boolean *out)->OSStatus {
        if(!get(ref) || !Properties::valid(object))return kAudioHardwareBadObjectError;
        if(!a || !out)return kAudioHardwareIllegalOperationError;
        if(!Properties::lookup(object,*a))return kAudioHardwareUnknownPropertyError;
        *out=false;return noErr;
    };
    t.GetPropertyDataSize=[](AudioServerPlugInDriverRef ref,AudioObjectID object,pid_t,const AudioObjectPropertyAddress *a,UInt32 n,const void *q,UInt32 *out)->OSStatus {
        auto *d=get(ref);if(!d)return kAudioHardwareBadObjectError;
        if(!a || !out)return kAudioHardwareIllegalOperationError;
        bool alive=true;
#ifdef PRO40_HAL_SOURCE_LOSS
        alive=d->sourceLoss.alive();
#endif
        *out=0;Properties::Value value;const auto status=Properties::resolve(object,*a,n,q,false,value,alive);
        if(!status)*out=value.size;return propertyResult(d,object,*a,status);
    };
    t.GetPropertyData=[](AudioServerPlugInDriverRef ref,AudioObjectID object,pid_t,const AudioObjectPropertyAddress *a,UInt32 n,const void *q,UInt32 capacity,UInt32 *used,void *out)->OSStatus {
        auto *d=get(ref);if(!d)return kAudioHardwareBadObjectError;
        if(!a || !used)return kAudioHardwareIllegalOperationError;
        *used=0;Snapshot s;if(!d->state.read(s))return kAudioHardwareIllegalOperationError;
        bool alive=true;
#ifdef PRO40_HAL_SOURCE_LOSS
        alive=d->sourceLoss.alive();
#endif
        return propertyResult(d,object,*a,Properties::get(object,*a,n,q,s.running(),capacity,used,out,alive));
    };
    t.SetPropertyData=[](AudioServerPlugInDriverRef ref,AudioObjectID object,pid_t,const AudioObjectPropertyAddress *a,UInt32,const void *,UInt32,const void *)->OSStatus {
        if(!get(ref) || !Properties::valid(object))return kAudioHardwareBadObjectError;
        if(!a)return kAudioHardwareIllegalOperationError;
        return Properties::lookup(object,*a)?OSStatus(kAudioHardwareIllegalOperationError):OSStatus(kAudioHardwareUnknownPropertyError);
    };
    t.StartIO=[](AudioServerPlugInDriverRef ref,AudioObjectID object,UInt32 client)->OSStatus {return change(ref,object,client,State::Change::start);};
    t.StopIO=[](AudioServerPlugInDriverRef ref,AudioObjectID object,UInt32 client)->OSStatus {return change(ref,object,client,State::Change::stop);};
    t.GetZeroTimeStamp=[](AudioServerPlugInDriverRef ref,AudioObjectID object,UInt32 client,Float64 *sample,UInt64 *host,UInt64 *seed)->OSStatus {
        auto *d=get(ref);if(!device(d,object))return kAudioHardwareBadObjectError;
        Snapshot s;Pro40Audio::HalSilenceModel::Stamp stamp;
        if(!sample || !host || !seed)return kAudioHardwareIllegalOperationError;
        // AudioServerPlugIn.h:991–1000: the requester must be registered, not
        // individually started. Real host requests B's clock while A owns IO.
        // Counter-only anomalies here; control events above report totals.
        if(!d->state.read(s)){d->clockSnapshotFailures.fetch_add(1);return kAudioHardwareIllegalOperationError;}
        if(!s.known(client) || !s.running()){d->clockInactiveFailures.fetch_add(1);return kAudioHardwareIllegalOperationError;}
#if defined(PRO40_HAL_SYNTHETIC_AUDIO) || defined(PRO40_HAL_DUPLEX_AUDIO)
        if(!d->state.timestamp(client,mach_absolute_time(),stamp)){d->clockMathFailures.fetch_add(1);return kAudioHardwareIllegalOperationError;}
#else
        if(!Pro40Audio::HalSilenceModel::timestamp(s,mach_absolute_time(),stamp)){d->clockMathFailures.fetch_add(1);return kAudioHardwareIllegalOperationError;}
#endif
        *sample=stamp.sample;*host=stamp.host;*seed=stamp.seed;return noErr;
    };
    t.WillDoIOOperation=[](AudioServerPlugInDriverRef ref,AudioObjectID object,UInt32 client,UInt32 operation,Boolean *will,Boolean *inPlace)->OSStatus {
        auto *d=get(ref);if(!device(d,object))return kAudioHardwareBadObjectError;
        Snapshot s;if(!will || !inPlace || !d->state.read(s) || !s.known(client))return kAudioHardwareIllegalOperationError;
        *will=operation==kAudioServerPlugInIOOperationReadInput ||
            (Properties::outputChannels && operation==kAudioServerPlugInIOOperationWriteMix);*inPlace=true;return noErr;
    };
    t.BeginIOOperation=ioPhase;t.EndIOOperation=ioPhase;
    t.DoIOOperation=[](AudioServerPlugInDriverRef ref,AudioObjectID object,AudioObjectID stream,UInt32 client,UInt32 operation,UInt32 frames,const AudioServerPlugInIOCycleInfo *cycle,void *out,void *secondary)->OSStatus {
        const bool input=operation==kAudioServerPlugInIOOperationReadInput;
        const bool output=Properties::outputChannels && operation==kAudioServerPlugInIOOperationWriteMix;
        if(!input && !output)return kAudioHardwareIllegalOperationError;
        if(stream!=(input?Properties::stream:Properties::outputStream))return kAudioHardwareBadObjectError;
        if(!out || secondary)return kAudioHardwareIllegalOperationError;
        const auto status=ioPhase(ref,object,client,operation,frames,cycle);if(status)return status;
#ifdef PRO40_HAL_CALLBACK_TIMING
        // Measurement only: preserve the snapshot used for this observation.
        // Does not grant audio-session admission or replace ioPhase validation.
        auto *d=get(ref);Snapshot snapshot;
        if(d->state.read(snapshot) && snapshot.active(client)) {
            Pro40Audio::HalCallbackTimingRecord observation;
            observation.host=mach_absolute_time();observation.anchor=snapshot.anchor;
            observation.seed=snapshot.seed;observation.client=client;observation.frames=frames;
            observation.numer=snapshot.numer;observation.denom=snapshot.denom;observation.cycle=*cycle;
            observation.operation=operation;
            d->timing.record(observation);
        } else d->timingSkipped.fetch_add(1,std::memory_order_relaxed);
#endif
#ifdef PRO40_HAL_DUPLEX_AUDIO
        if(!frames)return kAudioHardwareIllegalOperationError;
        if(output) {
            // SDK AudioServerPlugIn.h:339-343: one fully mixed, in-place output
            // interval. Host memory is copied into owned PCM, never retained.
            auto *d=get(ref);
            if(!d->state.writeOutput(client,*cycle,frames,std::span(static_cast<const Float32 *>(out),frames*Properties::outputChannels))) {
                d->outputFailures.fetch_add(1,std::memory_order_relaxed);return kAudioHardwareUnspecifiedError;
            }
            return noErr;
        }
#elif defined(PRO40_HAL_DUPLEX_TIMING)
        // Measurement profile only: no output mutation, queue or device access.
        if(output)return noErr;
#endif
#if defined(PRO40_HAL_SYNTHETIC_AUDIO) || defined(PRO40_HAL_DUPLEX_AUDIO)
        if(!get(ref)->state.readInput(client,*cycle,frames,std::span(static_cast<Float32 *>(out),frames*Properties::inputChannels)).ok())
            get(ref)->inputFailures.fetch_add(1,std::memory_order_relaxed);
#else
        std::fill_n(static_cast<Float32 *>(out),frames*Properties::inputChannels,0.0f);
#endif
        return noErr;
    };
    return t;
}
}
extern "C" void *Pro40SilenceFactory(CFAllocatorRef,CFUUIDRef type) {
    if(!type || !CFEqual(type,kAudioServerPlugInTypeUUID))return nullptr;
    mach_timebase_info_data_t timebase{};
    if(mach_timebase_info(&timebase)!=KERN_SUCCESS || !timebase.numer || !timebase.denom)return nullptr;
    auto *d=new(std::nothrow) Driver(timebase.numer,timebase.denom);
    return d?&d->handle.interface:nullptr;
}
extern "C" UInt64 Pro40SilenceNotificationFailures(AudioServerPlugInDriverRef ref) {
    auto *d=get(ref);return d?d->notificationFailures.load():0;
}
#ifdef PRO40_HAL_DUPLEX_AUDIO
bool Pro40DuplexConfigure(AudioServerPlugInDriverRef ref,Pro40Audio::HalDuplexPrepare prepare) {
    auto *d=get(ref);return d && d->state.configure(std::move(prepare));
}
extern "C" UInt64 Pro40DuplexInputFailures(AudioServerPlugInDriverRef ref) {
    auto *d=get(ref);return d?d->inputFailures.load():0;
}
extern "C" UInt64 Pro40DuplexOutputFailures(AudioServerPlugInDriverRef ref) {
    auto *d=get(ref);return d?d->outputFailures.load():0;
}
#endif
#ifdef PRO40_HAL_SYNTHETIC_AUDIO
extern "C" bool Pro40SyntheticConfigure(AudioServerPlugInDriverRef ref,mach_port_t service) {
    auto *d=get(ref);return d && d->state.configure(service);
}
extern "C" UInt64 Pro40SyntheticInputFailures(AudioServerPlugInDriverRef ref) {
    auto *d=get(ref);return d?d->inputFailures.load():0;
}
#endif
#if defined(PRO40_HAL_CALLBACK_TIMING) && !defined(PRO40_HAL_CALLBACK_TIMING_LOG)
// Single control/test consumer only; caller owns driver lifetime. No callback IO.
extern "C" size_t Pro40SilenceDrainTiming(AudioServerPlugInDriverRef ref,
    void (*consumer)(const Pro40Audio::HalCallbackTimingRecord *,void *),void *context,UInt64 *dropped) {
    auto *d=get(ref);if(!d || !consumer || !dropped)return 0;
    *dropped=d->timing.dropped();
    return d->timing.drain([&](const auto &record){consumer(&record,context);});
}
#endif
// Registered bundle entry point. The direct lab factory above shares every
// callback but does not require a process-global CFPlugIn registration.
extern "C" void *Pro40SilenceRegisteredFactory(CFAllocatorRef allocator,CFUUIDRef type) {
    auto *ref=Pro40SilenceFactory(allocator,type);
    if(ref) {
        CFPlugInAddInstanceForFactory(factoryID());
        get(ref)->registeredInstance=true;
    }
    return ref;
}
