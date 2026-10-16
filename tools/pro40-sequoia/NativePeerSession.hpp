// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeStartWorker.hpp"
#include "NativeFeedConnection.hpp"
#include "NativeFeedMailbox.hpp"
#include "NativePeerCleanup.hpp"
namespace Pro40Audio {
// Single-use non-RT peer session owner. Admission remains source-owned; caller
// supplies bounded capability acquisition, sender work and remote/control cleanup.
// External channel, mailboxes and callback captures must outlive successful join.
class NativePeerSession {
public:
    using Wall=NativeStartWorker::Wall;
    NativePeerSession(NativeStartMailbox &startup,NativeFeedMailbox &feed)
        :startup_(startup),feed_(feed),worker_(startup){}
    NativePeerSession(const NativePeerSession &)=delete;
    NativePeerSession &operator=(const NativePeerSession &)=delete;
    template<class Channel,class Acquire,class Run>
    bool start(Channel &channel,Wall::time_point deadline,uint64_t epoch,
               mach_msg_timeout_t timeout,Acquire acquire,Run run){
        if(finished_)return false;
        return worker_.start(channel,deadline,[this,epoch,timeout,acquire=std::move(acquire),run=std::move(run)]() mutable {
            NativeFeedConnection connection;
            if(!connection.open(epoch,timeout,acquire,[this]{return startup_.cancelled()||feed_.cancelled();})){
                connectionClosed_=connection.close();return false;
            }
            run(*connection.sender());
            connectionClosed_=connection.close();return true;
        });
    }
    void activate() noexcept {worker_.activate();} // Only after source accepts READY.
    void cancel() noexcept {worker_.cancel();feed_.cancel();}
    bool failed() const noexcept {return worker_.failed();}
    bool joinable() const noexcept {return worker_.joinable();}
    // Explicit supervisor recovery join; does not change a latched cleanup error.
    bool joinUntil(Wall::time_point deadline){return worker_.joinUntil(deadline);}
    // Caller cancels first on failure and releases any test/cooperative holds.
    // Normal completion instead waits for the sender's end/drain contract.
    // Never close control ports or consult worker-owned state before joining.
    template<class ConfirmRemote,class CloseControl>
    int32_t finish(Wall::time_point deadline,ConfirmRemote confirm,CloseControl close){
        if(finished_)return result_;
        finished_=true;
        if(!worker_.joinUntil(deadline))return result_;
        const bool remote=confirm();
        const bool control=close(); // Always attempt local close after remote failure.
        result_=peerCleanupStatus(connectionClosed_&&control?LocalCleanup::closed:LocalCleanup::incomplete,
                                  remote?RemoteCleanup::confirmed:RemoteCleanup::unconfirmed);
        return result_;
    }
private:
    NativeStartMailbox &startup_;NativeFeedMailbox &feed_;NativeStartWorker worker_;
    bool connectionClosed_=true; // Worker writes; supervisor reads only after join.
    bool finished_=false;int32_t result_=int32_t(PeerCleanupError::localIncomplete);
};
}
