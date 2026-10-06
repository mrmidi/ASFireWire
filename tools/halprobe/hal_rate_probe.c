// SPDX-License-Identifier: Apache-2.0
// Active HAL configuration experiment. Deliberately restricted to the silent
// virtual lab UID: this tool cannot change a physical FireWire device.
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static const char *probe_uid = "ASFWConfigurationProbe";
static atomic_uint_fast64_t callbacks = 0;
static volatile sig_atomic_t interrupted = 0;
static void on_signal(int value) { (void)value; interrupted = 1; }
static bool read_property(AudioObjectID id, AudioObjectPropertySelector selector,
                          AudioObjectPropertyScope scope, void *data, UInt32 size) {
    AudioObjectPropertyAddress address = {selector, scope, kAudioObjectPropertyElementMain};
    return AudioObjectGetPropertyData(id, &address, 0, NULL, &size, data) == noErr;
}
static AudioObjectID find_probe(void) {
    AudioObjectPropertyAddress address = {kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &bytes) != noErr) return 0;
    AudioObjectID *devices = malloc(bytes);
    if (!devices) return 0;
    AudioObjectID result = 0;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &bytes, devices) == noErr) {
        for (UInt32 i = 0; i < bytes / sizeof(*devices); ++i) {
            CFStringRef uid = NULL;
            if (!read_property(devices[i], kAudioDevicePropertyDeviceUID,
                kAudioObjectPropertyScopeGlobal, &uid, sizeof(uid)) || !uid) continue;
            char text[256];
            bool matches = CFStringGetCString(uid, text, sizeof(text), kCFStringEncodingUTF8) &&
                strcmp(text, probe_uid) == 0;
            CFRelease(uid);
            if (matches) { result = devices[i]; break; }
        }
    }
    free(devices);
    return result;
}
static bool stream(AudioObjectID device, AudioObjectPropertyScope scope,
                   AudioObjectID *id, AudioStreamBasicDescription *format) {
    AudioObjectPropertyAddress address = {kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain};
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, NULL, &bytes) != noErr || bytes != sizeof(*id)) return false;
    return read_property(device, kAudioDevicePropertyStreams, scope, id, sizeof(*id)) &&
        read_property(*id, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, format, sizeof(*format));
}
static OSStatus set_rate(AudioObjectID device, double rate) {
    AudioObjectPropertyAddress address = {kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    return AudioObjectSetPropertyData(device, &address, 0, NULL, sizeof(rate), &rate);
}
static OSStatus set_stream_rate(AudioObjectID output, double rate) {
    AudioStreamBasicDescription format = {0};
    if (!read_property(output, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal,
        &format, sizeof(format))) return kAudioHardwareUnspecifiedError;
    format.mSampleRate = rate;
    format.mChannelsPerFrame = rate > 96000 ? 8 : rate > 48000 ? 12 : 16;
    format.mBytesPerFrame = format.mChannelsPerFrame * sizeof(Float32);
    format.mBytesPerPacket = format.mBytesPerFrame;
    AudioObjectPropertyAddress address = {kAudioStreamPropertyPhysicalFormat,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    Boolean settable = false;
    OSStatus check = AudioObjectIsPropertySettable(output, &address, &settable);
    if (check != noErr || !settable) return kAudioHardwareUnsupportedOperationError;
    return AudioObjectSetPropertyData(output, &address, 0, NULL, sizeof(format), &format);
}
static bool coherent(AudioObjectID device, double rate, AudioObjectID input_id, AudioObjectID output_id) {
    AudioObjectID input = 0, output = 0;
    AudioStreamBasicDescription in = {0}, out = {0};
    AudioStreamBasicDescription physical_in = {0}, physical_out = {0};
    double nominal = 0;
    UInt32 period = 0;
    const UInt32 channels = rate > 96000 ? 8 : rate > 48000 ? 12 : 16;
    const UInt32 expected_period = rate > 96000 ? 49152 : rate > 48000 ? 24576 : 12288;
    return find_probe() == device && stream(device, kAudioObjectPropertyScopeInput, &input, &in) &&
        stream(device, kAudioObjectPropertyScopeOutput, &output, &out) &&
        read_property(input, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, &physical_in, sizeof(physical_in)) &&
        read_property(output, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, &physical_out, sizeof(physical_out)) &&
        read_property(device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &nominal, sizeof(nominal)) &&
        read_property(device, 'ring', kAudioObjectPropertyScopeGlobal, &period, sizeof(period)) &&
        nominal == rate && in.mSampleRate == rate && out.mSampleRate == rate &&
        input == input_id && output == output_id && in.mChannelsPerFrame == channels &&
        out.mChannelsPerFrame == channels && period == expected_period &&
        physical_in.mSampleRate == rate && physical_out.mSampleRate == rate &&
        physical_in.mChannelsPerFrame == channels && physical_out.mChannelsPerFrame == channels;
}
static OSStatus io(AudioObjectID device, const AudioTimeStamp *now, const AudioBufferList *input,
    const AudioTimeStamp *input_time, AudioBufferList *output, const AudioTimeStamp *output_time, void *context) {
    (void)device; (void)now; (void)input; (void)input_time; (void)output_time; (void)context;
    if (output) for (UInt32 i = 0; i < output->mNumberBuffers; ++i)
        if (output->mBuffers[i].mData) memset(output->mBuffers[i].mData, 0, output->mBuffers[i].mDataByteSize);
    atomic_fetch_add_explicit(&callbacks, 1, memory_order_relaxed);
    return noErr;
}
static bool wait_for_commit(AudioObjectID device, double rate, AudioObjectID input, AudioObjectID output) {
    uint64_t at_commit = UINT64_MAX;
    for (unsigned i = 0; i < 500 && !interrupted; ++i) {
        if (coherent(device, rate, input, output)) {
            uint64_t count = atomic_load_explicit(&callbacks, memory_order_relaxed);
            if (at_commit == UINT64_MAX) at_commit = count;
            // The original IOProc must resume; there is no Start call here.
            if (count >= at_commit + 20) return true;
        } else at_commit = UINT64_MAX;
        usleep(10000);
    }
    return false;
}
int main(int argc, char **argv) {
    bool format_request = argc == 2 && strcmp(argv[1], "--stream-format") == 0;
    if (argc > 2 || (argc == 2 && !format_request)) {
        fprintf(stderr, "usage: hal_rate_probe [--stream-format]\n"); return 1;
    }
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    AudioObjectID device = find_probe(), input = 0, output = 0;
    AudioStreamBasicDescription in = {0}, out = {0};
    double original = 0;
    if (!device || !stream(device, kAudioObjectPropertyScopeInput, &input, &in) ||
        !stream(device, kAudioObjectPropertyScopeOutput, &output, &out) ||
        !read_property(device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &original, sizeof(original))) {
        fprintf(stderr, "Silent virtual configuration probe unavailable; no device was changed.\n"); return 1;
    }
    AudioDeviceIOProcID proc = NULL;
    if (AudioDeviceCreateIOProcID(device, io, NULL, &proc) != noErr) return 1;
    if (AudioDeviceStart(device, proc) != noErr) { AudioDeviceDestroyIOProcID(device, proc); return 1; }
    usleep(200000);
    bool passed = atomic_load(&callbacks) > 0;
    const double sequence[] = {48000, 96000, 44100, 32000, 88200, 176400, 192000, 48000};
    for (size_t i = 0; passed && i < sizeof(sequence) / sizeof(sequence[0]) && !interrupted; ++i) {
        uint64_t before = atomic_load(&callbacks);
        OSStatus result = format_request ? set_stream_rate(output, sequence[i]) : set_rate(device, sequence[i]);
        bool observed = wait_for_commit(device, sequence[i], input, output);
        passed = result == noErr && observed;
        printf("{\"origin\":\"%s\",\"device\":%u,\"input\":%u,\"output\":%u,\"requested\":%.0f,\"set_status\":%d,\"coherent_and_resumed\":%s,\"callbacks\":%llu}\n",
            format_request ? "stream-format" : "nominal-rate", device, input, output, sequence[i], (int)result, observed ? "true" : "false",
            (unsigned long long)(atomic_load(&callbacks) - before));
        fflush(stdout);
    }
    // Always attempt restoration, including on interruption or failed apply.
    OSStatus restored = set_rate(device, original);
    bool canceled = interrupted != 0;
    interrupted = 0;
    bool restoration = restored == noErr && wait_for_commit(device, original, input, output);
    printf("{\"restored\":%.0f,\"restoration_confirmed\":%s}\n", original, restoration ? "true" : "false");
    AudioDeviceStop(device, proc); AudioDeviceDestroyIOProcID(device, proc);
    return passed && restoration && !canceled ? 0 : 2;
}
