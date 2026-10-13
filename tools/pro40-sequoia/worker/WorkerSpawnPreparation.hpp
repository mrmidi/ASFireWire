// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "WorkerChildOwner.hpp"
#include <spawn.h>
#include <mach/mach.h>
#include <memory>
#include <optional>
namespace Pro40 {
// Serial owner for a single launch. All child dup sources are above fd 10 so
// remapping stdin/stdout/stderr/admission cannot overwrite a later source.
// bootstrap is borrowed through launch; arguments/environment remain caller-owned.
// No verification or release here. Retain any spawned child until actual reap.
class WorkerSpawnPreparation final {
    int fds_[7]={-1,-1,-1,-1,-1,-1,-1}; // life, release, result pairs; stderr copy
    posix_spawn_file_actions_t actions_{};
    posix_spawnattr_t attributes_{};
    bool attempted_=false,prepared_=false,launched_=false,actionsLive_=false,attributesLive_=false;
    bool transferred_=false;
    int cleanupError_=0;
    std::optional<Pro40Audio::NativeChildProcess> child_;
#ifdef PRO40_SPAWN_TESTING
    unsigned failStep_=0,step_=0;
    int failClose_=-1;
#endif
    bool inject()noexcept{
#ifdef PRO40_SPAWN_TESTING
        return ++step_==failStep_;
#else
        return false;
#endif
    }
    void record(int error)noexcept{if(error && !cleanupError_)cleanupError_=error;}
    void closeFD(unsigned index)noexcept{
        const int fd=std::exchange(fds_[index],-1);
        if(fd>=0 && ::close(fd)<0)record(errno); // Never retry ambiguous close.
#ifdef PRO40_SPAWN_TESTING
        if(fd>=0 && int(index)==failClose_){failClose_=-1;record(EIO);}
#endif
    }
    int promote(unsigned index)noexcept{
        if(inject())return EIO;
        const int high=fcntl(fds_[index],F_DUPFD_CLOEXEC,11);
        if(high<0)return errno;
        closeFD(index);fds_[index]=high;return cleanupError_;
    }
    int finishSetup()noexcept{
        if(std::exchange(actionsLive_,false))record(posix_spawn_file_actions_destroy(&actions_));
        if(std::exchange(attributesLive_,false))record(posix_spawnattr_destroy(&attributes_));
        for(unsigned i:{0u,2u,5u,6u})closeFD(i);
        return cleanupError_;
    }
public:
    enum class Namespace {replacementBootstrap,inherit};
    WorkerSpawnPreparation()=default;
    WorkerSpawnPreparation(const WorkerSpawnPreparation&)=delete;
    WorkerSpawnPreparation& operator=(const WorkerSpawnPreparation&)=delete;
    ~WorkerSpawnPreparation(){(void)close();} // Does not prove reap or replace owner retention.
#ifdef PRO40_SPAWN_TESTING
    void failBeforeStep(unsigned step)noexcept{if(!attempted_)failStep_=step;}
    unsigned steps()const noexcept{return step_;}
    void failSetupCloseForTesting()noexcept{failClose_=0;}
#endif
    [[nodiscard]] int prepare(mach_port_t bootstrap,int errorFD,Namespace space=Namespace::replacementBootstrap)noexcept{
        if(attempted_)return EALREADY;
        attempted_=true;
        if(space!=Namespace::replacementBootstrap && space!=Namespace::inherit)return EINVAL;
        if((space==Namespace::replacementBootstrap?!MACH_PORT_VALID(bootstrap):bootstrap!=MACH_PORT_NULL) || errorFD<0)return EINVAL;
        int error=0;
        for(unsigned first:{0u,2u,4u}){
            if(inject())return EIO;
            if(pipe(fds_+first)<0)return errno;
            for(unsigned i:{first,first+1})if((error=promote(i)))return error;
        }
        if(inject())return EIO;
        fds_[6]=fcntl(errorFD,F_DUPFD_CLOEXEC,11);if(fds_[6]<0)return errno;
        for(unsigned i:{2u,3u,4u}){
            if(inject())return EIO;
            const int flags=fcntl(fds_[i],F_GETFL);
            if(flags<0 || fcntl(fds_[i],F_SETFL,flags|O_NONBLOCK)<0)return errno;
        }
        if(inject())return EIO;
        if((error=posix_spawn_file_actions_init(&actions_)))return error;
        actionsLive_=true;
        for(auto mapping:{std::pair{0u,0},std::pair{5u,1},std::pair{6u,2},std::pair{2u,10}}){
            if(inject())return EIO;
            if((error=posix_spawn_file_actions_adddup2(&actions_,fds_[mapping.first],mapping.second)))return error;
        }
        if(inject())return EIO;
        if((error=posix_spawnattr_init(&attributes_)))return error;
        attributesLive_=true;
        if(inject())return EIO;
        if((error=posix_spawnattr_setflags(&attributes_,POSIX_SPAWN_CLOEXEC_DEFAULT)))return error;
        if(inject())return EIO;
        if(space==Namespace::replacementBootstrap &&
           (error=posix_spawnattr_setspecialport_np(&attributes_,bootstrap,TASK_BOOTSTRAP_PORT)))return error;
        prepared_=true;return 0;
    }
    // Spawn errors are returned directly by posix_spawn, not from errno. Even if
    // setup cleanup fails after successful spawn, child_ retains wait ownership.
    [[nodiscard]] int launch(const char *path,char *const argv[],char *const env[])noexcept{
        if(!prepared_ || launched_)return EINVAL;
        launched_=true;pid_t pid=0;
        const int error=posix_spawn(&pid,path,&actions_,&attributes_,argv,env);
        if(!error)child_.emplace(pid);
        (void)finishSetup();
        if(error){(void)close();return error;}
        if(cleanupError_){closeFD(1);closeFD(3);}
        return cleanupError_;
    }
    [[nodiscard]] std::shared_ptr<WorkerChildOwner> takeWorker(){
        if(!child_ || transferred_)return {};
        auto worker=std::make_shared<WorkerChildOwner>(std::move(*child_),fds_[1],fds_[3]);
        fds_[1]=fds_[3]=-1;child_.reset();transferred_=true;
        if(cleanupError_)worker->cancel();
        return worker;
    }
    [[nodiscard]] int takeResultReader()noexcept{
        return transferred_?std::exchange(fds_[4],-1):-1;
    }
    bool retainsChild()const noexcept{return child_.has_value();}
    // Closes preparation endpoints but deliberately keeps any child wait record.
    [[nodiscard]] int close()noexcept{
        prepared_=false;(void)finishSetup();
        for(unsigned i:{1u,3u,4u})closeFD(i);return cleanupError_;
    }
    int cleanupError()const noexcept{return cleanupError_;}
};
}
