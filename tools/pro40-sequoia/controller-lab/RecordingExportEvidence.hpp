// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../worker/WorkerLaunchSpecification.hpp"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <charconv>
#include <cstring>
#include <utility>
#include <cerrno>
namespace Pro40Lab {
struct RecordingExportReport {
    const char *status="not-observed";
    bool verified=false;
    uint64_t frames=0;
};
// Reads only this launch's fresh private directory, after child collection.
// Validates the canonical RecordingExport.hpp format, not arbitrary JSON/WAV.
// File presence is not native success, audible content or timing-export proof.
class RecordingExportEvidence final {
    int directory_=-1;
    std::string stem_;
    bool requested_=false,prepared_=false,finished_=false;
    RecordingExportReport result_;
    bool closeDirectory(){const int fd=std::exchange(directory_,-1);return fd<0 || close(fd)==0;}
    bool readFile(const std::string &name,void *bytes,size_t count,uint64_t &size){
        const int fd=openat(directory_,name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(fd<0)return false;
        struct stat info{};
        bool ok=fstat(fd,&info)==0 && S_ISREG(info.st_mode) && info.st_uid==geteuid() &&
            !(info.st_mode&0077) && info.st_nlink==1 && info.st_size>=0;
        size=ok?uint64_t(info.st_size):0;
        if(size<count)ok=false;
        size_t offset=0;
        while(ok && offset<count){
            const auto n=read(fd,static_cast<char*>(bytes)+offset,count-offset);
            if(n<0 && errno==EINTR)continue;
            if(n<=0){ok=false;break;}offset+=size_t(n);
        }
        if(close(fd)!=0)ok=false;
        return ok;
    }
public:
    RecordingExportEvidence()=default;
    RecordingExportEvidence(const RecordingExportEvidence&)=delete;
    RecordingExportEvidence& operator=(const RecordingExportEvidence&)=delete;
    ~RecordingExportEvidence(){(void)closeDirectory();}
    bool prepare(const Pro40::WorkerLaunchSpecification &spec){
        if(prepared_ || directory_>=0 || finished_)return false;
        requested_=spec.operation()!=Pro40::WorkerLaunchSpecification::Operation::session;
        if(!requested_){prepared_=true;return true;}
        const auto path=spec.identityOptions().logPath;const auto slash=path.rfind('/');
        stem_=path.substr(slash+1);
        directory_=open(path.substr(0,slash).c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        struct stat info{};
        if(directory_<0 || fstat(directory_,&info)!=0 || info.st_uid!=geteuid() || (info.st_mode&0077))return false;
        for(const auto &name:{stem_,stem_+".recording.json",stem_+".input1.wav"}){
            if(fstatat(directory_,name.c_str(),&info,AT_SYMLINK_NOFOLLOW)==0 || errno!=ENOENT)return false;
        }
        prepared_=true;return true;
    }
    RecordingExportReport finish(bool nativeAccepted){
        if(finished_)return result_;finished_=true;
        result_.status=!requested_?"not-requested":!prepared_?"preflight-failed":!nativeAccepted?"native-not-accepted":"invalid-or-missing";
        if(requested_ && prepared_ && nativeAccepted){
            // Small summary: stat first, then read exactly its bounded length.
            struct stat info{};std::array<char,256> summary{};uint64_t size=0;
            const auto report=stem_+".recording.json";
            if(fstatat(directory_,report.c_str(),&info,AT_SYMLINK_NOFOLLOW)==0 && info.st_size>0 && info.st_size<256 &&
               readFile(report,summary.data(),size_t(info.st_size),size) && size==uint64_t(info.st_size)){
                constexpr std::string_view prefix="{\"status\":\"saved\",\"frames\":";
                std::string_view text(summary.data(),size);
                uint64_t frames=0;
                if(text.starts_with(prefix)){
                    const auto parsed=std::from_chars(text.data()+prefix.size(),text.data()+text.size(),frames);
                    char expected[256]{};
                    const int n=std::snprintf(expected,sizeof(expected),
                        "{\"status\":\"saved\",\"frames\":%llu,\"rate\":48000,\"bits\":24,\"channels\":1,\"capture_fault\":false}\n",
                        static_cast<unsigned long long>(frames));
                    if(parsed.ec==std::errc{} && frames>0 && frames<=(UINT32_MAX-36)/3 && n>0 && size_t(n)==size && text==expected){
                        std::array<uint8_t,44> h{};uint64_t wavSize=0;
                        const auto word=[&](size_t at,size_t bytes){uint32_t v=0;for(size_t i=0;i<bytes;++i)v|=uint32_t(h[at+i])<<(8*i);return v;};
                        if(readFile(stem_+".input1.wav",h.data(),h.size(),wavSize) && wavSize==44+frames*3 &&
                           !memcmp(h.data(),"RIFF",4) && word(4,4)==36+frames*3 && !memcmp(h.data()+8,"WAVEfmt ",8) &&
                           word(16,4)==16 && word(20,2)==1 && word(22,2)==1 && word(24,4)==48000 && word(28,4)==144000 &&
                           word(32,2)==3 && word(34,2)==24 && !memcmp(h.data()+36,"data",4) && word(40,4)==frames*3)
                            result_={"saved",true,frames};
                    }
                }
            }
        }
        if(!closeDirectory())result_={"close-failed",false,0};
        return result_;
    }
};
}
