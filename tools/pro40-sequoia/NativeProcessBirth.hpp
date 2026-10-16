// SPDX-License-Identifier: Apache-2.0
#pragma once
// Read-only instance evidence. SDK libproc.h:96, sys/proc_info.h:59-82,718-719.
// This is not permission to signal a PID or proof that its descendants exited.
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <cstdio>
#include <cstring>
#include <optional>
struct NativeProcessBirth {
    int pid=0;char boot[64]{};uint64_t seconds=0,microseconds=0;
    static std::optional<NativeProcessBirth> read(int pid){
        if(pid<=0)return std::nullopt;
        NativeProcessBirth birth;birth.pid=pid;size_t size=sizeof(birth.boot);
        if(sysctlbyname("kern.bootsessionuuid",birth.boot,&size,nullptr,0)||size>sizeof(birth.boot)||
           !std::memchr(birth.boot,0,sizeof(birth.boot))||std::strlen(birth.boot)!=36||
           std::strspn(birth.boot,"0123456789ABCDEFabcdef-")!=36)return std::nullopt;
        proc_bsdinfo info{};
        if(proc_pidinfo(pid,PROC_PIDTBSDINFO,0,&info,sizeof(info))!=sizeof(info)||info.pbi_pid!=unsigned(pid)||
           !info.pbi_start_tvsec||info.pbi_start_tvusec>=1000000)return std::nullopt;
        birth.seconds=info.pbi_start_tvsec;birth.microseconds=info.pbi_start_tvusec;return birth;
    }
    void print()const{
        std::printf("{\"pid\":%d,\"boot\":\"%s\",\"start_seconds\":%llu,\"start_microseconds\":%llu}\n",
            pid,boot,static_cast<unsigned long long>(seconds),static_cast<unsigned long long>(microseconds));
    }
};
