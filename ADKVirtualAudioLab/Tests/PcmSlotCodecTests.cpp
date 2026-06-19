#include "TestHarness.hpp"

#include "../Protocols/Audio/AMDTP/PcmSlotCodec.hpp"

namespace ASFW::LabTests {

using Protocols::Audio::AMDTP::PcmSlotCodec;
using Protocols::Audio::AMDTP::PcmSlotEncoding;

void RunPcmSlotCodecTests(TestContext& ctx) {
    // Scalar conversion: symmetric 2^23-1 scale, round half away from zero.
    CHECK_EQ_U32(ctx, PcmSlotCodec::Float32ToSigned24(1.0f), 0x007FFFFF);
    CHECK_EQ_U32(ctx, static_cast<uint32_t>(PcmSlotCodec::Float32ToSigned24(-1.0f)),
                 static_cast<uint32_t>(-8388607));
    CHECK_EQ_U32(ctx, PcmSlotCodec::Float32ToSigned24(0.0f), 0);
    // 0.5 * 8388607 = 4194303.5 → rounds away from zero → 4194304
    CHECK_EQ_U32(ctx, PcmSlotCodec::Float32ToSigned24(0.5f), 4194304);
    CHECK_EQ_U32(ctx, static_cast<uint32_t>(PcmSlotCodec::Float32ToSigned24(-0.5f)),
                 static_cast<uint32_t>(-4194304));
    // Clamp beyond full scale
    CHECK_EQ_U32(ctx, PcmSlotCodec::Float32ToSigned24(2.0f), 0x007FFFFF);
    CHECK_EQ_U32(ctx, static_cast<uint32_t>(PcmSlotCodec::Float32ToSigned24(-2.0f)),
                 static_cast<uint32_t>(-8388607));
    // Denormal-magnitude input quantizes to zero
    CHECK_EQ_U32(ctx, PcmSlotCodec::Float32ToSigned24(1e-38f), 0);

    // AM824 MBLA: 0x40 label, 24-bit two's complement payload
    CHECK_EQ_U32(ctx, PcmSlotCodec::EncodeFloat32(1.0f, PcmSlotEncoding::Am824MBLA),
                 0x407FFFFF);
    CHECK_EQ_U32(ctx, PcmSlotCodec::EncodeFloat32(-1.0f, PcmSlotEncoding::Am824MBLA),
                 0x40800001); // -8388607 & 0xFFFFFF = 0x800001
    CHECK_EQ_U32(ctx, PcmSlotCodec::EncodeFloat32(0.0f, PcmSlotEncoding::Am824MBLA),
                 0x40000000);

    // Raw 24-in-32, big-endian slot: no label, right-justified
    CHECK_EQ_U32(ctx, PcmSlotCodec::EncodeFloat32(1.0f, PcmSlotEncoding::RawSigned24In32BE),
                 0x007FFFFF);
    // Negative samples sign-extend into the top byte (0xFF), matching the
    // Saffire.kext host→device wire capture — not zero padding.
    CHECK_EQ_U32(ctx, PcmSlotCodec::EncodeFloat32(-1.0f, PcmSlotEncoding::RawSigned24In32BE),
                 0xFF800001);

    // Raw 24-in-32, little-endian slot: byte-swapped BE value
    CHECK_EQ_U32(ctx, PcmSlotCodec::EncodeFloat32(1.0f, PcmSlotEncoding::RawSigned24In32LE),
                 0xFFFF7F00);
    CHECK_EQ_U32(ctx, PcmSlotCodec::EncodeFloat32(-1.0f, PcmSlotEncoding::RawSigned24In32LE),
                 0x010080FF);
    CHECK_EQ_U32(ctx, PcmSlotCodec::EncodeFloat32(0.0f, PcmSlotEncoding::RawSigned24In32LE),
                 0x00000000);

    // Interleaved frame: 2 source channels into 3 slots (2 PCM + 1 untouched)
    {
        const float frame[2] = {1.0f, -1.0f};
        uint32_t slots[3] = {0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF};
        PcmSlotCodec::EncodeInterleavedFloat32Frame(frame, 2, slots, 3, 2,
                                                    PcmSlotEncoding::Am824MBLA);
        CHECK_EQ_U32(ctx, slots[0], 0x407FFFFF);
        CHECK_EQ_U32(ctx, slots[1], 0x40800001);
        CHECK_EQ_U32(ctx, slots[2], 0xDEADBEEF); // non-PCM slot untouched
    }

    // Fewer source channels than PCM slots: missing channels encode silence
    {
        const float frame[1] = {1.0f};
        uint32_t slots[2] = {0, 0};
        PcmSlotCodec::EncodeInterleavedFloat32Frame(frame, 1, slots, 2, 2,
                                                    PcmSlotEncoding::Am824MBLA);
        CHECK_EQ_U32(ctx, slots[0], 0x407FFFFF);
        CHECK_EQ_U32(ctx, slots[1], 0x40000000);
    }
}

} // namespace ASFW::LabTests
