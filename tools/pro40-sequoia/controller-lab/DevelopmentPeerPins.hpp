// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
namespace Pro40Lab {
struct DevelopmentPeerPins {
    std::string controller, helper;
};
// Local lab provisioning only; same-user writable state is not a developer trust
// root. Caller supplies its retained, canonical package-directory path, NEVER a
// request argument. Read the one fixed sibling file only, without following links.
inline bool readDevelopmentPeerPins(const std::string& directory, DevelopmentPeerPins& result) {
    result={};
    if(directory.empty() || directory[0]!='/' || directory.back()=='/')return false;
    int parent=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(parent<0)return false;
    size_t start=1;
    while(start<directory.size()){
        auto end=directory.find('/',start);
        auto component=directory.substr(start,end==std::string::npos?end:end-start);
        if(component.empty() || component=="." || component==".." || component.find('\0')!=std::string::npos){close(parent);return false;}
        int next=openat(parent,component.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        close(parent);if(next<0)return false;parent=next;
        if(end==std::string::npos)break;start=end+1;
    }
    struct stat ds{};
    if(fstat(parent,&ds) || ds.st_uid!=geteuid() || (ds.st_mode&07777)!=0700){close(parent);return false;}
    int fd=openat(parent,"development-peer-pins.txt",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    close(parent);if(fd<0)return false;
    struct stat before{},after{};
    constexpr size_t payloadLength=85; // version line plus two 40-hex lines
    std::array<char,payloadLength+1> bytes{};
    bool valid=fstat(fd,&before)==0 && S_ISREG(before.st_mode) && before.st_uid==geteuid() &&
        (before.st_mode&07777)==0600 && before.st_nlink==1 && before.st_size==85;
    size_t used=0;
    while(valid && used<bytes.size()){
        ssize_t n=read(fd,bytes.data()+used,bytes.size()-used);
        if(n<0 && errno==EINTR)continue;
        if(n<0){valid=false;break;}if(!n)break;used+=static_cast<size_t>(n);
    }
    valid=valid && fstat(fd,&after)==0 && before.st_size==after.st_size && before.st_ino==after.st_ino &&
        before.st_mtimespec.tv_sec==after.st_mtimespec.tv_sec && before.st_mtimespec.tv_nsec==after.st_mtimespec.tv_nsec &&
        before.st_ctimespec.tv_sec==after.st_ctimespec.tv_sec && before.st_ctimespec.tv_nsec==after.st_ctimespec.tv_nsec;
    if(close(fd)!=0)valid=false;
    if(!valid || used!=85 || bytes[0]!='v' || bytes[1]!='1' || bytes[2]!='\n' || bytes[43]!='\n' || bytes[84]!='\n')return false;
    auto hash=[&](size_t offset){
        bool nonzero=false;
        for(size_t i=offset;i<offset+40;++i){char c=bytes[i];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;nonzero|=c!='0';}
        return nonzero;
    };
    if(!hash(3)||!hash(44))return false;
    result={std::string(bytes.data()+3,40),std::string(bytes.data()+44,40)};
    return true;
}
}
