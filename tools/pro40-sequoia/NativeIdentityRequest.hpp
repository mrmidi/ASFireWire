// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <mach/message.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

namespace Pro40Audio {
// Internal value contract, NOT a serialized ABI. Trusted controller supplies
// policy, unpredictable request ID and session from its own state; the token
// must be copied from the original successful kernel receive, never a peer field.
struct NativeIdentityRequest {
    uint32_t version=1;
    uint32_t role=0; // 1 source, 2 receiver
    uint64_t guid=0,epoch=0;
    std::array<uint8_t,16> requestID{};
    std::array<uint8_t,20> policyHash{}; // Exact local build pin; not shipping trust.
    audit_token_t token=INVALID_AUDIT_TOKEN_VALUE;
    template<size_t N> static bool nonzero(const std::array<uint8_t,N> &bytes)noexcept{
        for(auto value:bytes)if(value)return true;
        return false;
    }
    bool valid()const noexcept{
        const audit_token_t invalid=INVALID_AUDIT_TOKEN_VALUE,zero{};
        return version==1 && (role==1 || role==2) && guid && epoch &&
            nonzero(requestID) && nonzero(policyHash) &&
            std::memcmp(&token,&invalid,sizeof(token))!=0 &&
            std::memcmp(&token,&zero,sizeof(token))!=0;
    }
    bool operator==(const NativeIdentityRequest &other)const noexcept{
        return version==other.version && role==other.role && guid==other.guid &&
            epoch==other.epoch && requestID==other.requestID && policyHash==other.policyHash &&
            std::memcmp(&token,&other.token,sizeof(token))==0;
    }
};
struct NativeIdentityResult {
    uint32_t version=1;
    NativeIdentityRequest request;
    int32_t status=0;
};

// One owner, one attempt. No IO/authentication here. Only consume a result read
// from the private pipe of the trusted helper after exact framing and clean exit.
// Echoes bind an honest trusted helper's answer; they do not authenticate a helper.
class NativeIdentityResultGate {
    const NativeIdentityRequest expected_;
    const uint64_t deadline_;
    bool pending_;
public:
    NativeIdentityResultGate(NativeIdentityRequest request,uint64_t deadline)noexcept:
        expected_(request),deadline_(deadline),pending_(request.valid() && deadline!=0){}
    NativeIdentityResultGate(const NativeIdentityResultGate &)=delete;
    NativeIdentityResultGate &operator=(const NativeIdentityResultGate &)=delete;
    void cancel()noexcept{pending_=false;}
    [[nodiscard]] bool pending()const noexcept{return pending_;}
    // now/deadline must use the same monotonic unit and epoch. Timeout at equality.
    // Even malformed, failed-exit and stale attempts consume permission.
    [[nodiscard]] std::optional<int32_t> consume(const NativeIdentityResult &result,
        bool cleanExit,bool cancelled,uint64_t now)noexcept{
        const bool wasPending=pending_;pending_=false;
        if(!wasPending || !cleanExit || cancelled || now>=deadline_ ||
           result.version!=1 || !(result.request==expected_))return std::nullopt;
        return result.status; // Zero means verified; nonzero preserves Security rejection.
    }
};
}
