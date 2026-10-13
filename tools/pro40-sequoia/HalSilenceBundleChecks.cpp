// SPDX-License-Identifier: Apache-2.0
#include <CoreAudio/AudioServerPlugIn.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <array>
#include <cstdlib>
int main(int argc,char **argv) {
    assert(argc>=2 && argc<=5);
    const UInt32 expectedSafety=argc>=3?UInt32(std::strtoul(argv[2],nullptr,10)):0;
    const bool hardware=argc>=4 && std::strcmp(argv[3],"--delegated-hardware")==0;
    const bool sourceLoss=argc>=4 && std::strcmp(argv[3],"--source-loss")==0;
    const bool delegated=hardware || (argc>=4 && std::strcmp(argv[3],"--delegated")==0);
    const bool discoveryOnly=sourceLoss || delegated || (argc==4 && std::strcmp(argv[3],"--discovery-only")==0);
    const bool duplexTiming=argc>=4 && std::strcmp(argv[3],"--duplex-timing")==0;
    assert(argc<4 || discoveryOnly || duplexTiming);
    assert(argc<5 || duplexTiming || delegated);
    const UInt32 outputSafety=argc==5?UInt32(std::strtoul(argv[4],nullptr,10)):0;
    auto url=CFURLCreateFromFileSystemRepresentation(nullptr,reinterpret_cast<const UInt8 *>(argv[1]),std::strlen(argv[1]),true);assert(url);
    auto plugin=CFPlugInCreate(nullptr,url);CFRelease(url);assert(plugin);
    auto factories=CFPlugInFindFactoriesForPlugInTypeInPlugIn(kAudioServerPlugInTypeUUID,plugin);
    assert(factories && CFArrayGetCount(factories)==1);
    auto factory=static_cast<CFUUIDRef>(CFArrayGetValueAtIndex(factories,0));
#ifdef PRO40_SESSION_PROFILE
    assert(delegated);
    auto expected=CFUUIDCreateFromString(nullptr,hardware?CFSTR("D48D5072-B661-4932-8BA1-E27BA757778B"):CFSTR("B458D34A-C116-4D8B-88DE-F27F6516E0A7"));
#else
    auto expected=CFUUIDCreateFromString(nullptr,sourceLoss?CFSTR("139B6A6B-216F-48FB-9231-678D02620516"):hardware?CFSTR("F8C4AA9C-7145-4BE0-AC5A-40D8C810EDF9"):delegated?CFSTR("D0B66B29-545B-43F7-87D1-79ADB166B8C4"):duplexTiming?CFSTR("CE25A683-7413-4D79-8EC6-8FC6A15B4577"):CFSTR("9EC1681B-D26E-4E92-A88A-D54B349B197E"));
#endif
    assert(CFEqual(factory,expected));CFRelease(expected);
    assert(!CFPlugInInstanceCreate(nullptr,factory,IUnknownUUID));
    AudioServerPlugInHostInterface host{};
    host.PropertiesChanged=[](AudioServerPlugInHostRef,AudioObjectID,UInt32,const AudioObjectPropertyAddress *)->OSStatus{return noErr;};
    host.RequestDeviceConfigurationChange=[](AudioServerPlugInHostRef,AudioObjectID,UInt64,void *)->OSStatus{return kAudioHardwareUnsupportedOperationError;};
    for(unsigned n=0;n<20;++n) {
        auto driver=static_cast<AudioServerPlugInDriverRef>(CFPlugInInstanceCreate(nullptr,factory,kAudioServerPlugInTypeUUID));assert(driver);
        auto &api=**driver;void *queried=nullptr;
        assert(api.QueryInterface(driver,CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID),&queried)==S_OK && queried==driver);
        assert(api.Release(queried)==1);
        assert(api.Initialize(driver,&host)==noErr);
        for(auto scope:{kAudioObjectPropertyScopeInput,kAudioObjectPropertyScopeOutput}) {
            AudioObjectPropertyAddress safety{kAudioDevicePropertySafetyOffset,scope,kAudioObjectPropertyElementMain};
            UInt32 value=999,size=0;
            assert(api.GetPropertyData(driver,2,0,&safety,0,nullptr,sizeof(value),&size,&value)==noErr);
            assert(size==sizeof(value) && value==(scope==kAudioObjectPropertyScopeInput?expectedSafety:outputSafety));
        }
        AudioObjectPropertyAddress translate{kAudioPlugInPropertyTranslateUIDToDevice,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
#ifdef PRO40_SESSION_PROFILE
        CFStringRef uid=hardware?CFSTR("local.pro40.session.device"):CFSTR("local.pro40.session-fixture.device");AudioObjectID device=0;UInt32 used=0;
#else
        CFStringRef uid=sourceLoss?CFSTR("local.pro40.source-loss-lab.device"):hardware?CFSTR("local.pro40.native-lab.device"):delegated?CFSTR("local.pro40.delegated-lab.device"):duplexTiming?CFSTR("local.pro40.duplex-timing-lab.device"):CFSTR("local.pro40.silence-lab.device");AudioObjectID device=0;UInt32 used=0;
#endif
        assert(api.GetPropertyData(driver,1,0,&translate,sizeof(uid),&uid,sizeof(device),&used,&device)==noErr && device==2);
#ifdef PRO40_SESSION_PROFILE
        AudioObjectPropertyAddress layout{kAudioDevicePropertyPreferredChannelLayout,kAudioObjectPropertyScopeOutput,0};
        UInt32 layoutSize=0;Boolean settable=true;
        assert(api.GetPropertyDataSize(driver,device,0,&layout,0,nullptr,&layoutSize)==noErr && layoutSize==12);
        assert(api.IsPropertySettable(driver,device,0,&layout,&settable)==noErr && !settable);
        std::array<UInt32,4> words{99,99,99,99};
        assert(api.GetPropertyData(driver,device,0,&layout,0,nullptr,11,&used,words.data())==kAudioHardwareBadPropertySizeError && used==0 && words[0]==99);
        assert(api.GetPropertyData(driver,device,0,&layout,0,nullptr,16,&used,words.data())==noErr && used==12);
        assert(words[0]==kAudioChannelLayoutTag_MPEG_5_1_A && words[1]==0 && words[2]==0 && words[3]==99);
        layout.mScope=kAudioObjectPropertyScopeInput;assert(!api.HasProperty(driver,device,0,&layout));
        layout={kAudioDevicePropertyPreferredChannelsForStereo,kAudioObjectPropertyScopeOutput,0};
        assert(api.GetPropertyData(driver,device,0,&layout,0,nullptr,16,&used,words.data())==noErr && used==8 && words[0]==1 && words[1]==2);
        for(UInt32 channel=1;channel<=7;++channel) {
            layout={kAudioObjectPropertyName,kAudioObjectPropertyScopeOutput,channel};
            if(channel==7){assert(!api.HasProperty(driver,device,0,&layout));continue;}
            CFStringRef name=nullptr;
            assert(api.GetPropertyData(driver,device,0,&layout,0,nullptr,sizeof(name),&used,&name)==noErr && used==sizeof(name) && name);
            const CFStringRef expectedNames[]={CFSTR("1 — Left"),CFSTR("2 — Right"),CFSTR("3 — Centre"),CFSTR("4 — LFE"),CFSTR("5 — Left Surround"),CFSTR("6 — Right Surround")};
            assert(CFEqual(name,expectedNames[channel-1]));CFRelease(name);
        }
        for(auto scope:{kAudioObjectPropertyScopeInput,kAudioObjectPropertyScopeOutput})for(auto selector:{kAudioDevicePropertyDeviceCanBeDefaultDevice,kAudioDevicePropertyDeviceCanBeDefaultSystemDevice}) {
            layout={selector,scope,0};UInt32 eligible=99;
            assert(api.GetPropertyData(driver,device,0,&layout,0,nullptr,sizeof(eligible),&used,&eligible)==noErr);
            assert(eligible==UInt32(hardware && selector==kAudioDevicePropertyDeviceCanBeDefaultDevice));
        }
#endif
        AudioObjectPropertyAddress period{kAudioDevicePropertyZeroTimeStampPeriod,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
        UInt32 timestampPeriod=0;
        assert(api.GetPropertyData(driver,device,0,&period,0,nullptr,sizeof(timestampPeriod),&used,&timestampPeriod)==noErr && timestampPeriod>=10923);
        AudioServerPlugInClientInfo client{};client.mClientID=7;
        if(!discoveryOnly) {
        assert(api.AddDeviceClient(driver,2,&client)==noErr && api.StartIO(driver,2,7)==noErr);
        AudioServerPlugInIOCycleInfo cycle{};std::array<Float32,128*8> samples;samples.fill(1);
        assert(api.DoIOOperation(driver,2,3,7,kAudioServerPlugInIOOperationReadInput,128,&cycle,samples.data(),nullptr)==noErr);
        for(size_t i=0;i<samples.size();++i)assert(samples[i]==(i<128*(duplexTiming?8u:1u)?0:1));
        if(duplexTiming) {
            samples.fill(.25f);
            assert(api.DoIOOperation(driver,2,4,7,kAudioServerPlugInIOOperationWriteMix,128,&cycle,samples.data(),nullptr)==noErr);
            for(auto sample:samples)assert(sample==.25f);
        }
        assert(api.StopIO(driver,2,7)==noErr && api.RemoveDeviceClient(driver,2,&client)==noErr);
        }
        assert(api.Release(driver)==0);
        // No saved callback/table pointer is used after the final Release.
    }
    CFRelease(factories);CFRelease(plugin);
    std::puts(discoveryOnly?"PASS: local CFPlugIn discovery/properties, 20 create/release cycles; no IO or HAL installation.":"PASS: local CFPlugIn metadata discovery, registered factory, 20 create/IO/release cycles. Not installed in HAL.");
}
