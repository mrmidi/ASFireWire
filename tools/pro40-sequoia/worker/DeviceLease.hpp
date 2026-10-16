// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace Pro40 {
enum class DeviceAdmission {acquired, busy, unclean, ioError};
enum class DeviceRecovery {recovered, alreadyClean, busy, auditRejected, ioError};
// Advisory per-user/per-GUID exclusion for cooperating workers. The file is
// never unlinked: replacing its inode could bypass an existing flock holder.
class DeviceLease {
    int fd_=-1;
    bool acquired_=false;
    bool writeState(const char *text,size_t length) {
        return pwrite(fd_,text,length,0)==static_cast<ssize_t>(length) &&
               ftruncate(fd_,length)==0 && fsync(fd_)==0;
    }
    DeviceAdmission inspect(const char *directory,uint64_t guid,bool create) {
        if(fd_>=0)return DeviceAdmission::ioError;
        const int dir=open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(dir<0)return DeviceAdmission::ioError;
        struct stat info{};
        const bool safe=fstat(dir,&info)==0 && info.st_uid==geteuid() && (info.st_mode&0777)==0700;
        char name[32];std::snprintf(name,sizeof(name),"%016llx.lock",static_cast<unsigned long long>(guid));
        if(safe)fd_=openat(dir,name,O_RDWR|(create?O_CREAT:0)|O_NOFOLLOW|O_CLOEXEC,0600);
        close(dir);
        if(fd_<0 || fstat(fd_,&info)<0 || !S_ISREG(info.st_mode) || info.st_uid!=geteuid() ||
           info.st_nlink!=1 || (info.st_mode&0777)!=0600)return DeviceAdmission::ioError;
        if(flock(fd_,LOCK_EX|LOCK_NB)<0)
            return errno==EWOULDBLOCK?DeviceAdmission::busy:DeviceAdmission::ioError;
        std::array<char,8> state{};
        const auto count=pread(fd_,state.data(),state.size(),0);
        if(count<0)return DeviceAdmission::ioError;
        if(count!=0 && (count!=6 || std::memcmp(state.data(),"clean\n",6)!=0))return DeviceAdmission::unclean;
        return DeviceAdmission::acquired;
    }
public:
    DeviceLease()=default;
    DeviceLease(const DeviceLease &)=delete;
    DeviceLease &operator=(const DeviceLease &)=delete;
    ~DeviceLease(){if(fd_>=0)close(fd_);}
    DeviceAdmission acquire(const char *directory,uint64_t guid) {
        const auto state=inspect(directory,guid,true);
        if(state!=DeviceAdmission::acquired)return state;
        // Must be persisted before any native operation. Abrupt exit leaves
        // active/partial state behind even though the OS releases the flock.
        if(!writeState("active\n",7))return DeviceAdmission::ioError;
        acquired_=true;
        return DeviceAdmission::acquired;
    }
    bool confirmClean() {
        if(!acquired_)return false;
        return writeState("clean\n",6);
    }
    // Never create or replace a record during recovery. The audit executes
    // with exclusive flock held, and refusal preserves its original bytes.
    template<class Verify> DeviceRecovery recover(const char *directory,uint64_t guid,Verify verify) {
        const auto state=inspect(directory,guid,false);
        if(state==DeviceAdmission::busy)return DeviceRecovery::busy;
        if(state==DeviceAdmission::ioError)return DeviceRecovery::ioError;
        if(!verify())return DeviceRecovery::auditRejected;
        if(state==DeviceAdmission::acquired)return DeviceRecovery::alreadyClean;
        return writeState("clean\n",6)?DeviceRecovery::recovered:DeviceRecovery::ioError;
    }
};
}
