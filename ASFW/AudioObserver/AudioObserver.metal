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

struct ConsumeRangeParams {
    ulong startFrame;
    uint frameCount;
    uint ringFrames;
    uint channels;
    uint leftChannel;
    uint rightChannel;
};

struct ConsumeRangePartial {
    float peakLeft;
    float peakRight;
    float peakMid;
    float peakSide;
    float energyLeft;
    float energyRight;
    float crossLeftRight;
    float energyMid;
    float energySide;
    uint overRangeLeft;
    uint overRangeRight;
    uint invalidLeft;
    uint invalidRight;
};

// Stateful 48 kHz BS.1770 K-weighting. State words contain two direct-form
// biquads per selected channel plus unfinished 10 ms channel-energy sums.
kernel void asfwKWeightRange(
    device const float* samples [[buffer(0)]],
    device uint* state [[buffer(1)]],
    device uint* output [[buffer(2)]],
    constant ConsumeRangeParams& params [[buffer(3)]],
    uint tid [[thread_position_in_grid]]) {
    if (tid != 0) return;

    float l1x1 = as_type<float>(state[0]);
    float l1x2 = as_type<float>(state[1]);
    float l1y1 = as_type<float>(state[2]);
    float l1y2 = as_type<float>(state[3]);
    float r1x1 = as_type<float>(state[4]);
    float r1x2 = as_type<float>(state[5]);
    float r1y1 = as_type<float>(state[6]);
    float r1y2 = as_type<float>(state[7]);
    float l2x1 = as_type<float>(state[8]);
    float l2x2 = as_type<float>(state[9]);
    float l2y1 = as_type<float>(state[10]);
    float l2y2 = as_type<float>(state[11]);
    float r2x1 = as_type<float>(state[12]);
    float r2x2 = as_type<float>(state[13]);
    float r2y1 = as_type<float>(state[14]);
    float r2y2 = as_type<float>(state[15]);
    float energyLeft = as_type<float>(state[16]);
    float energyRight = as_type<float>(state[17]);
    uint partialFrames = state[18];
    uint chunkCount = 0;

    for (uint i = 0; i < params.frameCount; ++i) {
        const uint frame = uint((params.startFrame + ulong(i)) % params.ringFrames);
        const ulong base = ulong(frame) * params.channels;
        const float rawLeft = samples[base + params.leftChannel];
        const float rawRight = samples[base + params.rightChannel];
        const float left = isfinite(rawLeft) ? rawLeft : 0.0f;
        const float right = isfinite(rawRight) ? rawRight : 0.0f;

        const float leftStage1 = 1.5351248596f * left - 2.6916961894f * l1x1
            + 1.1983928109f * l1x2 + 1.6906592932f * l1y1 - 0.7324807742f * l1y2;
        l1x2 = l1x1; l1x1 = left; l1y2 = l1y1; l1y1 = leftStage1;
        const float leftFiltered = leftStage1 - 2.0f * l2x1 + l2x2
            + 1.9900474548f * l2y1 - 0.9900722504f * l2y2;
        l2x2 = l2x1; l2x1 = leftStage1; l2y2 = l2y1; l2y1 = leftFiltered;

        const float rightStage1 = 1.5351248596f * right - 2.6916961894f * r1x1
            + 1.1983928109f * r1x2 + 1.6906592932f * r1y1 - 0.7324807742f * r1y2;
        r1x2 = r1x1; r1x1 = right; r1y2 = r1y1; r1y1 = rightStage1;
        const float rightFiltered = rightStage1 - 2.0f * r2x1 + r2x2
            + 1.9900474548f * r2y1 - 0.9900722504f * r2y2;
        r2x2 = r2x1; r2x1 = rightStage1; r2y2 = r2y1; r2y1 = rightFiltered;

        energyLeft += leftFiltered * leftFiltered;
        energyRight += rightFiltered * rightFiltered;
        ++partialFrames;
        if (partialFrames == 480) {
            const ulong endFrame = params.startFrame + ulong(i) + 1;
            const uint base = 17 + chunkCount * 4;
            output[base] = uint(endFrame & 0xfffffffful);
            output[base + 1] = uint(endFrame >> 32);
            output[base + 2] = as_type<uint>(energyLeft);
            output[base + 3] = as_type<uint>(energyRight);
            ++chunkCount;
            energyLeft = 0.0f;
            energyRight = 0.0f;
            partialFrames = 0;
        }
    }

    state[0] = as_type<uint>(l1x1); state[1] = as_type<uint>(l1x2);
    state[2] = as_type<uint>(l1y1); state[3] = as_type<uint>(l1y2);
    state[4] = as_type<uint>(r1x1); state[5] = as_type<uint>(r1x2);
    state[6] = as_type<uint>(r1y1); state[7] = as_type<uint>(r1y2);
    state[8] = as_type<uint>(l2x1); state[9] = as_type<uint>(l2x2);
    state[10] = as_type<uint>(l2y1); state[11] = as_type<uint>(l2y2);
    state[12] = as_type<uint>(r2x1); state[13] = as_type<uint>(r2x2);
    state[14] = as_type<uint>(r2y1); state[15] = as_type<uint>(r2y2);
    state[16] = as_type<uint>(energyLeft);
    state[17] = as_type<uint>(energyRight);
    state[18] = partialFrames;
    output[16] = chunkCount;
}

// Consume a bounded, previously unpublished frame range. The one-group tree
// reduction keeps all PCM loads on the GPU and writes only scalar summaries.
kernel void asfwConsumeOutputRange(
    device const float* samples [[buffer(0)]],
    device uint* output [[buffer(1)]],
    constant ConsumeRangeParams& params [[buffer(2)]],
    uint tid [[thread_index_in_threadgroup]],
    uint threadCount [[threads_per_threadgroup]]) {
    threadgroup ConsumeRangePartial partial[256];
    ConsumeRangePartial value = {};
    for (uint i = tid; i < params.frameCount; i += threadCount) {
        const uint frame = uint((params.startFrame + ulong(i)) % params.ringFrames);
        const ulong offset = ulong(frame) * params.channels;
        const float rawLeft = samples[offset + params.leftChannel];
        const float rawRight = samples[offset + params.rightChannel];
        const bool invalidLeft = !isfinite(rawLeft);
        const bool invalidRight = !isfinite(rawRight);
        const float left = invalidLeft ? 0.0f : rawLeft;
        const float right = invalidRight ? 0.0f : rawRight;
        const float mid = (left + right) * 0.70710678118f;
        const float side = (left - right) * 0.70710678118f;

        value.peakLeft = max(value.peakLeft, abs(left));
        value.peakRight = max(value.peakRight, abs(right));
        value.peakMid = max(value.peakMid, abs(mid));
        value.peakSide = max(value.peakSide, abs(side));
        value.energyLeft += left * left;
        value.energyRight += right * right;
        value.crossLeftRight += left * right;
        value.energyMid += mid * mid;
        value.energySide += side * side;
        value.overRangeLeft += uint(abs(left) >= 1.0f);
        value.overRangeRight += uint(abs(right) >= 1.0f);
        value.invalidLeft += uint(invalidLeft);
        value.invalidRight += uint(invalidRight);
    }
    partial[tid] = value;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    for (uint stride = threadCount >> 1; stride > 0; stride >>= 1) {
        if (tid < stride) {
            ConsumeRangePartial other = partial[tid + stride];
            partial[tid].peakLeft = max(partial[tid].peakLeft, other.peakLeft);
            partial[tid].peakRight = max(partial[tid].peakRight, other.peakRight);
            partial[tid].peakMid = max(partial[tid].peakMid, other.peakMid);
            partial[tid].peakSide = max(partial[tid].peakSide, other.peakSide);
            partial[tid].energyLeft += other.energyLeft;
            partial[tid].energyRight += other.energyRight;
            partial[tid].crossLeftRight += other.crossLeftRight;
            partial[tid].energyMid += other.energyMid;
            partial[tid].energySide += other.energySide;
            partial[tid].overRangeLeft += other.overRangeLeft;
            partial[tid].overRangeRight += other.overRangeRight;
            partial[tid].invalidLeft += other.invalidLeft;
            partial[tid].invalidRight += other.invalidRight;
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    if (tid == 0) {
        ConsumeRangePartial result = partial[0];
        const float count = float(params.frameCount);
        const float pairEnergy = result.energyLeft + result.energyRight;
        const float denominator = sqrt(result.energyLeft * result.energyRight);
        const bool correlationValid = params.frameCount > 0 && denominator > 1.0e-12f;
        const float correlation = correlationValid
            ? clamp(result.crossLeftRight / denominator, -1.0f, 1.0f) : 0.0f;
        const float balance = pairEnergy > 1.0e-12f
            ? (result.energyRight - result.energyLeft) / pairEnergy : 0.0f;
        const float sideFraction = pairEnergy > 1.0e-12f
            ? result.energySide / pairEnergy : 0.0f;

        output[0] = as_type<uint>(result.peakLeft);
        output[1] = as_type<uint>(result.peakRight);
        output[2] = as_type<uint>(correlation);
        output[3] = params.frameCount;
        output[4] = as_type<uint>(result.peakMid);
        output[5] = as_type<uint>(result.peakSide);
        output[6] = as_type<uint>(sqrt(result.energyLeft / count));
        output[7] = as_type<uint>(sqrt(result.energyRight / count));
        output[8] = as_type<uint>(sqrt(result.energyMid / count));
        output[9] = as_type<uint>(sqrt(result.energySide / count));
        output[10] = as_type<uint>(balance);
        output[11] = as_type<uint>(sideFraction);
        output[12] = uint(correlationValid);
        output[13] = result.overRangeLeft;
        output[14] = result.overRangeRight;
        output[15] = as_type<uint>(pairEnergy > 1.0e-12f
            ? 10.0f * log10(max(result.energyMid / pairEnergy, 1.0e-12f))
            : 0.0f);
        output[90] = result.invalidLeft;
        output[91] = result.invalidRight;
    }
}
