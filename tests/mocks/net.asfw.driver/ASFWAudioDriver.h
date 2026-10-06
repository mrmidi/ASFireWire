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
        return kIOReturnSuccess;
    }
    std::vector<OSObject*> removedObjects;

    void ZtsAnchorReady_Impl(ASFWAudioDriver_ZtsAnchorReady_Args);
    void TxPreparationReady_Impl(ASFWAudioDriver_TxPreparationReady_Args);
};
