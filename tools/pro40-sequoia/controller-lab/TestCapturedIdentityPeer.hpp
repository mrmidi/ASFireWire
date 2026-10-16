// SPDX-License-Identifier: Apache-2.0
#pragma once
// Test-only owned sender. Uses the same private bootstrap-port capture as
// NativePeerIdentityChecks; no service lookup or user audio process is involved.
#include "../NativeOriginalPeerCapture.hpp"
#include <mach/mach.h>
#include <bsm/libbsm.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
extern char **environ;
namespace Pro40Lab {
class TestCapturedIdentityPeer final {
    int keepAlive_=-1;
    pid_t pid_=0;
    audit_token_t token_{};
    static void require(bool ok){if(!ok){std::fputs("FAIL owned original-peer capture/cleanup\n",stderr);std::_Exit(80);}}
public:
    explicit TestCapturedIdentityPeer(const char *path){
        mach_port_t port=MACH_PORT_NULL;
        require(mach_port_allocate(mach_task_self(),MACH_PORT_RIGHT_RECEIVE,&port)==KERN_SUCCESS);
        require(mach_port_insert_right(mach_task_self(),port,port,MACH_MSG_TYPE_MAKE_SEND)==KERN_SUCCESS);
        int pipeFD[2];require(pipe(pipeFD)==0);
        posix_spawnattr_t attr;require(posix_spawnattr_init(&attr)==0);
        require(posix_spawnattr_setspecialport_np(&attr,port,TASK_BOOTSTRAP_PORT)==0);
        posix_spawn_file_actions_t actions;require(posix_spawn_file_actions_init(&actions)==0);
        require(posix_spawn_file_actions_adddup2(&actions,pipeFD[0],STDIN_FILENO)==0);
        require(posix_spawn_file_actions_addclose(&actions,pipeFD[0])==0);
        require(posix_spawn_file_actions_addclose(&actions,pipeFD[1])==0);
        char *args[]={const_cast<char*>(path),nullptr};
        require(posix_spawn(&pid_,path,&actions,&attr,args,environ)==0);
        require(posix_spawnattr_destroy(&attr)==0);
        require(posix_spawn_file_actions_destroy(&actions)==0);
        require(close(pipeFD[0])==0);keepAlive_=pipeFD[1];
        {
            Pro40Audio::NativeMachReceived<> received;
            require(received.read(port,3000,Pro40Audio::nativeAuditReceiveOptions));
            const auto captured=Pro40Audio::NativeOriginalPeerCapture::capture(received,port,pid_);
            require(captured.has_value());token_=captured->token();
        } // Drop received rights before dropping the capture endpoint.
        require(mach_port_deallocate(mach_task_self(),port)==KERN_SUCCESS);
        require(mach_port_mod_refs(mach_task_self(),port,MACH_PORT_RIGHT_RECEIVE,-1)==KERN_SUCCESS);
    }
    TestCapturedIdentityPeer(const TestCapturedIdentityPeer&)=delete;
    TestCapturedIdentityPeer& operator=(const TestCapturedIdentityPeer&)=delete;
    ~TestCapturedIdentityPeer(){reap();}
    audit_token_t token()const{return token_;}
    pid_t pid()const{return pid_;}
    void reap(){
        if(keepAlive_<0)return;
        require(close(keepAlive_)==0);keepAlive_=-1;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        for(;;){
            int status=0;const auto ended=waitpid(pid_,&status,WNOHANG);
            if(ended==pid_){require(WIFEXITED(status)&&WEXITSTATUS(status)==0);return;}
            require(ended==0 && std::chrono::steady_clock::now()<deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};
}
