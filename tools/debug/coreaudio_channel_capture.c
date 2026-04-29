#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    AudioQueueRef queue;
    CFRunLoopRef runLoop;
    FILE *file;
    UInt32 channels;
    UInt32 outputChannels;
    UInt32 channelIndex;
    UInt32 bytesPerFrame;
    uint64_t targetFrames;
    uint64_t framesWritten;
    bool done;
} CaptureState;

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s --device NAME_OR_UID (--channel N | --all-channels) --duration SECONDS --out PATH\n",
            argv0);
}

static void stage(const char *message) {
    fprintf(stderr, "%s\n", message);
    fflush(stderr);
}

static void print_status(const char *what, OSStatus status) {
    char fourcc[5] = {0};
    uint32_t code = CFSwapInt32HostToBig((uint32_t)status);
    memcpy(fourcc, &code, 4);
    bool printable = true;
    for (int i = 0; i < 4; ++i) {
        if (fourcc[i] < 32 || fourcc[i] > 126) {
            printable = false;
        }
    }
    if (printable) {
        fprintf(stderr, "%s failed: %d ('%s')\n", what, (int)status, fourcc);
    } else {
        fprintf(stderr, "%s failed: %d\n", what, (int)status);
    }
}

static bool cfstring_contains(CFStringRef value, const char *needle) {
    if (!value || !needle || !*needle) {
        return false;
    }
    char buffer[512];
    if (!CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
        return false;
    }
    return strstr(buffer, needle) != NULL;
}

static bool copy_cfstring(AudioObjectID object,
                          AudioObjectPropertySelector selector,
                          AudioObjectPropertyScope scope,
                          CFStringRef *out) {
    AudioObjectPropertyAddress address = {
        .mSelector = selector,
        .mScope = scope,
        .mElement = kAudioObjectPropertyElementMain,
    };
    UInt32 size = sizeof(CFStringRef);
    CFStringRef value = NULL;
    OSStatus status = AudioObjectGetPropertyData(object, &address, 0, NULL, &size, &value);
    if (status != noErr || !value) {
        return false;
    }
    *out = value;
    return true;
}

static UInt32 input_channel_count(AudioDeviceID device) {
    AudioObjectPropertyAddress address = {
        .mSelector = kAudioDevicePropertyStreamConfiguration,
        .mScope = kAudioDevicePropertyScopeInput,
        .mElement = kAudioObjectPropertyElementMain,
    };
    UInt32 size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(device, &address, 0, NULL, &size);
    if (status != noErr || size == 0) {
        return 0;
    }
    AudioBufferList *buffers = (AudioBufferList *)calloc(1, size);
    if (!buffers) {
        return 0;
    }
    status = AudioObjectGetPropertyData(device, &address, 0, NULL, &size, buffers);
    if (status != noErr) {
        free(buffers);
        return 0;
    }
    UInt32 channels = 0;
    for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i) {
        channels += buffers->mBuffers[i].mNumberChannels;
    }
    free(buffers);
    return channels;
}

static double nominal_sample_rate(AudioDeviceID device) {
    AudioObjectPropertyAddress address = {
        .mSelector = kAudioDevicePropertyNominalSampleRate,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain,
    };
    Float64 rate = 0.0;
    UInt32 size = sizeof(rate);
    OSStatus status = AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &rate);
    return status == noErr ? rate : 0.0;
}

static bool find_input_device(const char *needle,
                              AudioDeviceID *outDevice,
                              CFStringRef *outUID,
                              UInt32 *outChannels,
                              double *outRate) {
    AudioObjectPropertyAddress address = {
        .mSelector = kAudioHardwarePropertyDevices,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain,
    };
    UInt32 size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size);
    if (status != noErr || size == 0) {
        print_status("AudioObjectGetPropertyDataSize(devices)", status);
        return false;
    }
    AudioDeviceID *devices = (AudioDeviceID *)calloc(size / sizeof(AudioDeviceID), sizeof(AudioDeviceID));
    if (!devices) {
        return false;
    }
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, devices);
    if (status != noErr) {
        print_status("AudioObjectGetPropertyData(devices)", status);
        free(devices);
        return false;
    }

    const UInt32 count = size / sizeof(AudioDeviceID);
    for (UInt32 i = 0; i < count; ++i) {
        CFStringRef name = NULL;
        CFStringRef uid = NULL;
        UInt32 channels = input_channel_count(devices[i]);
        if (channels == 0) {
            continue;
        }
        bool haveName = copy_cfstring(devices[i], kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &name);
        bool haveUID = copy_cfstring(devices[i], kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, &uid);
        bool match = (haveName && cfstring_contains(name, needle)) || (haveUID && cfstring_contains(uid, needle));
        if (match) {
            char nameBuffer[512] = {0};
            char uidBuffer[512] = {0};
            if (name) {
                CFStringGetCString(name, nameBuffer, sizeof(nameBuffer), kCFStringEncodingUTF8);
            }
            if (uid) {
                CFStringGetCString(uid, uidBuffer, sizeof(uidBuffer), kCFStringEncodingUTF8);
            }
            fprintf(stderr,
                    "Selected input device id=%u name='%s' uid='%s' inputChannels=%u nominalRate=%.0f\n",
                    devices[i],
                    nameBuffer,
                    uidBuffer,
                    channels,
                    nominal_sample_rate(devices[i]));
            *outDevice = devices[i];
            *outUID = uid ? CFRetain(uid) : NULL;
            *outChannels = channels;
            *outRate = nominal_sample_rate(devices[i]);
            if (name) {
                CFRelease(name);
            }
            if (uid) {
                CFRelease(uid);
            }
            free(devices);
            return true;
        }
        if (name) {
            CFRelease(name);
        }
        if (uid) {
            CFRelease(uid);
        }
    }
    free(devices);
    return false;
}

static void write_le16(FILE *f, uint16_t value) {
    fputc(value & 0xff, f);
    fputc((value >> 8) & 0xff, f);
}

static void write_le32(FILE *f, uint32_t value) {
    fputc(value & 0xff, f);
    fputc((value >> 8) & 0xff, f);
    fputc((value >> 16) & 0xff, f);
    fputc((value >> 24) & 0xff, f);
}

static void write_wav_header(FILE *f, uint32_t sampleRate, uint16_t channels, uint32_t dataBytes) {
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    write_le32(f, 36 + dataBytes);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    write_le32(f, 16);
    write_le16(f, 1);
    write_le16(f, channels);
    write_le32(f, sampleRate);
    write_le32(f, sampleRate * channels * 3);
    write_le16(f, channels * 3);
    write_le16(f, 24);
    fwrite("data", 1, 4, f);
    write_le32(f, dataBytes);
}

static void write_int24(FILE *f, int32_t sample) {
    uint32_t value = (uint32_t)sample & 0x00ffffffU;
    fputc(value & 0xff, f);
    fputc((value >> 8) & 0xff, f);
    fputc((value >> 16) & 0xff, f);
}

static void input_callback(void *userData,
                           AudioQueueRef queue,
                           AudioQueueBufferRef buffer,
                           const AudioTimeStamp *startTime,
                           UInt32 numPackets,
                           const AudioStreamPacketDescription *packetDescriptions) {
    (void)startTime;
    (void)packetDescriptions;
    CaptureState *state = (CaptureState *)userData;
    UInt32 frames = numPackets;
    if (frames == 0 && state->bytesPerFrame > 0) {
        frames = buffer->mAudioDataByteSize / state->bytesPerFrame;
    }
    const float *samples = (const float *)buffer->mAudioData;
    for (UInt32 frame = 0; frame < frames && state->framesWritten < state->targetFrames; ++frame) {
        for (UInt32 ch = 0; ch < state->outputChannels; ++ch) {
            const UInt32 sourceChannel = (state->outputChannels == state->channels)
                                           ? ch
                                           : state->channelIndex;
            float value = samples[(frame * state->channels) + sourceChannel];
            if (value > 1.0f) {
                value = 1.0f;
            } else if (value < -1.0f) {
                value = -1.0f;
            }
            int32_t pcm = (int32_t)lrintf(value * 8388607.0f);
            write_int24(state->file, pcm);
        }
        state->framesWritten++;
    }
    if (state->framesWritten >= state->targetFrames) {
        state->done = true;
        AudioQueueStop(queue, false);
        if (state->runLoop) {
            CFRunLoopStop(state->runLoop);
        }
        return;
    }
    OSStatus status = AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
    if (status != noErr) {
        print_status("AudioQueueEnqueueBuffer(callback)", status);
        state->done = true;
        AudioQueueStop(queue, true);
        if (state->runLoop) {
            CFRunLoopStop(state->runLoop);
        }
    }
}

int main(int argc, char **argv) {
    const char *deviceNeedle = NULL;
    const char *outPath = NULL;
    int channelNumber = 0;
    bool allChannels = false;
    double duration = 0.0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            deviceNeedle = argv[++i];
        } else if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
            channelNumber = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--all-channels") == 0) {
            allChannels = true;
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration = atof(argv[++i]);
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            outPath = argv[++i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!deviceNeedle || !outPath || duration <= 0.0 ||
        (allChannels == (channelNumber > 0))) {
        usage(argv[0]);
        return 2;
    }

    AudioDeviceID device = kAudioObjectUnknown;
    CFStringRef uid = NULL;
    UInt32 channels = 0;
    double nominalRate = 0.0;
    if (!find_input_device(deviceNeedle, &device, &uid, &channels, &nominalRate) || !uid) {
        fprintf(stderr, "No matching input device found for '%s'\n", deviceNeedle);
        return 1;
    }
    if (!allChannels && (UInt32)channelNumber > channels) {
        fprintf(stderr, "Requested channel %d but device has %u input channels\n", channelNumber, channels);
        CFRelease(uid);
        return 1;
    }

    const double sampleRate = nominalRate > 0.0 ? nominalRate : 48000.0;
    CaptureState state = {
        .queue = NULL,
        .runLoop = CFRunLoopGetCurrent(),
        .file = NULL,
        .channels = channels,
        .outputChannels = allChannels ? channels : 1,
        .channelIndex = (UInt32)(channelNumber - 1),
        .bytesPerFrame = channels * sizeof(float),
        .targetFrames = (uint64_t)llround(duration * sampleRate),
        .framesWritten = 0,
        .done = false,
    };

    state.file = fopen(outPath, "wb+");
    if (!state.file) {
        fprintf(stderr, "Failed to open '%s': %s\n", outPath, strerror(errno));
        CFRelease(uid);
        return 1;
    }
    write_wav_header(state.file, (uint32_t)sampleRate, (uint16_t)state.outputChannels, 0);
    fflush(state.file);

    AudioStreamBasicDescription asbd;
    memset(&asbd, 0, sizeof(asbd));
    asbd.mSampleRate = sampleRate;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
    asbd.mBytesPerPacket = channels * sizeof(float);
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = channels * sizeof(float);
    asbd.mChannelsPerFrame = channels;
    asbd.mBitsPerChannel = 32;

    stage("Stage: AudioQueueNewInput");
    OSStatus status = AudioQueueNewInput(&asbd,
                                         input_callback,
                                         &state,
                                         CFRunLoopGetCurrent(),
                                         kCFRunLoopCommonModes,
                                         0,
                                         &state.queue);
    stage("Stage: AudioQueueNewInput returned");
    if (status != noErr) {
        print_status("AudioQueueNewInput", status);
        fclose(state.file);
        CFRelease(uid);
        return 1;
    }

    stage("Stage: AudioQueueSetProperty(CurrentDevice)");
    status = AudioQueueSetProperty(state.queue, kAudioQueueProperty_CurrentDevice, &uid, sizeof(uid));
    stage("Stage: AudioQueueSetProperty(CurrentDevice) returned");
    if (status != noErr) {
        print_status("AudioQueueSetProperty(CurrentDevice)", status);
        AudioQueueDispose(state.queue, true);
        fclose(state.file);
        CFRelease(uid);
        return 1;
    }

    const UInt32 bufferFrames = 1024;
    const UInt32 bufferBytes = bufferFrames * state.bytesPerFrame;
    stage("Stage: allocate/enqueue input buffers");
    for (int i = 0; i < 4; ++i) {
        AudioQueueBufferRef buffer = NULL;
        status = AudioQueueAllocateBuffer(state.queue, bufferBytes, &buffer);
        if (status != noErr) {
            print_status("AudioQueueAllocateBuffer", status);
            AudioQueueDispose(state.queue, true);
            fclose(state.file);
            CFRelease(uid);
            return 1;
        }
        status = AudioQueueEnqueueBuffer(state.queue, buffer, 0, NULL);
        if (status != noErr) {
            print_status("AudioQueueEnqueueBuffer", status);
            AudioQueueDispose(state.queue, true);
            fclose(state.file);
            CFRelease(uid);
            return 1;
        }
    }
    stage("Stage: allocate/enqueue input buffers returned");

    const time_t wallStart = time(NULL);
    stage("Stage: AudioQueueStart");
    status = AudioQueueStart(state.queue, NULL);
    stage("Stage: AudioQueueStart returned");
    if (status != noErr) {
        print_status("AudioQueueStart", status);
        AudioQueueDispose(state.queue, true);
        fclose(state.file);
        CFRelease(uid);
        return 1;
    }

    while (!state.done) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
        if (difftime(time(NULL), wallStart) > duration + 5.0) {
            fprintf(stderr, "Timed out after %.0f seconds; stopping with %llu/%llu frames\n",
                    difftime(time(NULL), wallStart),
                    (unsigned long long)state.framesWritten,
                    (unsigned long long)state.targetFrames);
            state.done = true;
            AudioQueueStop(state.queue, true);
            break;
        }
    }

    AudioQueueDispose(state.queue, true);
    const uint32_t dataBytes = (uint32_t)(state.framesWritten * state.outputChannels * 3);
    write_wav_header(state.file, (uint32_t)sampleRate, (uint16_t)state.outputChannels, dataBytes);
    fflush(state.file);
    fclose(state.file);
    CFRelease(uid);

    fprintf(stderr,
            "Captured %llu frames from %s to %s (duration %.6f seconds)\n",
            (unsigned long long)state.framesWritten,
            allChannels ? "all channels" : "selected channel",
            outPath,
            (double)state.framesWritten / sampleRate);
    return state.framesWritten > 0 ? 0 : 1;
}
