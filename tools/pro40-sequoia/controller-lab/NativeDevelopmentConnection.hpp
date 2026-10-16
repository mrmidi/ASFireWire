// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeVerifierConnection.hpp"
#if OS_OBJECT_USE_OBJC
#error Compile this adapter with OS_OBJECT_USE_OBJC=0 to match the C++ connection owner
#endif
namespace Pro40Lab {
struct DevelopmentControllerPreparation {
    std::shared_ptr<NativeVerifierConnection> connection;
    bool cleanupPending=false; // finalized callback owed even on post-creation refusal
};
DevelopmentControllerPreparation prepareDevelopmentController(dispatch_queue_t queue,
    LaunchNonce nonce,uint64_t deadlineNS,NativeVerifierConnection::Callbacks callbacks,
    std::optional<Pro40Audio::NativeIdentityRequest> originalRequest = {});
// Owner queue only. Validates this running executable and derives the peer
// requirement from its package. Never accepts a peer-supplied path or requirement.
std::shared_ptr<NativeVerifierConnection> prepareDevelopmentConnection(
    xpc_connection_t connection, dispatch_queue_t queue, bool helper,
    LaunchNonce nonce, uint64_t deadlineNS, NativeVerifierConnection::Callbacks callbacks,
    std::optional<Pro40Audio::NativeIdentityRequest> originalRequest = {});
}
