// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DuplexProvisionIntroduction.hpp"
#include <array>
#include <chrono>
namespace Pro40Audio {
// One control thread owns all registry and pending introduction state. No
// native/audio resources. Requirements are retained; a named service merely
// discovers this owner and is never sufficient authority to register a port.
class DuplexProvisionService final {
    using Clock=std::chrono::steady_clock;
    IdentityDetail::Owned<SecRequirementRef> controller_,helper_;
    DuplexProvisionRegistry registry_;
    struct Pending {
        std::unique_ptr<ProvisionIntroduction::Accepted> accepted;
        Clock::time_point expires;
    };
    std::array<Pending,8> pending_{};
    static auto retain(SecRequirementRef requirement) {
        if(requirement)CFRetain(requirement);return IdentityDetail::Owned<SecRequirementRef>(requirement);
    }
public:
    struct Report {
        unsigned introduced=0,denied=0,completed=0,rejected=0,expired=0;
        kern_return_t listenerStatus=KERN_SUCCESS;
        kern_return_t rejectedStatus=KERN_SUCCESS;
    };
    DuplexProvisionService(SecRequirementRef controller,SecRequirementRef helper)
        :controller_(retain(controller)),helper_(retain(helper)),
         registry_([this](const audit_token_t &t){return verifyNativePeerIdentity(t,controller_.get())==errSecSuccess;},
                   [this](const audit_token_t &t){return verifyNativePeerIdentity(t,helper_.get())==errSecSuccess;}){}
    // At most eight 1-ms receives plus one introduction per turn. Security calls
    // are synchronous and have no preemption guarantee. Never use on an audio
    // callback or hold the driver's state lock around this service loop.
    [[nodiscard]] Report poll(mach_port_t listener) {
        Report report;
        for(auto &slot:pending_) {
            if(!slot.accepted)continue;
            if(Clock::now()>=slot.expires){slot.accepted.reset();++report.expired;continue;}
            const auto result=ProvisionTransport::serveOne(registry_,slot.accepted->receive.get(),slot.accepted->token,1);
            if(result.transport==MACH_RCV_TIMED_OUT)continue;
            if(result.transport==KERN_SUCCESS)++report.completed;else {++report.rejected;report.rejectedStatus=result.transport;}
            // Exactly one registry RPC per authenticated private introduction.
            // A retry obtains another introduction and uses the same nonce.
            slot.accepted.reset();
        }
        for(auto &slot:pending_)if(!slot.accepted) {
            auto result=ProvisionIntroduction::serveOne(listener,controller_.get(),helper_.get(),1);
            report.listenerStatus=result.transport;
            if(result.accepted) {
                slot.accepted=std::move(result.accepted);slot.expires=Clock::now()+std::chrono::seconds(2);++report.introduced;
            }else if(result.transport==KERN_SUCCESS)++report.denied;
            else if(result.transport!=MACH_RCV_TIMED_OUT){++report.rejected;report.rejectedStatus=result.transport;}
            break;
        }
        return report;
    }
};
}
