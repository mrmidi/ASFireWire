// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeVerifierConnection.hpp"
namespace Pro40Lab {
// Serial-queue owner of a borrowed inactive listener. Completion proves local
// finalization only; it is neither remote exit evidence nor journal reconciliation.
class NativeVerifierServer final {
    struct State;
    std::shared_ptr<State> state_;
    explicit NativeVerifierServer(std::shared_ptr<State>);
public:
    using Completion = std::function<void(bool, std::string)>;
    static std::shared_ptr<NativeVerifierServer> create(xpc_connection_t,
        dispatch_queue_t, uint64_t deadlineNS, std::string trustedCallerRequirement, Completion,
        std::optional<NativeVerifierConnection::Identity> identity = {});
    ~NativeVerifierServer();
    NativeVerifierServer(const NativeVerifierServer&)=delete;
    NativeVerifierServer& operator=(const NativeVerifierServer&)=delete;
    void activate();
    void cancel();
};
}
