// SPDX-License-Identifier: Apache-2.0
#pragma once
#import <ServiceManagement/ServiceManagement.h>
#include <atomic>
#include <memory>

namespace Pro40Lab {
// SDK SMAppService.h:134-170, 203-218; SMErrors.h:55-69. Raw status is
// evidence, not an ownership decision or helper-exit receipt. Contradictory
// NSError/BOOL results are unrepresentable and must block the journal owner.
inline NSDictionary *serviceResult(BOOL success, NSError *error, NSInteger status) {
    if ((success && error) || (!success && (!error || error.domain.length == 0))) return nil;
    const bool conflict = error && [error.domain isEqual:SMAppServiceErrorDomain] &&
        error.code == kSMErrorAlreadyRegistered;
    return @{@"success": @(success != NO), @"error_domain": error ? [error.domain copy] : @"",
        @"error_code": error ? @(error.code) : @0, @"raw_status": @(status),
        @"ownership_conflict": @(conflict)};
}

// Single controller queue owns calls and authorization. Service is SMAppService
// in production and a fake in tests. No journal permission is inferred here.
// The caller must retain its journal owner in the completion until delivery;
// a timeout cannot cancel an OS operation or release its ownership obligation.
template<class Service>
class ServiceOperations final {
    Service *service_;
    dispatch_queue_t queue_;
    bool registerIssued_ = false, registerAttempted_ = false, unregisterIssued_ = false;
public:
    ServiceOperations(Service *service, dispatch_queue_t controllerQueue)
        : service_(service), queue_(controllerQueue) {}
    ServiceOperations(const ServiceOperations&) = delete;
    ServiceOperations& operator=(const ServiceOperations&) = delete;
    template<class Authorize>
    [[nodiscard]] NSDictionary *registerOnce(Authorize&& authorize) {
        if (registerIssued_ || !service_ || !queue_) return nil;
        registerIssued_ = true;
        if (!authorize()) return nil;
        registerAttempted_ = true;
        NSError *error = nil;
        BOOL success = [service_ registerAndReturnError:&error];
        return serviceResult(success, error, static_cast<NSInteger>(service_.status));
    }
    template<class Authorize>
    [[nodiscard]] bool unregisterOnce(Authorize&& authorize, void (^completion)(NSDictionary *)) {
        if (!registerAttempted_ || unregisterIssued_ || !service_ || !queue_ || !completion) return false;
        unregisterIssued_ = true;
        if (!authorize()) return false;
        // SDK callback runs on a default target queue. Retain all delivery state
        // independently of this wrapper; marshal to the owner's serial queue.
        Service *service = service_;
        dispatch_queue_t queue = queue_;
        auto delivered = std::make_shared<std::atomic<bool>>(false);
        [service unregisterWithCompletionHandler:^(NSError *error) {
            if (delivered->exchange(true)) return;
            dispatch_async(queue, ^{
                completion(serviceResult(error == nil, error, static_cast<NSInteger>(service.status)));
            });
        }];
        return true;
    }
};
using NativeServiceOperations = ServiceOperations<SMAppService>;
}
