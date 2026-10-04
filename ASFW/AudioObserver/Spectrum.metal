#include <metal_stdlib>
using namespace metal;
struct SpectrumParams { ulong writeEnd; uint ringFrames; uint channels; uint channel; uint sampleRate; uint otherChannel; uint transform; uint fftSize; uint window; };
struct SpectrumVertex { float4 position [[position]]; };

// Real FFT: pack windowed even/odd samples into an N/2 complex transform.
// Dynamic threadgroup storage is N/2 * sizeof(float2): 32 KiB at N=8192.
// Stereo power uses two transforms, reusing this storage, never averaging PCM.
inline float spectrumSample(device const float* ring, thread const SpectrumParams& p,
                            uint i, bool otherLane) {
    const uint frame = uint((p.writeEnd - p.fftSize + i) % p.ringFrames);
    const ulong base = ulong(frame) * p.channels;
    float sample = ring[base + p.channel], other = ring[base + p.otherChannel];
    sample = isfinite(sample) ? sample : 0;
    other = isfinite(other) ? other : 0;
    if (p.transform == 1) sample = (sample + other) * 0.70710678118f;
    else if (p.transform == 2) sample = (sample - other) * 0.70710678118f;
    else if (otherLane) sample = other;
    const float angle = 2.0f * M_PI_F * float(i) / float(p.fftSize);
    const float window = p.window == 1 ? 0.54f - 0.46f * cos(angle)
        : p.window == 2 ? 0.42f - 0.5f * cos(angle) + 0.08f * cos(2 * angle)
        : 0.5f - 0.5f * cos(angle);
    return sample * window;
}
inline void spectrumTransform(device const float* ring, thread const SpectrumParams& p,
                              threadgroup float2* values, uint tid, uint threads, bool otherLane) {
    const uint complexSize = p.fftSize / 2;
    const uint stages = uint(log2(float(complexSize)));
    for (uint i = tid; i < complexSize; i += threads) {
        uint reversed = 0, bits = i;
        for (uint j = 0; j < stages; ++j) { reversed = (reversed << 1) | (bits & 1); bits >>= 1; }
        values[reversed] = float2(spectrumSample(ring, p, 2 * i, otherLane),
                                 spectrumSample(ring, p, 2 * i + 1, otherLane));
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint width = 2; width <= complexSize; width <<= 1) {
        const uint halfWidth = width >> 1;
        for (uint butterfly = tid; butterfly < complexSize / 2; butterfly += threads) {
            const uint j = butterfly % halfWidth;
            const uint a = (butterfly / halfWidth) * width + j, b = a + halfWidth;
            const float angle = -2.0f * M_PI_F * float(j) / float(width);
            const float2 v = values[b], u = values[a];
            const float2 product(v.x * cos(angle) - v.y * sin(angle),
                                 v.x * sin(angle) + v.y * cos(angle));
            values[a] = u + product; values[b] = u - product;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
}
inline float spectrumAmplitude(threadgroup float2* values, thread const SpectrumParams& p, uint bin) {
    const uint halfSize = p.fftSize / 2;
    // Reconstruct X[k] = E[k] + exp(-i*2*pi*k/N)*O[k]. Modulo also
    // handles DC/Nyquist: Z[0].real +/- Z[0].imag, with no doubled gain.
    const float2 a = values[bin % halfSize];
    const float2 b = values[(halfSize - bin) % halfSize] * float2(1, -1);
    const float2 even = (a + b) * 0.5f, difference = a - b;
    const float2 odd(difference.y * 0.5f, -difference.x * 0.5f);
    const float angle = -2.0f * M_PI_F * float(bin) / float(p.fftSize);
    const float2 full = even + float2(odd.x * cos(angle) - odd.y * sin(angle),
                                     odd.x * sin(angle) + odd.y * cos(angle));
    const float gain = p.window == 1 ? 0.54f : p.window == 2 ? 0.42f : 0.5f;
    const float scale = (bin == 0 || bin == halfSize) ? 1.0f / (p.fftSize * gain) : 2.0f / (p.fftSize * gain);
    return length(full) * scale;
}
kernel void asfwSpectrumFFT(device const float* ring [[buffer(0)]],
    device float* amplitudes [[buffer(1)]], constant SpectrumParams& params [[buffer(2)]],
    threadgroup float2* values [[threadgroup(0)]],
    uint tid [[thread_index_in_threadgroup]], uint3 size [[threads_per_threadgroup]]) {
    SpectrumParams p = params;
    spectrumTransform(ring, p, values, tid, size.x, false);
    for (uint bin = tid; bin <= p.fftSize / 2; bin += size.x)
        amplitudes[bin] = spectrumAmplitude(values, p, bin);
    if (p.transform == 3) {
        // Every lane finishes reading the first transform before scratch reuse.
        threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);
        spectrumTransform(ring, p, values, tid, size.x, true);
        for (uint bin = tid; bin <= p.fftSize / 2; bin += size.x) {
            const float left = amplitudes[bin], right = spectrumAmplitude(values, p, bin);
            amplitudes[bin] = sqrt((left * left + right * right) * 0.5f);
        }
    }
}

struct SpectrogramParams { SpectrumParams fft; ulong firstSlice; uint hop; uint columns; };
kernel void asfwSpectrogramSTFT(device const float* ring [[buffer(0)]],
    device ulong* stamps [[buffer(1)]], constant SpectrogramParams& params [[buffer(2)]],
    device float* stereoScratch [[buffer(3)]],
    texture2d<float, access::write> history [[texture(0)]],
    threadgroup float2* values [[threadgroup(0)]],
    uint tid [[thread_index_in_threadgroup]], uint3 group [[threadgroup_position_in_grid]],
    uint3 size [[threads_per_threadgroup]]) {
    const ulong slice = params.firstSlice + group.x;
    SpectrumParams p = params.fft;
    p.writeEnd = slice * params.hop;
    const uint bins = p.fftSize / 2 + 1;
    const uint column = uint(slice % params.columns);
    spectrumTransform(ring, p, values, tid, size.x, false);
    for (uint bin = tid; bin < bins; bin += size.x) {
        const float amplitude = spectrumAmplitude(values, p, bin);
        if (p.transform == 3) stereoScratch[group.x * bins + bin] = amplitude;
        else history.write(float4(20.0f * log10(max(amplitude, 1e-6f))), uint2(column, bin));
    }
    if (p.transform == 3) {
        threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);
        spectrumTransform(ring, p, values, tid, size.x, true);
        for (uint bin = tid; bin < bins; bin += size.x) {
            const float left = stereoScratch[group.x * bins + bin], right = spectrumAmplitude(values, p, bin);
            const float amplitude = sqrt((left * left + right * right) * 0.5f);
            history.write(float4(20.0f * log10(max(amplitude, 1e-6f))), uint2(column, bin));
        }
    }
    if (tid == 0) stamps[column] = slice + 1;
}
struct SpectrogramDisplay { ulong latestSlice; uint columns; uint fftSize; uint sampleRate; uint pixelHeight; };
struct SpectrogramVertex { float4 position [[position]]; float2 uv; };
vertex SpectrogramVertex asfwSpectrogramVertex(uint id [[vertex_id]]) {
    float2 uv = float2((id << 1) & 2, id & 2);
    return { float4(uv * 2 - 1, 0, 1), float2(uv.x, 1 - uv.y) };
}
// Shared amplitude palette for both history views and their legend.
inline float3 spectrogramColor(float db) {
    float t = clamp((db + 100) / 100, 0.0f, 1.0f);
    float3 color = t < 0.33f ? mix(float3(0.025,0.035,0.05), float3(0.08,0.2,0.55), t / 0.33f)
        : t < 0.66f ? mix(float3(0.08,0.2,0.55), float3(0.1,0.85,0.7), (t-0.33f)/0.33f)
        : t < 0.85f ? mix(float3(0.1,0.85,0.7), float3(1,0.8,0.2), (t-0.66f)/0.19f)
        : t < 0.93f ? mix(float3(1,0.8,0.2), float3(1,0.4,0.08), (t-0.85f)/0.08f)
        : mix(float3(1,0.4,0.08), float3(1,0.08,0.04), (t-0.93f)/0.07f);
    return color;
}

struct WaterfallCamera { float4x4 matrix; float4 dimensions; };
inline float4 waterfallPosition(float frequency, float age, float level, constant WaterfallCamera& camera) {
    return camera.matrix * float4((frequency - 0.5f) * camera.dimensions.x,
                                 level * camera.dimensions.y, -age * camera.dimensions.z, 1);
}
// Check every intervening STFT stamp once per time interval, rather than
// joining across a missing column merely because both endpoints survived.
kernel void asfwWaterfallContinuity(device const ulong* stamps [[buffer(0)]],
    device uint* valid [[buffer(1)]], constant SpectrogramDisplay& p [[buffer(2)]],
    constant WaterfallCamera& camera [[buffer(3)]], uint segment [[thread_position_in_grid]]) {
    uint count = uint(camera.dimensions.w);
    if (segment >= count - 1) return;
    uint first = segment * (p.columns - 1) / (count - 1);
    uint last = (segment + 1) * (p.columns - 1) / (count - 1);
    valid[segment] = 0;
    if (p.latestSlice < last) return;
    for (uint age = first; age <= last; ++age) {
        ulong slice = p.latestSlice - age;
        if (stamps[uint(slice % p.columns)] != slice + 1) return;
    }
    valid[segment] = 1;
}
struct WaterfallVertex { float4 position [[position]]; float db; };
inline float waterfallDB(float frequency, uint column, texture2d<float, access::read> history,
                         constant SpectrogramDisplay& p) {
    float ratio = min(20000.0f, float(p.sampleRate) / 2) / 20;
    float bin = clamp(20 * pow(ratio, frequency) * p.fftSize / p.sampleRate, 0.0f, float(p.fftSize / 2));
    uint lo = uint(bin), hi = min(lo + 1, p.fftSize / 2);
    float db = mix(history.read(uint2(column, lo)).x, history.read(uint2(column, hi)).x, fract(bin));
    uint first = uint(ceil(20 * pow(ratio, max(0.0f, frequency - 0.5f / 255)) * p.fftSize / p.sampleRate));
    uint last = min(uint(20 * pow(ratio, min(1.0f, frequency + 0.5f / 255)) * p.fftSize / p.sampleRate), p.fftSize / 2);
    for (uint b = first; b <= last; ++b) db = max(db, history.read(uint2(column, b)).x);
    return db;
}
inline WaterfallVertex waterfallSample(float frequency, uint row,
    texture2d<float, access::read> history, device const ulong* stamps,
    constant SpectrogramDisplay& p, constant WaterfallCamera& camera) {
    uint age = row * (p.columns - 1) / (uint(camera.dimensions.w) - 1);
    if (p.latestSlice < age) return { float4(0, 0, -1, 1), -100 };
    ulong slice = p.latestSlice - age;
    uint column = uint(slice % p.columns);
    if (stamps[column] != slice + 1) return { float4(0, 0, -1, 1), -100 };
    float db = waterfallDB(frequency, column, history, p);
    return { waterfallPosition(frequency, float(age) / (p.columns - 1),
                              clamp((db + 100) / 100, 0.0f, 1.0f), camera), db };
}
vertex WaterfallVertex asfwWaterfallSurfaceVertex(uint vid [[vertex_id]], uint segment [[instance_id]],
    texture2d<float, access::read> history [[texture(0)]], device const ulong* stamps [[buffer(0)]],
    constant SpectrogramDisplay& p [[buffer(1)]], constant WaterfallCamera& camera [[buffer(2)]],
    device const uint* valid [[buffer(3)]]) {
    if (!valid[segment]) return { float4(0, 0, -1, 1), -100 };
    // Alternating vertices from adjacent measured rows form the two triangles
    // of each frequency/time cell. No CPU interpolation or mesh uploads.
    return waterfallSample(float(vid / 2) / 255, segment + (vid & 1), history, stamps, p, camera);
}
vertex WaterfallVertex asfwWaterfallVertex(uint vid [[vertex_id]], uint row [[instance_id]],
    texture2d<float, access::read> history [[texture(0)]], device const ulong* stamps [[buffer(0)]],
    constant SpectrogramDisplay& p [[buffer(1)]], constant WaterfallCamera& camera [[buffer(2)]]) {
    return waterfallSample(float(vid) / 255, row, history, stamps, p, camera);
}
fragment float4 asfwWaterfallFragment(WaterfallVertex in [[stage_in]]) {
    // Color depends solely on interpolated measured dBFS, never on age or lighting.
    return float4(spectrogramColor(in.db), 1);
}
fragment float4 asfwWaterfallWireFragment() { return float4(0.015, 0.025, 0.035, 0.3); }


fragment float4 asfwSpectrogramFragment(SpectrogramVertex in [[stage_in]],
    texture2d<float, access::read> history [[texture(0)]], device const ulong* stamps [[buffer(0)]],
    constant SpectrogramDisplay& p [[buffer(1)]]) {
    uint age = min(uint((1 - clamp(in.uv.x, 0.0f, 1.0f)) * p.columns), p.columns - 1);
    if (p.latestSlice < age) return float4(0.025, 0.035, 0.05, 1);
    ulong slice = p.latestSlice - age;
    uint column = uint(slice % p.columns);
    if (stamps[column] != slice + 1) return float4(0.025, 0.035, 0.05, 1);
    float hz = 20 * pow(min(20000.0f, float(p.sampleRate) / 2) / 20, 1 - in.uv.y);
    float bin = clamp(hz * p.fftSize / p.sampleRate, 0.0f, float(p.fftSize / 2));
    uint lo = uint(bin), hi = min(lo + 1, p.fftSize / 2);
    float db = mix(history.read(uint2(column, lo)).x, history.read(uint2(column, hi)).x, fract(bin));
    // Keep narrow tones visible when a logarithmic row covers several FFT bins.
    float halfRow = 0.5f / max(1.0f, float(p.pixelHeight));
    float ratio = min(20000.0f, float(p.sampleRate) / 2) / 20;
    uint first = uint(ceil(20 * pow(ratio, max(0.0f, 1 - in.uv.y - halfRow)) * p.fftSize / p.sampleRate));
    uint last = min(uint(20 * pow(ratio, min(1.0f, 1 - in.uv.y + halfRow)) * p.fftSize / p.sampleRate), p.fftSize / 2);
    for (uint b = first; b <= last; ++b) db = max(db, history.read(uint2(column, b)).x);
    return float4(spectrogramColor(db), 1);
}

vertex SpectrumVertex asfwSpectrumVertex(uint vid [[vertex_id]],
    device const float* amplitudes [[buffer(0)]],
    constant SpectrumParams& p [[buffer(1)]]) {
    uint fftSize = p.fftSize;
    float maximumHz = min(20000.0f, float(p.sampleRate) * 0.5f);
    float ratio = maximumHz / 20.0f;
    float hz = 20.0f * pow(ratio, float(vid) / 511.0f);
    float nextHz = 20.0f * pow(ratio, float(min(vid + 1, 511u)) / 511.0f);
    float binPosition = hz * fftSize / p.sampleRate;
    uint lo = min(uint(binPosition), fftSize / 2);
    uint hi = min(lo + 1, fftSize / 2);
    float amplitude = mix(amplitudes[lo], amplitudes[hi], fract(binPosition));
    // Peak aggregation retains narrow peaks when logarithmic pixels span bins.
    for (uint bin = uint(ceil(binPosition)); bin <= min(uint(nextHz * fftSize / p.sampleRate), fftSize / 2); ++bin)
        amplitude = max(amplitude, amplitudes[bin]);
    float db = 20.0f * log10(max(amplitude, 1.0e-6f));
    float y = 2.0f * (clamp(db, -120.0f, 6.0f) + 120.0f) / 126.0f - 1.0f;
    return { float4(-1.0f + 2.0f * float(vid) / 511.0f, y, 0, 1) };
}

struct SmoothingParams { float alpha; float elapsed; uint reset; uint binCount; };
// Average linear power. Peak holds for two seconds, then decays 12 dB/second.
kernel void asfwSpectrumSmooth(device const float* raw [[buffer(0)]],
    device float4* history [[buffer(1)]], device float* average [[buffer(2)]],
    device float* peaks [[buffer(3)]], constant SmoothingParams& p [[buffer(4)]],
    uint bin [[thread_position_in_grid]]) {
    if (bin >= p.binCount) return;
    float a = raw[bin];
    float4 h = p.reset ? float4(a*a, a, 0, 0) : history[bin];
    h.x = mix(a*a, h.x, p.alpha);
    h.z += p.elapsed;
    if (h.z > 2) h.y *= pow(10.0f, -12.0f * p.elapsed / 20.0f);
    if (a >= h.y) { h.y = a; h.z = 0; }
    history[bin] = h;
    average[bin] = sqrt(max(0.0f, h.x));
    peaks[bin] = h.y;
}
fragment float4 asfwSpectrumPeakFragment() { return float4(0.75, 0.55, 0.22, 1); }
