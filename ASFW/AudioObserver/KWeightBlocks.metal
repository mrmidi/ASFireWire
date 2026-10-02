#include <metal_stdlib>
using namespace metal;

// Original Metal implementation inspired by Zhai & Paris (2026),
// arXiv:2607.23763v1, Sec. II-F, Eqs. (24)-(25): zero-state response,
// inter-group state recurrence, then homogeneous correction.
// https://arxiv.org/abs/2607.23763v1
// Our local solver is a 32-frame recurrence, not the paper's PH/CR solver;
// our scan is bounded to one threadgroup, not CUDA decoupled lookback.
struct KWeightRange {
    ulong startFrame; uint frameCount; uint ringFrames; uint channels;
    uint leftChannel; uint rightChannel;
};
constant uint kWeightBlockFrames = 32;
constant uint kWeightPowerCount = 5761;

static float2 kWeightInput(device const float* input, device const uint* state,
                          constant KWeightRange& p, uint stage, int i) {
    const uint offset = stage * 8;
    if (i < 0) {
        const uint lag = uint(-i - 1);
        return float2(as_type<float>(state[offset + lag]), as_type<float>(state[offset + 4 + lag]));
    }
    if (stage == 1) return ((device const float2*)input)[i];
    const uint frame = uint((p.startFrame + ulong(i)) % p.ringFrames);
    const ulong base = ulong(frame) * p.channels;
    const float2 value(input[base + p.leftChannel], input[base + p.rightChannel]);
    return select(float2(0), value, isfinite(value));
}
static float4 kWeightApply(float4 m, float4 s) {
    return float4(fma(m.x, s.x, m.y * s.y), fma(m.z, s.x, m.w * s.y),
                  fma(m.x, s.z, m.y * s.w), fma(m.z, s.z, m.w * s.w));
}

kernel void asfwKWeightBlockLocal(
    device const float* input [[buffer(0)]], device const uint* state [[buffer(1)]],
    device float2* result [[buffer(2)]], device float4* carries [[buffer(3)]],
    constant KWeightRange& p [[buffer(6)]], constant uint& stage [[buffer(7)]],
    uint block [[thread_position_in_grid]]) {
    const uint start = block * kWeightBlockFrames;
    if (start >= p.frameCount) return;
    const uint end = min(p.frameCount, start + kWeightBlockFrames);
    const float3 b = stage == 0 ? float3(1.5351248596f, -2.6916961894f, 1.1983928109f) : float3(1, -2, 1);
    const float2 a = stage == 0 ? float2(1.6906592932f, -0.7324807742f) : float2(1.9900474548f, -0.9900722504f);
    float2 x1 = kWeightInput(input, state, p, stage, int(start) - 1);
    float2 x2 = kWeightInput(input, state, p, stage, int(start) - 2);
    float2 y1 = 0, delta = 0;
    for (uint i = start; i < end; ++i) {
        const float2 x = kWeightInput(input, state, p, stage, int(i));
        const float dcGain = (b.x + b.y) + b.z;
        const float2 feed = fma(b.x, x - x1, fma(-b.z, x1 - x2, dcGain * x1));
        // Algebraically identical recurrence in (y, delta-y) coordinates.
        delta = fma((a.x - 1.0f) + a.y, y1, fma(-a.y, delta, feed));
        const float2 y = y1 + delta;
        result[i] = y;
        x2 = x1; x1 = x; y1 = y;
    }
    carries[block] = float4(y1.x, delta.x, y1.y, delta.y);
}

kernel void asfwKWeightBlockScan(
    device const uint* state [[buffer(1)]], device const float4* carries [[buffer(3)]],
    device float4* initial [[buffer(4)]], device const float4* powers [[buffer(5)]],
    constant KWeightRange& p [[buffer(6)]], constant uint& stage [[buffer(7)]],
    uint tid [[thread_index_in_threadgroup]]) {
    threadgroup float4 sums[256];
    const uint blocks = (p.frameCount + kWeightBlockFrames - 1) / kWeightBlockFrames;
    const uint powerOffset = stage * kWeightPowerCount;
    sums[tid] = tid < blocks ? carries[tid] : float4(0);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    // Inclusive scan of affine state transfers; every predecessor block is full.
    // The final, possibly short block's carry is never used as a predecessor.
    for (uint distance = 1; distance < blocks; distance *= 2) {
        float4 value = sums[tid];
        if (tid < blocks && tid >= distance)
            value += kWeightApply(powers[powerOffset + distance * kWeightBlockFrames], sums[tid - distance]);
        threadgroup_barrier(mem_flags::mem_threadgroup);
        sums[tid] = value;
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (tid < blocks) {
        const uint offset = stage * 8;
        const float yl = as_type<float>(state[offset + 2]), yr = as_type<float>(state[offset + 6]);
        const float4 previous(yl, yl - as_type<float>(state[offset + 3]),
                              yr, yr - as_type<float>(state[offset + 7]));
        initial[tid] = kWeightApply(powers[powerOffset + tid * kWeightBlockFrames], previous)
                     + (tid > 0 ? sums[tid - 1] : float4(0));
    }
}

kernel void asfwKWeightBlockCorrect(
    device float2* result [[buffer(2)]], device const float4* initial [[buffer(4)]],
    device const float4* powers [[buffer(5)]], constant KWeightRange& p [[buffer(6)]],
    constant uint& stage [[buffer(7)]], uint i [[thread_position_in_grid]]) {
    if (i >= p.frameCount) return;
    const float4 correction = kWeightApply(powers[stage * kWeightPowerCount + i % kWeightBlockFrames + 1],
                                          initial[i / kWeightBlockFrames]);
    result[i] += float2(correction.x, correction.z);
}

// One threadgroup per 10-ms chunk, including the final unfinished chunk.
// At most two samples per lane; no whole-packet scalar walk remains.
kernel void asfwKWeightChunkReduce(
    device const float* samples [[buffer(0)]], device const uint* state [[buffer(1)]],
    device const float2* filtered [[buffer(2)]], device const float2* peaks [[buffer(3)]],
    device float4* chunks [[buffer(4)]], constant KWeightRange& p [[buffer(5)]],
    uint group [[threadgroup_position_in_grid]], uint tid [[thread_index_in_threadgroup]]) {
    threadgroup float4 energies[256];
    threadgroup float2 maxima[256];
    const uint partial = state[18];
    const uint start = uint(max(0, int(group * 480) - int(partial)));
    const uint end = min(p.frameCount, uint(max(0, int((group + 1) * 480) - int(partial))));
    float4 energy = 0;
    float2 peak = 0;
    for (uint i = start + tid; i < end; i += 256) {
        const float2 raw = kWeightInput(samples, state, p, 0, int(i));
        const float2 weighted = filtered[i];
        energy.xy += weighted * weighted;
        energy.z += dot(raw, raw);
        energy.w = max(energy.w, max(abs(raw.x), abs(raw.y)));
        peak = max(peak, peaks[i]);
    }
    energies[tid] = energy; maxima[tid] = peak;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (tid < stride) {
            energies[tid].xyz += energies[tid + stride].xyz;
            energies[tid].w = max(energies[tid].w, energies[tid + stride].w);
            maxima[tid] = max(maxima[tid], maxima[tid + stride]);
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (tid == 0) {
        chunks[group * 2] = energies[0];
        chunks[group * 2 + 1] = float4(maxima[0], 0, 0);
    }
}
