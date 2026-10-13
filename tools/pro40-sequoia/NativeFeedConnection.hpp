// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeFeedMachSender.hpp"
#include <optional>
namespace Pro40Audio {
// Worker-thread-only, single-use data connection owner. Acquire must return one
// owned Mach send right (or null) using bounded provisioning. Caller is responsible
// for peer authentication; this class defines no handshake or authentication policy.
// Cancellation is checked around acquisition; it cannot undo an in-flight send.
class NativeFeedConnection {
public:
    enum class State {empty,active,failed,closed};
    NativeFeedConnection()=default;
    ~NativeFeedConnection(){close();}
    NativeFeedConnection(const NativeFeedConnection &)=delete;
    NativeFeedConnection &operator=(const NativeFeedConnection &)=delete;
    template<class Acquire,class Cancelled>
    bool open(uint64_t epoch,mach_msg_timeout_t timeout,Acquire acquire,Cancelled cancelled){
        if(state_!=State::empty)return false;
        if(!epoch||!timeout||cancelled()){state_=State::failed;return false;}
        const mach_port_t port=acquire();
        if(!MACH_PORT_VALID(port)){state_=State::failed;return false;}
        // Adopt immediately, including when cancellation occurred inside acquire.
        sender_.emplace(port,epoch,timeout);
        if(cancelled()){
            cleanupOK_=sender_->close();state_=State::failed;return false;
        }
        state_=State::active;return true;
    }
    NativeFeedMachSender *sender() noexcept {return state_==State::active?&*sender_:nullptr;}
    State state() const noexcept {return state_;}
    bool close() noexcept {
        if(sender_)cleanupOK_=sender_->close()&&cleanupOK_;
        state_=State::closed;return cleanupOK_;
    }
private:
    std::optional<NativeFeedMachSender> sender_;
    State state_=State::empty;bool cleanupOK_=true;
};
}
