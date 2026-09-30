#include <metal_stdlib>
using namespace metal;

struct ScopeParams {
    ulong writeEndFrame;
    uint ringFrames;
    uint channels;
    uint windowFrames;
    uint validateSamples;
};

struct ScopeVertexOut {
    float4 position [[position]];
};

kernel void analyzeRing(
    device const float* samples [[buffer(0)]],
    device uint* output [[buffer(1)]],
    constant ScopeParams& params [[buffer(2)]],
    uint threadID [[thread_position_in_grid]])
{
    if (threadID != 0) return;

    float leftPeak = 0.0f;
    float rightPeak = 0.0f;
    float sumLR = 0.0f;
    float sumLL = 0.0f;
    float sumRR = 0.0f;
    float sumL = 0.0f;
    float sumR = 0.0f;
    const uint count = min(params.windowFrames, params.ringFrames);
    const ulong firstFrame = params.writeEndFrame - count;

    for (uint i = 0; i < count; ++i) {
        const uint frame = uint((firstFrame + i) % params.ringFrames);
        const uint base = frame * params.channels;
        const float left = samples[base];
        const float right = samples[base + 1];
        leftPeak = max(leftPeak, abs(left));
        rightPeak = max(rightPeak, abs(right));
        sumLR += left * right;
        sumLL += left * left;
        sumRR += right * right;
        sumL += left;
        sumR += right;
    }

    const float meanCorrection = count > 0 ? 1.0f / float(count) : 0.0f;
    const float centeredLR = sumLR - sumL * sumR * meanCorrection;
    const float centeredLL = max(0.0f, sumLL - sumL * sumL * meanCorrection);
    const float centeredRR = max(0.0f, sumRR - sumR * sumR * meanCorrection);
    const float denominator = sqrt(centeredLL * centeredRR);
    const float correlation = count == 0 || denominator <= 1.0e-20f
        ? 0.0f
        : clamp(centeredLR / denominator, -1.0f, 1.0f);
    output[0] = as_type<uint>(leftPeak);
    output[1] = as_type<uint>(rightPeak);
    output[2] = as_type<uint>(correlation);
    output[3] = count;

    if (params.validateSamples != 0 && count > 1) {
        for (uint sampleNumber = 0; sampleNumber < 16; ++sampleNumber) {
            const ulong delta = ulong(sampleNumber) * ulong(count - 1) / 15;
            const uint frame = uint((firstFrame + delta) % params.ringFrames);
            const uint base = frame * params.channels;
            output[4 + sampleNumber * 2] = as_type<uint>(samples[base]);
            output[5 + sampleNumber * 2] = as_type<uint>(samples[base + 1]);
        }
    }
}

vertex ScopeVertexOut phaseScopeVertex(
    uint vertexID [[vertex_id]],
    device const float* samples [[buffer(0)]],
    constant ScopeParams& params [[buffer(1)]])
{
    float x = 0.0f;
    float y = 0.0f;
    if (params.ringFrames != 0 && params.channels >= 2 &&
        vertexID < params.windowFrames) {
        const uint count = min(params.windowFrames, params.ringFrames);
        const ulong firstFrame = params.writeEndFrame - count;
        const uint frame = uint((firstFrame + vertexID) % params.ringFrames);
        const uint base = frame * params.channels;
        const float left = samples[base];
        const float right = samples[base + 1];
        constexpr float kInvSqrt2 = 0.70710678118f;
        x = (left - right) * kInvSqrt2;
        y = (left + right) * kInvSqrt2;
    }
    return { .position = float4(clamp(x, -1.0f, 1.0f),
                                clamp(y, -1.0f, 1.0f), 0.0f, 1.0f) };
}

fragment float4 phaseScopeFragment()
{
    return float4(0.20f, 0.92f, 0.62f, 1.0f);
}
