// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeVerifierConnection.hpp"
namespace Pro40Lab {
// No dispatch/XPC object types in this ABI: ARC session code borrows its serial
// queue using (__bridge void *). The implementation retains native resources.
class ControllerConnection {
public:
    virtual ~ControllerConnection()=default;
    virtual void activate()=0;
    virtual void release()=0; // called only inside durable release action
    virtual void cancel()=0;
};
struct ControllerPreparation {
    std::shared_ptr<ControllerConnection> connection;
    bool cleanupPending=false;
};
ControllerPreparation prepareControllerConnection(void *serialQueue,LaunchNonce nonce,uint64_t deadline,
    NativeVerifierConnection::Callbacks callbacks,
    std::optional<Pro40Audio::NativeIdentityRequest> originalRequest = {});
}
