#include <metal_stdlib>
using namespace metal;
constant uint fftSize = 2048;
struct SpectrumParams { ulong writeEnd; uint ringFrames; uint channels; uint channel; uint sampleRate; uint otherChannel; uint transform; };
struct SpectrumVertex { float4 position [[position]]; };

// One threadgroup performs a radix-2 DIT FFT. Bit-reversed input followed by
// eleven butterfly stages produces natural-order frequency bins.
kernel void asfwSpectrumFFT(device const float* ring [[buffer(0)]],
                            device float* amplitudes [[buffer(1)]],
                            constant SpectrumParams& p [[buffer(2)]],
                            uint tid [[thread_index_in_threadgroup]],
                            uint3 groupSize [[threads_per_threadgroup]]) {
    const uint threads = groupSize.x;
    threadgroup float2 values[2048];
    for (uint i = tid; i < fftSize; i += threads) {
        uint reversed = 0;
        uint bits = i;
        for (uint j = 0; j < 11; ++j) { reversed = (reversed << 1) | (bits & 1); bits >>= 1; }
        uint frame = uint((p.writeEnd - fftSize + i) % p.ringFrames);
        // Periodic Hann has coherent gain exactly 1/2.
        float window = 0.5f - 0.5f * cos(2.0f * M_PI_F * float(i) / float(fftSize));
        float sample = ring[ulong(frame) * p.channels + p.channel];
        float other = ring[ulong(frame) * p.channels + p.otherChannel];
        if (p.transform == 1) sample = (sample + other) * 0.70710678118f;
        if (p.transform == 2) sample = (sample - other) * 0.70710678118f;
        values[reversed] = float2(sample * window, 0);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint width = 2; width <= fftSize; width <<= 1) {
        uint halfWidth = width >> 1;
        for (uint butterfly = tid; butterfly < fftSize / 2; butterfly += threads) {
            uint j = butterfly % halfWidth;
            uint a = (butterfly / halfWidth) * width + j;
            uint b = a + halfWidth;
            float angle = -2.0f * M_PI_F * float(j) / float(width);
            float2 v = values[b];
            float2 product = float2(v.x * cos(angle) - v.y * sin(angle),
                                    v.x * sin(angle) + v.y * cos(angle));
            float2 u = values[a];
            values[a] = u + product;
            values[b] = u - product;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    for (uint bin = tid; bin <= fftSize / 2; bin += threads) {
        // Single-sided peak amplitude: a bin-centred full-scale sine is 0 dBFS.
        float scale = (bin == 0 || bin == fftSize / 2) ? 2.0f / fftSize : 4.0f / fftSize;
        amplitudes[bin] = length(values[bin]) * scale;
    }
}

vertex SpectrumVertex asfwSpectrumVertex(uint vid [[vertex_id]],
    device const float* amplitudes [[buffer(0)]],
    constant SpectrumParams& p [[buffer(1)]]) {
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

struct SmoothingParams { float alpha; float elapsed; uint reset; uint unused; };
// Average linear power. Peak holds for two seconds, then decays 12 dB/second.
kernel void asfwSpectrumSmooth(device const float* raw [[buffer(0)]],
    device float4* history [[buffer(1)]], device float* average [[buffer(2)]],
    device float* peaks [[buffer(3)]], constant SmoothingParams& p [[buffer(4)]],
    uint bin [[thread_position_in_grid]]) {
    if (bin > 1024) return;
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
