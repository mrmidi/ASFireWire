// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Host stand-in for the IIG-generated ASFWAudioDriver.h. It declares only the
// action handlers the host tests drive, and expands IMPL the way the IIG
// preprocessor does (Class::Method_Impl(Class_Method_Args)), so the real
// handler bodies compile unchanged.

#pragma once

#include <DriverKit/IOService.h>
#include <DriverKit/OSAction.h>
#include <AudioDriverKit/AudioDriverKit.h>

#include "ASFWAudioDevice.h"
#include "ASFWAudioNub.h"

#include <cstdint>
#include <vector>

struct ASFWAudioDriver_IVars;

#define ASFWAudioDriver_ZtsAnchorReady_Args OSAction* action, uint64_t generation
#define ASFWAudioDriver_TxPreparationReady_Args OSAction* action, uint64_t generation

#ifndef IMPL
#define IMPL(className, methodName) className::methodName##_Impl(className##_##methodName##_Args)
#endif

class ASFWAudioDriver : public IOService {
public:
    ASFWAudioDriver_IVars* ivars{nullptr};

    // IOUserAudioDriver::RemoveObject: records what the graph teardown detaches.
    kern_return_t RemoveObject(OSObject* object) {
        removedObjects.push_back(object);
        if (removedAt == 0) removedAt = ++TeardownSequence();
        return kIOReturnSuccess;
    }
    std::vector<OSObject*> removedObjects;
    uint32_t removedAt{0};

    // IOUserAudioDriver::StopDevice: records the IO stop the teardown issues.
    kern_return_t StopDevice(uint32_t objectId, IOUserAudioStartStopFlags) {
        stoppedDevices.push_back(objectId);
        stopDeviceAt = ++TeardownSequence();
        return kIOReturnSuccess;
    }
    std::vector<uint32_t> stoppedDevices;
    uint32_t stopDeviceAt{0};

    void ZtsAnchorReady_Impl(ASFWAudioDriver_ZtsAnchorReady_Args);
    void TxPreparationReady_Impl(ASFWAudioDriver_TxPreparationReady_Args);
};
