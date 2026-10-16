// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <dispatch/dispatch.h>
#include <xpc/xpc.h>
#include <functional>
#include <memory>
#include <string>
#include "VerifierExchange.hpp"
#include "AuthenticatedVerifierPeer.hpp"
#include "VerifierIdentityTransport.hpp"
namespace Pro40Lab {
uint64_t verifierMonotonicNS();
// Calls other than destruction must run on the supplied serial owner queue.
// An inactive, exclusively configured connection is borrowed at create (retained
// internally). The caller must release its own reference. No registration occurs.
class NativeVerifierConnection final {
    struct State;
    std::shared_ptr<State> state_;
    explicit NativeVerifierConnection(std::shared_ptr<State> state);
public:
    // Explicit lab mode: never negotiate or fall back based on peer messages.
    // Controller supplies request only; helper supplies independent policy/check only.
    struct Identity {
        std::optional<Pro40Audio::NativeIdentityRequest> request;
        std::array<uint8_t,20> policy{};
        std::function<int32_t(const audit_token_t&)> check;
    };
    struct Callbacks {
        std::function<void(ExchangeKind,const AuthenticatedVerifierPeer&)> progress;
        std::function<void(std::string)> failure;
        std::function<void()> finalized; // local connection only, never remote exit
        std::function<void(const Pro40Audio::NativeIdentityResult&,const AuthenticatedVerifierPeer&)> identity;
    };
    static std::shared_ptr<NativeVerifierConnection> create(xpc_connection_t connection,
        dispatch_queue_t serialQueue, bool helper, LaunchNonce nonce,
        uint64_t deadlineNS, const std::string& trustedPeerRequirement, Callbacks callbacks,
        std::optional<Identity> identity = {});
    ~NativeVerifierConnection();
    NativeVerifierConnection(const NativeVerifierConnection&)=delete;
    NativeVerifierConnection& operator=(const NativeVerifierConnection&)=delete;
    // Controller owner invokes inside its durable launch gate. Helper owner may
    // activate an accepted connection. This API cannot establish durable intent.
    void activate();
    // True must come from the durable release gate plus independent observations.
    void release(bool durableAndObserved);
    void poll(); // owner timer supplies timely deadline enforcement
    void cancel();
};
}
