// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Host stand-in for the IIG-generated ASFWMidiNub. Records the order in which
// the publisher configures it, so the host suite can pin that the endpoint id
// is handed over explicitly after Create and before registration (the midi
// branch read it in Start, before it was set, cached 0 and crashed the dext).

#pragma once

#include <DriverKit/IOService.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSSharedPtr.h>

#include <cstdint>
#include <string>
#include <vector>

class ASFWMidiNub : public IOService {
public:
    ~ASFWMidiNub() override {
        if (published) published->release();
    }

    std::vector<std::string> calls;
    uint64_t endpointId{0};
    uint64_t endpointIdAtPublish{0};
    OSDictionary* published{nullptr};
    bool registered{false};
    uint32_t notifications{0};
    uint32_t terminations{0};

    void SetEndpointId(uint64_t id) {
        calls.emplace_back("SetEndpointId");
        endpointId = id;
    }
    uint64_t GetEndpointId() const { return endpointId; }

    kern_return_t CopyProperties(OSDictionary** out) override {
        calls.emplace_back("CopyProperties");
        *out = OSDictionary::withCapacity(32);
        return kIOReturnSuccess;
    }
    kern_return_t SetProperties(OSDictionary* properties) override {
        calls.emplace_back("SetProperties");
        if (properties) {
            properties->retain();
            if (published) published->release();
            published = properties;
        }
        return kIOReturnSuccess;
    }
    // The real nub refuses to register without an endpoint id.
    kern_return_t Publish() {
        calls.emplace_back("Publish");
        endpointIdAtPublish = endpointId;
        if (endpointId == 0) return kIOReturnNotReady;
        registered = true;
        return kIOReturnSuccess;
    }
    bool NotifyMidiReceived() {
        ++notifications;
        return true;
    }
    kern_return_t Terminate(uint64_t) override {
        ++terminations;
        return kIOReturnSuccess;
    }
};
