// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../InputRecording.hpp"
#include "../SessionRunResult.hpp"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>
namespace Pro40 {
inline bool writeRecordingBytes(int fd,std::span<const uint8_t> bytes) {
    while(!bytes.empty()) {
        const auto count=::write(fd,bytes.data(),bytes.size());
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return false;
        bytes=bytes.subspan(size_t(count));
    }
    return true;
}
// Called only after native teardown. Never replace an existing result file.
inline bool publishRecordingFile(const std::string &path,std::span<const uint8_t> header,
                                  std::span<const uint8_t> body={}) {
    std::string pattern=path+".tmp-XXXXXX";
    std::vector<char> temporary(pattern.begin(),pattern.end());temporary.push_back(0);
    const int fd=mkstemp(temporary.data());if(fd<0)return false;
    bool ok=writeRecordingBytes(fd,header) && writeRecordingBytes(fd,body) && fsync(fd)==0;
    if(close(fd)!=0)ok=false;
    if(ok)ok=link(temporary.data(),path.c_str())==0;
    unlink(temporary.data());
    return ok;
}
inline bool exportRecording(const std::string &log,const Pro40Packets::InputRecording &recording,
                            const NativeSessionResult &result) {
    const auto pcm=recording.pcm();
    const char *status="skipped";
    if(result.accepted()) {
        status="failed";
        if(recording.valid() && !recording.fault() && !pcm.empty()) {
            // Separate mono tracks preserve interface channel order without
            // assigning surround speaker labels. File IO occurs after teardown.
            const auto channelBytes=recording.frames()*3;
            std::array<uint8_t,44> h{};
            auto text=[&](size_t at,const char *s){for(size_t n=0;n<4;++n)h[at+n]=uint8_t(s[n]);};
            auto word=[&](size_t at,uint32_t value,size_t count){for(size_t n=0;n<count;++n)h[at+n]=uint8_t(value>>(n*8));};
            text(0,"RIFF");word(4,uint32_t(channelBytes+36),4);text(8,"WAVE");text(12,"fmt ");
            word(16,16,4);word(20,1,2);word(22,1,2);word(24,48000,4);word(28,144000,4);
            word(32,3,2);word(34,24,2);text(36,"data");word(40,uint32_t(channelBytes),4);
            bool saved=true;
            std::vector<uint8_t> channelPcm(recording.channels()>1?channelBytes:0);
            for(unsigned channel=0;channel<recording.channels();++channel) {
                if(recording.channels()>1)for(size_t frame=0;frame<recording.frames();++frame)
                    for(size_t byte=0;byte<3;++byte)
                        channelPcm[frame*3+byte]=pcm[(frame*recording.channels()+channel)*3+byte];
                const std::span<const uint8_t> body=recording.channels()==1?pcm:std::span<const uint8_t>(channelPcm);
                if(!publishRecordingFile(log+".input"+std::to_string(channel+1)+".wav",h,body))saved=false;
            }
            if(saved)status="saved";
        }
    }
    char summary[256];
    const int count=std::snprintf(summary,sizeof(summary),
        "{\"status\":\"%s\",\"frames\":%zu,\"rate\":48000,\"bits\":24,\"channels\":%u,\"capture_fault\":%s}\n",
        status,recording.frames(),recording.channels(),recording.fault()?"true":"false");
    const bool reported=count>0 && size_t(count)<sizeof(summary) && publishRecordingFile(log+".recording.json",
        {reinterpret_cast<const uint8_t *>(summary),size_t(count)});
    std::fprintf(stderr,"input-recording: %s frames=%zu channels=%u report=%s\n",status,recording.frames(),recording.channels(),reported?"saved":"failed");
    return reported && std::string(status)=="saved";
}
}
