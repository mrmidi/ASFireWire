#pragma once

#ifdef ASFW_HOST_TEST

#include <cstdint>

using IOUserAudioObjectID = uint64_t;
using IOUserAudioClassID = uint32_t;
using IOUserAudioFormatID = uint32_t;
using IOUserAudioFormatFlags = uint32_t;
using IOUserAudioTransportType = uint32_t;
using IOUserAudioClockAlgorithm = uint32_t;

enum IOUserAudioIOOperation : uint32_t {
    IOUserAudioIOOperationBeginRead = 1,
    IOUserAudioIOOperationWriteEnd = 2,
};

enum IOUserAudioObjectPropertyScope : uint32_t {
    IOUserAudioObjectPropertyScopeInput = static_cast<uint32_t>('inpt'),
    IOUserAudioObjectPropertyScopeOutput = static_cast<uint32_t>('outp'),
    IOUserAudioObjectPropertyScopeGlobal = static_cast<uint32_t>('glob'),
};

class IOUserAudioDevice {
public:
    virtual ~IOUserAudioDevice() = default;

    virtual void GetCurrentZeroTimestamp(uint64_t* sampleTime, uint64_t* hostTime) {
        if (sampleTime) {
            *sampleTime = 0;
        }
        if (hostTime) {
            *hostTime = 0;
        }
    }

    virtual void UpdateCurrentZeroTimestamp(uint64_t, uint64_t) { /* no-op stub */ }
};

class IOUserAudioStream {
public:
    virtual ~IOUserAudioStream() = default;
};

#endif // ASFW_HOST_TEST
