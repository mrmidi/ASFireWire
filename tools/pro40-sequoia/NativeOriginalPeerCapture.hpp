// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeAuditTrailer.hpp"
#include "NativeMachOwnership.hpp"
#include <bsm/libbsm.h>
namespace Pro40Audio {
// Capture for the private, header-only child introduction used by the controller
// lab. Not a production audio admission protocol or code-signing authorization.
// Construction requires a successful owned kernel receive, never arbitrary bytes.
// The caller still owns child liveness/reaping and the local receive endpoint.
class NativeOriginalPeerCapture final {
    audit_token_t token_;
    explicit NativeOriginalPeerCapture(audit_token_t token):token_(token){}
public:
    template<size_t Capacity>
    [[nodiscard]] static std::optional<NativeOriginalPeerCapture> capture(
        const NativeMachReceived<Capacity> &received,mach_port_t local,pid_t child)noexcept{
        if(!received.received() || !MACH_PORT_VALID(local) || child<=0)return {};
        mach_msg_header_t header{};
        std::memcpy(&header,received.bytes().data(),sizeof(header));
        if(header.msgh_size!=sizeof(header) || (header.msgh_bits&MACH_MSGH_BITS_COMPLEX) ||
           header.msgh_local_port!=local || header.msgh_remote_port!=MACH_PORT_NULL ||
           header.msgh_voucher_port!=MACH_PORT_NULL)return {};
        const auto token=nativeAuditToken(received.bytes());
        if(!token || audit_token_to_pid(*token)!=child)return {};
        return NativeOriginalPeerCapture(*token);
    }
    audit_token_t token()const noexcept{return token_;}
};
}
