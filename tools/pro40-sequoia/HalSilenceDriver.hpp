// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <CoreAudio/AudioServerPlugIn.h>
// In-process factory prototype. Not packaged/installed as a HAL plug-in yet.
extern "C" void *Pro40SilenceFactory(CFAllocatorRef allocator,CFUUIDRef type);
// Lab diagnostic only; caller holds a driver reference. No real-time logging.
extern "C" UInt64 Pro40SilenceNotificationFailures(AudioServerPlugInDriverRef driver);
#ifdef PRO40_HAL_DUPLEX_AUDIO
#include "HalDuplexDriverState.hpp"
// Explicit in-process prototype configuration. No service lookup or native open
// is hidden in the factory. Call before its first StartIO attempt.
bool Pro40DuplexConfigure(AudioServerPlugInDriverRef driver,Pro40Audio::HalDuplexPrepare prepare);
extern "C" UInt64 Pro40DuplexInputFailures(AudioServerPlugInDriverRef driver);
extern "C" UInt64 Pro40DuplexOutputFailures(AudioServerPlugInDriverRef driver);
#endif
#ifdef PRO40_HAL_SYNTHETIC_AUDIO
#include <mach/mach.h>
extern "C" bool Pro40SyntheticConfigure(AudioServerPlugInDriverRef driver,mach_port_t service);
extern "C" UInt64 Pro40SyntheticInputFailures(AudioServerPlugInDriverRef driver);
#endif
#ifdef PRO40_HAL_CALLBACK_TIMING
#include "HalCallbackTiming.hpp"
extern "C" size_t Pro40SilenceDrainTiming(AudioServerPlugInDriverRef driver,
    void (*consumer)(const Pro40Audio::HalCallbackTimingRecord *,void *),void *context,UInt64 *dropped);
#endif
