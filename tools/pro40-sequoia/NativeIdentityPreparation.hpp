// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <spawn.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <utility>
#include <initializer_list>
namespace Pro40Audio {
// Single-thread owner for helper preparation and its four pipe descriptors.
// Explicit close reports failures; destructor is a best-effort final backstop.
class NativeIdentityPreparation {
    int fds_[4]={-1,-1,-1,-1};
    posix_spawn_file_actions_t actions_{};
    posix_spawnattr_t attributes_{};
    bool actionsLive_=false,attributesLive_=false;
    int cleanupError_=0;
    void record(int error)noexcept{if(error && !cleanupError_)cleanupError_=error;}
public:
    NativeIdentityPreparation()=default;
    NativeIdentityPreparation(const NativeIdentityPreparation &)=delete;
    NativeIdentityPreparation &operator=(const NativeIdentityPreparation &)=delete;
    ~NativeIdentityPreparation(){(void)close();}
    int fd(unsigned index)const noexcept{return fds_[index];}
    posix_spawn_file_actions_t *actions()noexcept{return &actions_;}
    posix_spawnattr_t *attributes()noexcept{return &attributes_;}
    // failBeforeStep is deterministic laboratory fault injection; zero disables.
    [[nodiscard]] int prepare(unsigned failBeforeStep=0)noexcept{
        if(actionsLive_ || attributesLive_ || fds_[0]>=0 || fds_[1]>=0 || fds_[2]>=0 || fds_[3]>=0 || cleanupError_)return EINVAL;
        unsigned step=0;int error=0;
        const auto inject=[&]{return ++step==failBeforeStep;};
        if(inject())return EIO;
        if(pipe(fds_)<0)return errno;
        if(inject())return EIO;
        if(pipe(fds_+2)<0)return errno;
        if(inject())return EIO;
        if((error=posix_spawn_file_actions_init(&actions_)))return error;
        actionsLive_=true;
        for(auto pair:{std::pair{0u,STDIN_FILENO},std::pair{3u,STDOUT_FILENO},std::pair{4u,STDERR_FILENO}}){
            if(inject())return EIO;
            if((error=posix_spawn_file_actions_adddup2(&actions_,pair.first==4?STDERR_FILENO:fds_[pair.first],pair.second)))return error;
        }
        if(inject())return EIO;
        if((error=posix_spawnattr_init(&attributes_)))return error;
        attributesLive_=true;
        if(inject())return EIO;
        if((error=posix_spawnattr_setflags(&attributes_,POSIX_SPAWN_CLOEXEC_DEFAULT)))return error;
        for(auto index:{1u,2u}){
            if(inject())return EIO;
            const int flags=fcntl(fds_[index],F_GETFL);
            if(flags<0 || fcntl(fds_[index],F_SETFL,flags|O_NONBLOCK)<0)return errno;
        }
        return 0;
    }
    [[nodiscard]] int closeFD(unsigned index)noexcept{
        const int fd=std::exchange(fds_[index],-1);
        // Never retry an ambiguous close (including EINTR): a reused descriptor
        // could belong to something else. Sticky error prevents clean outcome.
        if(fd>=0 && ::close(fd)<0)record(errno);
        return cleanupError_;
    }
    [[nodiscard]] int finishSpawn()noexcept{
        if(std::exchange(actionsLive_,false))record(posix_spawn_file_actions_destroy(&actions_));
        if(std::exchange(attributesLive_,false))record(posix_spawnattr_destroy(&attributes_));
        (void)closeFD(0);(void)closeFD(3);return cleanupError_;
    }
    [[nodiscard]] int close()noexcept{
        (void)finishSpawn();(void)closeFD(1);(void)closeFD(2);return cleanupError_;
    }
};
}
