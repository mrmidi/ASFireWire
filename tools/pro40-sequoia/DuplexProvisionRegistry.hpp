// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeMachOwnership.hpp"
#include "NativeAuditTrailer.hpp"
#include <functional>
#include <memory>
#include <optional>
namespace Pro40Audio {
// Single non-RT service owner. Policies are trusted configuration retained by
// the owner, never requirements or authorization booleans from incoming packets.
// This registry grants only a private CONTROLLER endpoint. Native worker and
// HAL/audio lifetime admission remain the controller's responsibility.
class DuplexProvisionRegistry final {
public:
    using Verify=std::function<bool(const audit_token_t &)>;
    enum class Result:uint32_t {ok,identity,invalid,busy,absent,ownership,transport};
    struct Introduction {
        NativeMachPort controller{MACH_PORT_RIGHT_SEND};
        const audit_token_t token;
        const uint64_t registration;
        Introduction(NativeMachPort right,audit_token_t identity,uint64_t nonce)
            :controller(std::move(right)),token(identity),registration(nonce){}
    };
private:
    const Verify controllerPolicy_,helperPolicy_;
    std::optional<Introduction> owner_;
    static bool matches(audit_token_t a,audit_token_t b)noexcept{return std::memcmp(&a,&b,sizeof(a))==0;}
    static bool tokenValid(audit_token_t token)noexcept {
        const audit_token_t invalid=INVALID_AUDIT_TOKEN_VALUE;return !matches(token,invalid);
    }
    static bool sendRight(mach_port_t right)noexcept {
        mach_port_type_t type=0;
        return MACH_PORT_VALID(right) && mach_port_type(mach_task_self(),right,&type)==KERN_SUCCESS && (type&MACH_PORT_TYPE_SEND);
    }
    // Only proven receive-right death retires an uncooperative registration.
    // A signing/observation error does not grant permission to replace a live
    // controller. This never claims its worker or native resources were cleaned.
    bool retireDead()noexcept {
        if(!owner_)return true;
        mach_port_type_t type=0;
        if(mach_port_type(mach_task_self(),owner_->controller.get(),&type)==KERN_SUCCESS && (type&MACH_PORT_TYPE_DEAD_NAME)) {
            if(owner_->controller.close()!=KERN_SUCCESS)return false;
            owner_.reset();
        }
        return true;
    }
public:
    DuplexProvisionRegistry(Verify controller,Verify helper)
        :controllerPolicy_(std::move(controller)),helperPolicy_(std::move(helper)){}
    [[nodiscard]] Result bind(audit_token_t controller,uint64_t registration,mach_port_t endpoint) {
        if(!registration || !tokenValid(controller) || !sendRight(endpoint))return Result::invalid;
        if(!controllerPolicy_ || !controllerPolicy_(controller))return Result::identity;
        if(!retireDead())return Result::transport;
        if(owner_) {
            // A lost reply can be retried only for the identical registration.
            return matches(owner_->token,controller) && owner_->registration==registration && owner_->controller.get()==endpoint?
                Result::ok:Result::busy;
        }
        NativeMachPort retained(MACH_PORT_RIGHT_SEND);
        if(retained.retainSend(endpoint)!=KERN_SUCCESS)return Result::transport;
        owner_.emplace(std::move(retained),controller,registration);return Result::ok;
    }
    [[nodiscard]] Result unbind(audit_token_t controller,uint64_t registration) {
        if(!registration || !tokenValid(controller))return Result::invalid;
        if(!owner_)return Result::absent;
        if(!matches(owner_->token,controller) || owner_->registration!=registration)return Result::ownership;
        // Exact original owner may relinquish its control capability. Audio
        // cleanup is separately joined, never inferred from this operation.
        if(owner_->controller.close()!=KERN_SUCCESS)return Result::transport;
        owner_.reset();return Result::ok;
    }
    struct Lookup {Result result;std::unique_ptr<Introduction> introduction;};
    [[nodiscard]] Lookup lookup(audit_token_t helper) {
        if(!tokenValid(helper) || !helperPolicy_ || !helperPolicy_(helper))return {Result::identity,{}};
        if(!retireDead())return {Result::transport,{}};
        if(!owner_)return {Result::absent,{}};
        // Revalidate the original running controller at each introduction.
        if(!controllerPolicy_ || !controllerPolicy_(owner_->token))return {Result::identity,{}};
        if(!sendRight(owner_->controller.get()))return {Result::transport,{}};
        NativeMachPort retained(MACH_PORT_RIGHT_SEND);
        if(retained.retainSend(owner_->controller.get())!=KERN_SUCCESS)return {Result::transport,{}};
        return {Result::ok,std::make_unique<Introduction>(std::move(retained),owner_->token,owner_->registration)};
    }
};
}
