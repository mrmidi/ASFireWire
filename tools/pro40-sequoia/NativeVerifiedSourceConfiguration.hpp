// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeIdentityRequest.hpp"
#include "NativeSessionRunner.hpp"
#include <utility>
namespace Pro40Audio {
// Immutable local configuration shared by verification and runner entry. This
// does not authenticate a token: the caller must supply the original kernel
// receive token, retain that peer, and keep borrowed option contexts alive.
class NativeVerifiedSourceConfiguration final {
    NativeIdentityRequest request_;
    Pro40::NativeSessionOptions options_;
    NativeVerifiedSourceConfiguration(NativeIdentityRequest request,Pro40::NativeSessionOptions options):
        request_(request),options_(std::move(options)){}
public:
    // Epoch and unpredictable nonce come from the controller, never peer fields.
    // Epoch is a controller session identity, not a FireWire bus generation.
    [[nodiscard]] static std::optional<NativeVerifiedSourceConfiguration> create(
        Pro40::NativeSessionOptions options,uint64_t epoch,
        std::array<uint8_t,16> nonce,std::array<uint8_t,20> policy,audit_token_t capturedToken){
        NativeIdentityRequest request;
        request.role=1;request.guid=options.guid;request.epoch=epoch;
        request.requestID=nonce;request.policyHash=policy;request.token=capturedToken;
        if(!request.valid())return std::nullopt;
        // Runner option validation stays in the runner, before native entry.
        return NativeVerifiedSourceConfiguration(request,std::move(options));
    }
    NativeIdentityRequest request()const noexcept{return request_;}
    Pro40::NativeSessionOptions options()const{return options_;}
};
}
