// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <bsm/libbsm.h>
#include <optional>
#include <sys/socket.h>
#include <sys/un.h>
namespace Pro40 {
// Kernel socket observation, not signature verification or launch authorization.
// Caller owns an accepted connection and retains the same unreaped child through
// verification. Never accept a token supplied in the peer's payload instead.
class SocketPeerCapture final {
    audit_token_t token_;
    explicit SocketPeerCapture(audit_token_t token):token_(token){}
public:
    [[nodiscard]] static std::optional<SocketPeerCapture> capture(int fd,pid_t child)noexcept{
        if(fd<0 || child<=0)return {};
        int type=0;socklen_t size=sizeof(type);
        if(getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size) || size!=sizeof(type) || type!=SOCK_STREAM)return {};
        sockaddr_un peer{};size=sizeof(peer);
        if(getpeername(fd,reinterpret_cast<sockaddr*>(&peer),&size) || peer.sun_family!=AF_UNIX)return {};
        audit_token_t token{};size=sizeof(token);
        // macOS SDK sys/un.h:93, LOCAL_PEERTOKEN returns the peer audit token.
        if(getsockopt(fd,SOL_LOCAL,LOCAL_PEERTOKEN,&token,&size) || size!=sizeof(token) ||
           audit_token_to_pid(token)!=child)return {};
        return SocketPeerCapture(token);
    }
    audit_token_t token()const noexcept{return token_;}
};
}
