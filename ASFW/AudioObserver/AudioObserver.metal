#include <metal_stdlib>
using namespace metal;

struct ObserverParams {
    ulong writeEndFrame;
    uint ringFrames;
    uint channels;
    uint windowFrames;
    uint channel;
    uint rightChannel;
};

struct ObserverVertex {
    float4 position [[position]];
};

static inline float sampleAt(device const float* samples,
                             constant ObserverParams& params,
                             uint vertexID,
                             uint channel) {
    const ulong firstFrame = params.writeEndFrame - params.windowFrames;
    const uint frame = uint((firstFrame + vertexID) % params.ringFrames);
    return samples[ulong(frame) * params.channels + channel];
}

vertex ObserverVertex asfwPhaseVertex(
    uint vertexID [[vertex_id]],
    device const float* samples [[buffer(0)]],
    constant ObserverParams& params [[buffer(1)]]) {
    const float left = sampleAt(samples, params, vertexID, params.channel);
    const float right = sampleAt(samples, params, vertexID, params.rightChannel);
    const float x = (left - right) * 0.70710678118f;
    const float y = (left + right) * 0.70710678118f;
    return { float4(clamp(x, -1.0f, 1.0f), clamp(y, -1.0f, 1.0f), 0.0f, 1.0f) };
}

vertex ObserverVertex asfwWaveformVertex(
    uint vertexID [[vertex_id]],
    device const float* samples [[buffer(0)]],
    constant ObserverParams& params [[buffer(1)]]) {
    const float value = sampleAt(samples, params, vertexID, params.channel);
    const float x = params.windowFrames > 1
        ? -1.0f + 2.0f * float(vertexID) / float(params.windowFrames - 1)
        : 0.0f;
    return { float4(x, clamp(value, -1.0f, 1.0f), 0.0f, 1.0f) };
}

fragment float4 asfwAudioFragment() {
    return float4(0.20f, 0.91f, 0.73f, 1.0f);
}

kernel void asfwAnalyzeRing(
    device const float* samples [[buffer(0)]],
    device uint* output [[buffer(1)]],
    constant ObserverParams& params [[buffer(2)]],
    uint tid [[thread_position_in_grid]]) {
    if (tid != 0) return;

    const uint count = min(params.windowFrames, params.ringFrames);
    float peakLeft = 0.0f;
    float peakRight = 0.0f;
    float sumLeft = 0.0f;
    float sumRight = 0.0f;
    float sumLeftRight = 0.0f;
    float sumLeftSquared = 0.0f;
    float sumRightSquared = 0.0f;

    for (uint i = 0; i < count; ++i) {
        const float left = sampleAt(samples, params, i, params.channel);
        const float right = sampleAt(samples, params, i, params.rightChannel);
        peakLeft = max(peakLeft, abs(left));
        peakRight = max(peakRight, abs(right));
        sumLeft += left;
        sumRight += right;
        sumLeftRight += left * right;
        sumLeftSquared += left * left;
        sumRightSquared += right * right;
    }

    float correlation = 0.0f;
    if (count > 1) {
        const float n = float(count);
        const float covariance = sumLeftRight - sumLeft * sumRight / n;
        const float varianceLeft = max(0.0f, sumLeftSquared - sumLeft * sumLeft / n);
        const float varianceRight = max(0.0f, sumRightSquared - sumRight * sumRight / n);
        const float denominator = sqrt(varianceLeft * varianceRight);
        if (denominator > 1.0e-12f) correlation = clamp(covariance / denominator, -1.0f, 1.0f);
    }

    output[0] = as_type<uint>(peakLeft);
    output[1] = as_type<uint>(peakRight);
    output[2] = as_type<uint>(correlation);
    output[3] = count;
}
