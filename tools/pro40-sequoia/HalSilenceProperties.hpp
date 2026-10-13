// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <CoreAudio/AudioServerPlugIn.h>
#include "HalSilenceModel.hpp"
#include <array>
#include <cstring>
#include <optional>
#if defined(PRO40_HAL_DELEGATED_HARDWARE) && !defined(PRO40_HAL_DELEGATED_AUDIO)
#error Hardware HAL requires the delegated session lifecycle.
#endif
#if defined(PRO40_SESSION_PROFILE) && !defined(PRO40_HAL_DELEGATED_AUDIO)
#error Session HAL properties require delegated audio.
#endif

namespace Pro40Audio {
// Fixed-format fake-host property model, not a complete host-loadable driver.
// SDK contracts: AudioHardwareBase.h owned-object qualifiers (303ff), device
// properties (650ff), stream scopes/formats (1075ff); AudioServerPlugIn.h getters.
template<UInt32 Inputs=1,UInt32 Outputs=0> class BasicHalAudioProperties {
    static_assert(Inputs>=1 && Inputs<=20 && Outputs<=12);
public:
    static constexpr UInt32 inputChannels=Inputs,outputChannels=Outputs;
#if defined(PRO40_HAL_DUPLEX_TIMING_SAFETY) || defined(PRO40_HAL_DELEGATED_AUDIO)
#if !defined(PRO40_HAL_DUPLEX_TIMING) && !defined(PRO40_HAL_DELEGATED_AUDIO)
#error Duplex safety values require an explicit timing or delegated profile.
#endif
    // Candidate scheduling offsets, not measured ADC/DAC latency. The delegated
    // profiles keep scheduling offsets distinct. SDK AudioHardwareBase.h:705-707.
    static constexpr UInt32 inputSafetyOffset=2816,outputSafetyOffset=8192;
#else
#if defined(PRO40_HAL_TIMING_SAFETY) || defined(PRO40_HAL_SYNTHETIC_AUDIO)
    static constexpr UInt32 inputSafetyOffset=2048; // Fixed experimental variant.
#else
    static constexpr UInt32 inputSafetyOffset=0;
#endif
    static constexpr UInt32 outputSafetyOffset=0;
#endif
    static constexpr AudioObjectID plugin=kAudioObjectPlugInObject,device=2,stream=3,outputStream=4;
    static bool isStream(AudioObjectID object) {return object==stream || (Outputs && object==outputStream);}
    static CFStringRef deviceUID() {
#ifdef PRO40_SESSION_PROFILE
#ifdef PRO40_HAL_DELEGATED_HARDWARE
        return CFSTR("local.pro40.session.device");
#else
        return CFSTR("local.pro40.session-fixture.device");
#endif
#elif defined(PRO40_HAL_SOURCE_LOSS_FIXTURE)
        return CFSTR("local.pro40.source-loss-lab.device");
#elif defined(PRO40_HAL_DELEGATED_HARDWARE)
        return CFSTR("local.pro40.native-lab.device");
#elif defined(PRO40_HAL_DELEGATED_AUDIO)
        return CFSTR("local.pro40.delegated-lab.device");
#elif defined(PRO40_HAL_DUPLEX_TIMING)
        return CFSTR("local.pro40.duplex-timing-lab.device");
#else
        return Outputs?CFSTR("local.pro40.duplex-lab.device"):CFSTR("local.pro40.silence-lab.device");
#endif
    }
    struct Value {
        std::array<unsigned char,sizeof(AudioStreamRangedDescription)> bytes{};
        UInt32 size=0;CFStringRef string=nullptr;
        template<class T> static Value data(const T &data) {
            static_assert(sizeof(T)<=sizeof(bytes));Value v;v.size=sizeof(T);
            std::memcpy(v.bytes.data(),&data,sizeof(T));return v;
        }
        static Value text(CFStringRef text) {Value v;v.string=text;v.size=sizeof(CFStringRef);return v;}
    };
    static bool valid(AudioObjectID object) {return object==plugin || object==device || isStream(object);}
    static AudioStreamBasicDescription format(AudioObjectID object=stream) {
        const auto channels=object==outputStream?Outputs:Inputs;
        return {48000,kAudioFormatLinearPCM,kAudioFormatFlagsNativeFloatPacked,4*channels,1,4*channels,channels,32,0};
    }
    static std::optional<Value> lookup(AudioObjectID object,const AudioObjectPropertyAddress &a,bool running=false,bool alive=true) {
        if(!valid(object))return {};
#ifdef PRO40_SESSION_PROFILE
        // User's fixed physical 1..6 order. Per-channel names are scoped to the
        // device output elements, not six independent CoreAudio streams.
        if(object==device && a.mScope==kAudioObjectPropertyScopeOutput &&
           a.mSelector==kAudioObjectPropertyName && a.mElement>=1 && a.mElement<=Outputs) {
            static_assert(Outputs==6);
            const CFStringRef names[]={CFSTR("1 — Left"),CFSTR("2 — Right"),CFSTR("3 — Centre"),
                CFSTR("4 — LFE"),CFSTR("5 — Left Surround"),CFSTR("6 — Right Surround")};
            return Value::text(names[a.mElement-1]);
        }
#endif
        if(a.mElement!=kAudioObjectPropertyElementMain)return {};
        const bool global=a.mScope==kAudioObjectPropertyScopeGlobal;
        const bool input=a.mScope==kAudioObjectPropertyScopeInput,output=a.mScope==kAudioObjectPropertyScopeOutput;
        if(!global && !(object==device && (input || output)))return {};
        if(global)switch(a.mSelector) {
            case kAudioObjectPropertyBaseClass:return Value::data(UInt32(kAudioObjectClassID));
            case kAudioObjectPropertyClass:return Value::data(object==plugin?UInt32(kAudioPlugInClassID):object==device?UInt32(kAudioDeviceClassID):UInt32(kAudioStreamClassID));
            case kAudioObjectPropertyOwner:return Value::data(UInt32(object==plugin?kAudioObjectUnknown:object==device?plugin:device));
            case kAudioObjectPropertyName:
#ifdef PRO40_SESSION_PROFILE
#ifdef PRO40_HAL_DELEGATED_HARDWARE
                return Value::text(object==stream?CFSTR("Analogue Inputs 1–8"):object==outputStream?CFSTR("Surround Outputs 1–6"):CFSTR("Saffire Pro 40 (Experimental)"));
#else
                return Value::text(CFSTR("Pro 40 Session Fixture (No Hardware)"));
#endif
#elif defined(PRO40_HAL_SOURCE_LOSS_FIXTURE)
                return Value::text(CFSTR("Pro40 Source Loss Test (No Hardware)"));
#elif defined(PRO40_HAL_DELEGATED_HARDWARE)
                return Value::text(object==stream?CFSTR("Microphone Inputs"):object==outputStream?CFSTR("Line Outputs"):CFSTR("Pro40 Native Connection Test"));
#elif defined(PRO40_HAL_DELEGATED_AUDIO)
                return Value::text(object==stream?CFSTR("Generated Test Inputs"):object==outputStream?CFSTR("Test Outputs"):CFSTR("Pro40 Connection Test (No Hardware)"));
#endif
#ifdef PRO40_HAL_DUPLEX_TIMING
                return Value::text(object==stream?CFSTR("Silent Test Inputs"):object==outputStream?CFSTR("Discarded Test Outputs"):CFSTR("Pro40 Timing Test (No Hardware)"));
#endif
                if constexpr(Outputs) return Value::text(object==stream?CFSTR("Microphone Inputs"):object==outputStream?CFSTR("Line Outputs"):CFSTR("Pro40 Duplex Lab"));
                return Value::text(object==stream?CFSTR("Silent Input"):CFSTR("Pro40 Silence Lab"));
            case kAudioObjectPropertyManufacturer:return Value::text(CFSTR("Local Pro40 Development"));
            case kAudioObjectPropertyOwnedObjects:
                if(isStream(object))return Value{};
                if(object==plugin)return alive?Value::data(device):Value{};
                if constexpr(Outputs)return Value::data(std::array<AudioObjectID,2>{stream,outputStream});
                return Value::data(stream);
        }
        if(object==plugin && global)switch(a.mSelector) {
            case kAudioPlugInPropertyDeviceList:return alive?Value::data(device):Value{};
            case kAudioPlugInPropertyBundleID:
#ifdef PRO40_SESSION_PROFILE
                return Value::text(
#ifdef PRO40_HAL_DELEGATED_HARDWARE
                    CFSTR("local.pro40.session")
#else
                    CFSTR("local.pro40.session-fixture")
#endif
                );
#elif defined(PRO40_HAL_SOURCE_LOSS_FIXTURE)
                return Value::text(CFSTR("local.pro40.source-loss-lab"));
#elif defined(PRO40_HAL_DELEGATED_HARDWARE)
                return Value::text(CFSTR("local.pro40.native-lab"));
#elif defined(PRO40_HAL_DELEGATED_AUDIO)
                return Value::text(CFSTR("local.pro40.delegated-lab"));
#elif defined(PRO40_HAL_DUPLEX_TIMING)
                return Value::text(CFSTR("local.pro40.duplex-timing-lab"));
#else
                return Value::text(Outputs?CFSTR("local.pro40.duplex-lab"):CFSTR("local.pro40.silence-lab"));
#endif
            case kAudioPlugInPropertyTranslateUIDToDevice:return Value::data(AudioObjectID(kAudioObjectUnknown));
            case kAudioPlugInPropertyBoxList:
            case kAudioPlugInPropertyClockDeviceList:return Value{};
        }
        if(object==device) {
            if(a.mSelector==kAudioDevicePropertyStreams) {
                if(output)return Outputs?Value::data(outputStream):Value{};
                if constexpr(Outputs)if(global)return Value::data(std::array<AudioObjectID,2>{stream,outputStream});
                return Value::data(stream);
            }
            if((input || output) && (a.mSelector==kAudioDevicePropertyDeviceCanBeDefaultDevice || a.mSelector==kAudioDevicePropertyDeviceCanBeDefaultSystemDevice)) {
#if defined(PRO40_SESSION_PROFILE) && defined(PRO40_HAL_DELEGATED_HARDWARE)
                return Value::data(UInt32(a.mSelector==kAudioDevicePropertyDeviceCanBeDefaultDevice));
#else
                return Value::data(UInt32(0));
#endif
            }
#ifdef PRO40_SESSION_PROFILE
            // SDK AudioHardwareBase.h:722-729; CoreAudioBaseTypes.h:1225.
            // Tagged layout has a 12-byte header and zero channel descriptions.
            if(output && a.mSelector==kAudioDevicePropertyPreferredChannelsForStereo)
                return Value::data(std::array<UInt32,2>{1,2});
            if(output && a.mSelector==kAudioDevicePropertyPreferredChannelLayout)
                return Value::data(std::array<UInt32,3>{kAudioChannelLayoutTag_MPEG_5_1_A,0,0});
#endif
            // The real Sequoia host queries output safety offset even for this
            // input-only device (system-test-20260925-125552). Lab profiles
            // report uncalibrated zero latency. Only the opt-in timing experiment changes
            // safety; do not also shift sample timestamps or stream latency.
            if((input || output) && a.mSelector==kAudioDevicePropertyLatency)return Value::data(UInt32(0));
            if((input || output) && a.mSelector==kAudioDevicePropertySafetyOffset)
                return Value::data(input?inputSafetyOffset:outputSafetyOffset);
            if(global)switch(a.mSelector) {
                case kAudioDevicePropertyDeviceUID:return Value::text(deviceUID());
                case kAudioDevicePropertyModelUID:
#ifdef PRO40_SESSION_PROFILE
                    return Value::text(
#ifdef PRO40_HAL_DELEGATED_HARDWARE
                        CFSTR("local.pro40.session.model")
#else
                        CFSTR("local.pro40.session-fixture.model")
#endif
                    );
#elif defined(PRO40_HAL_SOURCE_LOSS_FIXTURE)
                    return Value::text(CFSTR("local.pro40.source-loss-lab.model"));
#elif defined(PRO40_HAL_DELEGATED_HARDWARE)
                    return Value::text(CFSTR("local.pro40.native-lab.model"));
#elif defined(PRO40_HAL_DELEGATED_AUDIO)
                    return Value::text(CFSTR("local.pro40.delegated-lab.model"));
#elif defined(PRO40_HAL_DUPLEX_TIMING)
                    return Value::text(CFSTR("local.pro40.duplex-timing-lab.model"));
#else
                    return Value::text(Outputs?CFSTR("local.pro40.duplex-lab.model"):CFSTR("local.pro40.silence-lab.model"));
#endif
                case kAudioDevicePropertyTransportType:return Value::data(UInt32(kAudioDeviceTransportTypeVirtual));
                case kAudioDevicePropertyClockDomain:return Value::data(UInt32(0));
                case kAudioDevicePropertyDeviceIsAlive:return Value::data(UInt32(alive));
                case kAudioDevicePropertyIsHidden:return Value::data(UInt32(0));
                case kAudioDevicePropertyRelatedDevices:return Value::data(device);
                case kAudioDevicePropertyDeviceIsRunning:return Value::data(UInt32(running));
                case kAudioDevicePropertyNominalSampleRate:return Value::data(Float64(48000));
                case kAudioDevicePropertyAvailableNominalSampleRates:return Value::data(AudioValueRange{48000,48000});
                case kAudioDevicePropertyZeroTimeStampPeriod:return Value::data(UInt32(HalSilenceModel::timestampPeriod));
                case kAudioObjectPropertyControlList:return Value{};
            }
        }
        if(isStream(object) && global)switch(a.mSelector) {
            case kAudioStreamPropertyDirection:return Value::data(UInt32(object==stream));
            case kAudioStreamPropertyStartingChannel:return Value::data(UInt32(1));
            case kAudioStreamPropertyIsActive:return Value::data(UInt32(running));
            case kAudioStreamPropertyTerminalType:return Value::data(UInt32(kAudioStreamTerminalTypeUnknown));
            case kAudioStreamPropertyLatency:return Value::data(UInt32(0));
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:return Value::data(format(object));
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyAvailablePhysicalFormats:return Value::data(AudioStreamRangedDescription{format(object),{48000,48000}});
        }
        return {};
    }
    static OSStatus resolve(AudioObjectID object,const AudioObjectPropertyAddress &a,UInt32 qsize,const void *qualifier,bool running,Value &value,bool alive=true) {
        if(!valid(object))return kAudioHardwareBadObjectError;
        const auto found=lookup(object,a,running,alive);if(!found)return kAudioHardwareUnknownPropertyError;
        value=*found;
        // AudioHardwareBase.h:375–381: UID is a qualifier CFStringRef;
        // an unmatched UID succeeds with kAudioObjectUnknown.
        if(a.mSelector==kAudioPlugInPropertyTranslateUIDToDevice) {
            if(qsize!=sizeof(CFStringRef) || !qualifier)return kAudioHardwareBadPropertySizeError;
            CFStringRef uid=nullptr;std::memcpy(&uid,qualifier,sizeof(uid));
            if(!uid || CFGetTypeID(uid)!=CFStringGetTypeID())return kAudioHardwareIllegalOperationError;
            value=Value::data(AudioObjectID(alive && CFEqual(uid,deviceUID())?device:kAudioObjectUnknown));
            return noErr;
        }
        if(!qsize)return noErr;
        if(a.mSelector!=kAudioObjectPropertyOwnedObjects || !qualifier || qsize%sizeof(AudioClassID))return kAudioHardwareBadPropertySizeError;
        bool match=false;
        const auto childClass=object==plugin?AudioClassID(kAudioDeviceClassID):AudioClassID(kAudioStreamClassID);
        for(UInt32 offset=0;offset<qsize;offset+=sizeof(AudioClassID)) {
            AudioClassID candidate;std::memcpy(&candidate,static_cast<const unsigned char *>(qualifier)+offset,sizeof(candidate));
            match|=candidate==childClass || candidate==kAudioObjectClassID;
        }
        if(!match)value=Value{};
        return noErr;
    }
    static OSStatus get(AudioObjectID object,const AudioObjectPropertyAddress &a,UInt32 qsize,const void *qualifier,bool running,UInt32 capacity,UInt32 *used,void *output,bool alive=true) {
        if(!used)return kAudioHardwareIllegalOperationError;
        *used=0;Value value;const auto status=resolve(object,a,qsize,qualifier,running,value,alive);
        if(status)return status;
        if(capacity<value.size || (value.size && !output))return kAudioHardwareBadPropertySizeError;
        if(value.string) {
            const auto retained=static_cast<CFStringRef>(CFRetain(value.string));
            std::memcpy(output,&retained,sizeof(retained));
        } else if(value.size)std::memcpy(output,value.bytes.data(),value.size);
        *used=value.size;return noErr;
    }
};
#if defined(PRO40_HAL_DUPLEX_AUDIO) || defined(PRO40_HAL_DUPLEX_TIMING)
using HalSilenceProperties=BasicHalAudioProperties<8,6>;
#else
using HalSilenceProperties=BasicHalAudioProperties<>;
#endif
}
