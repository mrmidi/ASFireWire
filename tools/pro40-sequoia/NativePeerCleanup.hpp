// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SessionLifecycle.hpp"
namespace Pro40Audio {
// Supervisor evidence, not a transport acknowledgement. Remote confirmation
// means peer worker joined and its data endpoint closed. Local cleanup includes
// sender join and all locally owned endpoint closes. No IO or device lease here.
enum class LocalCleanup { incomplete, closed };
enum class RemoteCleanup { unconfirmed, confirmed, notRequired };
enum class PeerCleanupError : int32_t { none=0, localIncomplete=-1, remoteUnconfirmed=-2 };
constexpr int32_t peerCleanupStatus(LocalCleanup local,RemoteCleanup remote) noexcept {
    if(local!=LocalCleanup::closed)return int32_t(PeerCleanupError::localIncomplete);
    if(remote==RemoteCleanup::confirmed||remote==RemoteCleanup::notRequired)return 0;
    return int32_t(PeerCleanupError::remoteUnconfirmed);
}
inline bool finishPeerCleanup(Pro40::SessionLifecycle &lifecycle,int32_t evidence,
                              LocalCleanup local,RemoteCleanup remote) noexcept {
    return lifecycle.finish(evidence,peerCleanupStatus(local,remote));
}
}
