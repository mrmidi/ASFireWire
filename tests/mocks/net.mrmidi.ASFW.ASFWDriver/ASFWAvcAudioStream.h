// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ASFWAudioDevice.h"
#include <DriverKit/OSSharedPtr.h>
class ASFWAvcAudioStream : public IOUserAudioStream {
public:
    void Bind(ASFWAudioDevice* device) { owner = OSSharedPtr<ASFWAudioDevice>(device, OSRetain); }
    OSSharedPtr<ASFWAudioDevice> owner;
};
