#include <metal_stdlib>
using namespace metal;

struct WaveformParams {
    ulong writeEndFrame;
    uint ringFrames;
    uint channels;
    uint windowFrames;
    uint channel;
};

struct WaveformVertexOut {
    float4 position [[position]];
};

vertex WaveformVertexOut waveformVertex(
    uint vertexID [[vertex_id]],
    device const float* samples [[buffer(0)]],
    constant WaveformParams& params [[buffer(1)]])
{
    float y = 0.0f;
    if (params.ringFrames != 0 && params.channels != 0 &&
        params.windowFrames > 1 && vertexID < params.windowFrames) {
        const ulong firstFrame = params.writeEndFrame >= params.windowFrames
            ? params.writeEndFrame - params.windowFrames
            : 0;
        const ulong absoluteFrame = firstFrame + vertexID;
        const uint frame = uint(absoluteFrame % params.ringFrames);
        y = clamp(samples[frame * params.channels + params.channel], -1.0f, 1.0f);
    }

    const float x = -1.0f +
        2.0f * float(vertexID) / float(params.windowFrames - 1);
    return { .position = float4(x, y, 0.0f, 1.0f) };
}

fragment float4 waveformFragment()
{
    return float4(0.20f, 0.92f, 0.62f, 1.0f);
}
