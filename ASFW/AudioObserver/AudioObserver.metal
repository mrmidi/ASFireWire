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
    // Standard goniometer / vectorscope orientation:
    // Y: Mid = (L + R) / √2 (in-phase mono points up)
    // X: Side = (R - L) / √2 (Left deflects to -X / top-left, Right deflects to +X / top-right)
    const float x = (right - left) * 0.70710678118f;
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

fragment float4 asfwWaveformFragment(constant float4& color [[buffer(0)]]) {
    return color;
}

// Keep in sync with AudioAnalysisLayout; regression tests exercise both kernels
// together and check the chunk region plus an output-buffer canary.
constant uint analysisChunkOffset = 96;
constant uint analysisChunkCapacity = 12;
constant uint analysisMaximumBatchFrames = 480 * analysisChunkCapacity;

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

// ITU-R BS.1770-5 Annex 2, order-48, four-phase FIR coefficients. The
// standard's matrix is transposed here so each phase is contiguous.
constant float asfwTruePeakCoefficients[4][12] = {
    { 0.0017089843750f, 0.0109863281250f, -0.0196533203125f, 0.0332031250000f,
      -0.0594482421875f, 0.1373291015625f, 0.9721679687500f, -0.1022949218750f,
      0.0476074218750f, -0.0266113281250f, 0.0148925781250f, -0.0083007812500f },
    { -0.0291748046875f, 0.0292968750000f, -0.0517578125000f, 0.0891113281250f,
      -0.1665039062500f, 0.4650878906250f, 0.7797851562500f, -0.2003173828125f,
      0.1015625000000f, -0.0582275390625f, 0.0330810546875f, -0.0189208984375f },
    { -0.0189208984375f, 0.0330810546875f, -0.0582275390625f, 0.1015625000000f,
      -0.2003173828125f, 0.7797851562500f, 0.4650878906250f, -0.1665039062500f,
      0.0891113281250f, -0.0517578125f, 0.0292968750f, -0.0291748046875f },
    { -0.0083007812500f, 0.0148925781250f, -0.0266113281250f, 0.0476074218750f,
      -0.1022949218750f, 0.9721679687500f, 0.1373291015625f, -0.0594482421875f,
      0.0332031250000f, -0.0196533203125f, 0.0109863281250f, 0.0017089843750f }
};

// Independent FIR evaluations: prior-batch samples come from committed history,
// never from ring frames that the writer may already have overwritten.
kernel void asfwTruePeakRange(
    device const float* samples [[buffer(0)]],
    device const uint* state [[buffer(1)]],
    device float2* peaks [[buffer(2)]],
    constant ConsumeRangeParams& params [[buffer(3)]],
    uint i [[thread_position_in_grid]]) {
    if (i >= params.frameCount || params.frameCount > analysisMaximumBatchFrames) return;
    float2 peak = 0;
    if (state[44] + i + 1 >= 12) {
        float2 interpolated[4] = {};
        for (uint tap = 0; tap < 12; ++tap) {
            float2 value;
            if (tap > i) {
                const uint history = tap - i - 1;
                value = float2(as_type<float>(state[20 + history]), as_type<float>(state[32 + history]));
            } else {
                const uint frame = uint((params.startFrame + ulong(i - tap)) % params.ringFrames);
                const ulong base = ulong(frame) * params.channels;
                value = float2(samples[base + params.leftChannel], samples[base + params.rightChannel]);
                value = select(float2(0), value, isfinite(value));
            }
            for (uint phase = 0; phase < 4; ++phase)
                interpolated[phase] += asfwTruePeakCoefficients[phase][tap] * value;
        }
        for (uint phase = 0; phase < 4; ++phase)
            peak = max(peak, select(float2(0), abs(interpolated[phase]), isfinite(interpolated[phase])));
    }
    peaks[i] = peak;
}

// Summarise the parallel K-weighted samples, keep unfinished 10 ms sums,
// and commit the cascade/FIR terminal histories after all filtering passes.
kernel void asfwKWeightRange(
    device const float* samples [[buffer(0)]],
    device uint* state [[buffer(1)]],
    device uint* output [[buffer(2)]],
    constant ConsumeRangeParams& params [[buffer(3)]],
    device const float2* peaks [[buffer(4)]],
    device const float2* filtered [[buffer(5)]],
    device const float2* shelf [[buffer(6)]],
    device const float4* chunks [[buffer(7)]],
    uint tid [[thread_position_in_grid]]) {
    if (tid != 0) return;
    if (params.frameCount > analysisMaximumBatchFrames) { output[16] = 0; output[94] = 0; return; }

    float energyLeft = as_type<float>(state[16]);
    float energyRight = as_type<float>(state[17]);
    uint partialFrames = state[18];
    float rawEnergy = as_type<float>(state[45]);
    float samplePeak = as_type<float>(state[46]);
    float chunkTruePeakLeft = as_type<float>(state[47]);
    float chunkTruePeakRight = as_type<float>(state[48]);
    float truePeakHistoryLeft[12];
    float truePeakHistoryRight[12];
    for (uint tap = 0; tap < 12; ++tap) {
        truePeakHistoryLeft[tap] = as_type<float>(state[20 + tap]);
        truePeakHistoryRight[tap] = as_type<float>(state[32 + tap]);
    }
    uint truePeakHistoryFrames = state[44];
    float truePeakLeft = 0.0f;
    float truePeakRight = 0.0f;
    bool truePeakValid = false;
    uint chunkCount = 0;

    const uint oldPartial = partialFrames;
    chunkCount = (oldPartial + params.frameCount) / 480;
    partialFrames = (oldPartial + params.frameCount) % 480;
    truePeakValid = truePeakHistoryFrames + params.frameCount >= 12;
    // Only compact group results are visited (at most 13), never PCM frames.
    for (uint chunk = 0; chunk <= chunkCount; ++chunk) {
        float4 value = chunks[chunk * 2];
        float2 peak = chunks[chunk * 2 + 1].xy;
        truePeakLeft = max(truePeakLeft, peak.x);
        truePeakRight = max(truePeakRight, peak.y);
        if (chunk == 0) {
            value.xyz += float3(energyLeft, energyRight, rawEnergy);
            value.w = max(value.w, samplePeak);
            peak = max(peak, float2(chunkTruePeakLeft, chunkTruePeakRight));
        }
        if (chunk < chunkCount) {
            const ulong endFrame = params.startFrame + ulong((chunk + 1) * 480 - oldPartial);
            const uint base = analysisChunkOffset + chunk * 8;
            output[base] = uint(endFrame); output[base + 1] = uint(endFrame >> 32);
            output[base + 2] = as_type<uint>(value.x); output[base + 3] = as_type<uint>(value.y);
            output[base + 4] = as_type<uint>(value.z); output[base + 5] = as_type<uint>(value.w);
            output[base + 6] = as_type<uint>(peak.x); output[base + 7] = as_type<uint>(peak.y);
        } else {
            energyLeft = value.x; energyRight = value.y; rawEnergy = value.z;
            samplePeak = value.w; chunkTruePeakLeft = peak.x; chunkTruePeakRight = peak.y;
        }
    }

    if (params.frameCount > 0) {
        const uint last = params.frameCount - 1;
        const uint frame = uint((params.startFrame + last) % params.ringFrames);
        const ulong base = ulong(frame) * params.channels;
        float2 raw(samples[base + params.leftChannel], samples[base + params.rightChannel]);
        raw = select(float2(0), raw, isfinite(raw));
        float2 rawPrevious(as_type<float>(state[0]), as_type<float>(state[4]));
        float2 shelfPrevious(as_type<float>(state[2]), as_type<float>(state[6]));
        float2 passInputPrevious(as_type<float>(state[8]), as_type<float>(state[12]));
        float2 passPrevious(as_type<float>(state[10]), as_type<float>(state[14]));
        if (last > 0) {
            const uint previousFrame = uint((params.startFrame + last - 1) % params.ringFrames);
            const ulong previousBase = ulong(previousFrame) * params.channels;
            rawPrevious = float2(samples[previousBase + params.leftChannel], samples[previousBase + params.rightChannel]);
            rawPrevious = select(float2(0), rawPrevious, isfinite(rawPrevious));
            shelfPrevious = shelf[last - 1]; passInputPrevious = shelfPrevious;
            passPrevious = filtered[last - 1];
        }
        const float2 s = shelf[last], y = filtered[last];
        state[0] = as_type<uint>(raw.x); state[1] = as_type<uint>(rawPrevious.x);
        state[2] = as_type<uint>(s.x); state[3] = as_type<uint>(shelfPrevious.x);
        state[4] = as_type<uint>(raw.y); state[5] = as_type<uint>(rawPrevious.y);
        state[6] = as_type<uint>(s.y); state[7] = as_type<uint>(shelfPrevious.y);
        state[8] = as_type<uint>(s.x); state[9] = as_type<uint>(passInputPrevious.x);
        state[10] = as_type<uint>(y.x); state[11] = as_type<uint>(passPrevious.x);
        state[12] = as_type<uint>(s.y); state[13] = as_type<uint>(passInputPrevious.y);
        state[14] = as_type<uint>(y.y); state[15] = as_type<uint>(passPrevious.y);
    }
    state[16] = as_type<uint>(energyLeft);
    state[17] = as_type<uint>(energyRight);
    state[18] = partialFrames;
    for (uint tap = 0; tap < 12; ++tap) {
        float2 value;
        if (tap < params.frameCount) {
            const uint frame = uint((params.startFrame + params.frameCount - 1 - tap) % params.ringFrames);
            const ulong base = ulong(frame) * params.channels;
            value = float2(samples[base + params.leftChannel], samples[base + params.rightChannel]);
            value = select(float2(0), value, isfinite(value));
        } else {
            value = float2(truePeakHistoryLeft[tap - params.frameCount], truePeakHistoryRight[tap - params.frameCount]);
        }
        state[20 + tap] = as_type<uint>(value.x);
        state[32 + tap] = as_type<uint>(value.y);
    }
    state[44] = min(12u, truePeakHistoryFrames + params.frameCount);
    state[45] = as_type<uint>(rawEnergy);
    state[46] = as_type<uint>(samplePeak);
    state[47] = as_type<uint>(chunkTruePeakLeft);
    state[48] = as_type<uint>(chunkTruePeakRight);
    output[16] = chunkCount;
    output[92] = as_type<uint>(truePeakLeft);
    output[93] = as_type<uint>(truePeakRight);
    output[94] = uint(truePeakValid);
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

struct AnalyzerPlotParams {
    uint mode; uint index; uint active; uint count;
    ulong latestFrame; uint sampleRate; uint padding;
    float value; float peak; float width; float height;
};
struct AnalyzerHistoryVertex {
    ulong frame; float correlation; float sideEnergy; uint breakBefore; float integrated;
};
struct AnalyzerPlotVertex { float4 position [[position]]; float4 color; };
vertex AnalyzerPlotVertex asfwAnalyzerPlotVertex(uint vid [[vertex_id]],
    constant AnalyzerPlotParams& p [[buffer(0)]],
    device const AnalyzerHistoryVertex* points [[buffer(1)]]) {
    float4 color = float4(0.20f, 0.91f, 0.73f, 1);
    if (p.mode == 2 || p.mode == 3) {
        uint segment = vid / 2;
        AnalyzerHistoryVertex a = points[segment];
        AnalyzerHistoryVertex b = points[segment + 1];
        AnalyzerHistoryVertex point = points[segment + vid % 2];
        bool broken = b.breakBefore != 0 || b.frame < a.frame ||
            b.frame - a.frame > ulong(p.sampleRate / 5);
        float age = float(p.latestFrame - point.frame) / max(1.0f, float(p.sampleRate));
        float x = 1.0f - 2.0f * age / 60.0f;
        float y = p.index == 0 ? point.correlation : point.sideEnergy * 2.0f - 1.0f;
        color = p.index == 0 ? float4(0.1f, 0.9f, 0.3f, 1) : float4(0.1f, 0.8f, 1, 1);
        if (p.mode == 3) {
            float value = p.index == 0 ? point.correlation : p.index == 1 ? point.sideEnergy : point.integrated;
            y = 2.0f * (value + 36.0f) / 30.0f - 1.0f;
            color = p.index == 0 ? float4(0.1f,0.9f,0.4f,1) : p.index == 1 ? float4(0.1f,0.55f,1,1) : float4(0.65f,0.25f,1,1);
            AnalyzerHistoryVertex other = points[segment + 1 - vid % 2];
            float otherValue = p.index == 0 ? other.correlation : p.index == 1 ? other.sideEnergy : other.integrated;
            if (!isfinite(value) || !isfinite(otherValue)) color.a = 0;
        }
        if (broken || age > 60) color.a = 0;
        return { float4(x, clamp(y, -1.0f, 1.0f), 0, 1), color };
    }
    // Two rectangles: RMS fill + sample-peak marker, or indicator + centre tick.
    uint rectangle = vid / 6;
    const float2 corners[6] = { float2(0,0), float2(1,0), float2(0,1),
                               float2(0,1), float2(1,0), float2(1,1) };
    float2 corner = corners[vid % 6];
    float2 lo; float2 hi;
    if (p.mode == 4) {
        float level = clamp((p.value + 60.0f) / 60.0f, 0.0f, 1.0f);
        lo = float2(-1,-1); hi = float2(-1 + 2 * level,1);
        color = p.index == 0 ? float4(0.1f,0.9f,0.4f,1) : p.index == 1 ? float4(0.1f,0.55f,1,1) : float4(0.65f,0.25f,1,1);
        if (!p.active || rectangle == 1) color.a = 0;
    } else if (p.mode == 0) {
        float level = clamp((20.0f * log10(max(p.value, 1.0e-6f)) + 60) / 66, 0.0f, 1.0f);
        float peak = clamp((20.0f * log10(max(p.peak, 1.0e-6f)) + 60) / 66, 0.0f, 1.0f);
        lo = float2(-1, -1); hi = float2(1, -1 + 2 * level);
        color = mix(float4(0.1f,0.88f,0.48f,1), float4(1,0.55f,0.1f,1),
                    clamp((corner.y * level - 0.65f) / 0.35f, 0.0f, 1.0f));
        if (rectangle == 1) {
            float y = -1 + 2 * peak;
            lo.y = max(-1.0f, y - 2 / max(1.0f, p.height));
            hi.y = min(1.0f, y + 2 / max(1.0f, p.height));
            color = float4(1, 0.55f, 0.1f, 1);
        }
        if (!p.active) color.a = 0;
    } else {
        float x = clamp(p.value, -1.0f, 1.0f);
        float radius = 8 / max(1.0f, p.width);
        x *= 1 - radius;
        lo = float2(x - radius, -0.65f); hi = float2(x + radius, 0.65f);
        if (p.index == 0 && p.value < 0) color = float4(1,0.55f,0.1f,1);
        if (!p.active) color.a = 0;
        if (rectangle == 1) { lo = float2(-1 / max(1.0f,p.width),-1); hi = -lo; color = float4(0.5f,0.5f,0.5f,1); }
    }
    return { float4(mix(lo, hi, corner), 0, 1), color };
}
fragment float4 asfwAnalyzerPlotFragment(AnalyzerPlotVertex in [[stage_in]]) {
    if (in.color.a == 0) discard_fragment();
    return in.color;
}

// Readout glyphs from AnalyzerGlyphAtlas: positions in drawable pixels (y
// down), coverage from an R8 atlas sampled pixel for pixel.
struct AnalyzerGlyphVertex { float2 position; float2 uv; };
struct AnalyzerGlyphOut { float4 position [[position]]; float2 uv; };

vertex AnalyzerGlyphOut asfwAnalyzerGlyphVertex(uint vid [[vertex_id]],
    device const AnalyzerGlyphVertex* vertices [[buffer(0)]],
    constant float2& drawableSize [[buffer(1)]]) {
    AnalyzerGlyphVertex v = vertices[vid];
    float2 ndc = float2(v.position.x / drawableSize.x * 2 - 1, 1 - v.position.y / drawableSize.y * 2);
    return { float4(ndc, 0, 1), v.uv };
}

fragment float4 asfwAnalyzerGlyphFragment(AnalyzerGlyphOut in [[stage_in]],
    texture2d<float> atlas [[texture(0)]],
    constant float4& color [[buffer(0)]]) {
    constexpr sampler pixelExact(filter::nearest, address::clamp_to_edge);
    float coverage = atlas.sample(pixelExact, in.uv).r;
    if (coverage <= 0) discard_fragment();
    return float4(color.rgb, color.a * coverage);
}
