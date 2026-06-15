#pragma once

#include <cstdint>

namespace ASFW::IsochTransport {

enum class AudioHalBufferProfileId : uint8_t {
    Aligned512 = 0,
    PreDiceZts192 = 1,
    DiceWorking1536 = 2,
};

struct AudioHalBufferProfile final {
    const char* name;
    uint32_t frameRingFrames;
    uint32_t clientIoBudgetFrames;
    uint32_t zeroTimestampPeriodFrames;
};

// Historical aligned geometry. Retained as a compile-time fallback.
inline constexpr AudioHalBufferProfile kAudioHalBufferProfileAligned512{
    "aligned-512",
    512,
    512,
    512,
};

// Pre-DICE geometry retained for comparison.
inline constexpr AudioHalBufferProfile kAudioHalBufferProfilePreDiceZts192{
    "pre-dice-zts-192",
    1536,
    512,
    192,
};

// Working DICE baseline from e28f1a4 (tag: adk-duplex-working).
inline constexpr AudioHalBufferProfile kAudioHalBufferProfileDiceWorking1536{
    "dice-working-1536",
    1536,
    512,
    1536,
};

[[nodiscard]] constexpr bool IsValidAudioHalBufferProfile(
    const AudioHalBufferProfile& profile) noexcept {
    return profile.name != nullptr &&
           profile.frameRingFrames != 0 &&
           profile.clientIoBudgetFrames != 0 &&
           profile.zeroTimestampPeriodFrames != 0 &&
           profile.frameRingFrames % profile.clientIoBudgetFrames == 0 &&
           profile.frameRingFrames % profile.zeroTimestampPeriodFrames == 0;
}

static_assert(IsValidAudioHalBufferProfile(kAudioHalBufferProfileAligned512));
static_assert(IsValidAudioHalBufferProfile(
    kAudioHalBufferProfilePreDiceZts192));
static_assert(IsValidAudioHalBufferProfile(
    kAudioHalBufferProfileDiceWorking1536));

[[nodiscard]] constexpr AudioHalBufferProfile SelectAudioHalBufferProfile(
    AudioHalBufferProfileId id) noexcept {
    switch (id) {
        case AudioHalBufferProfileId::Aligned512:
            return kAudioHalBufferProfileAligned512;
        case AudioHalBufferProfileId::PreDiceZts192:
            return kAudioHalBufferProfilePreDiceZts192;
        case AudioHalBufferProfileId::DiceWorking1536:
        default:
            return kAudioHalBufferProfileDiceWorking1536;
    }
}

// Override with -DASFW_AUDIO_HAL_BUFFER_PROFILE=0, 1, or 2. Buffer geometry is
// compile-time because it sizes shared memory exported across driver processes.
#if defined(ASFW_AUDIO_HAL_BUFFER_PROFILE)
inline constexpr uint8_t kAudioHalBufferProfileRaw =
    ASFW_AUDIO_HAL_BUFFER_PROFILE;
#else
inline constexpr uint8_t kAudioHalBufferProfileRaw = 2;
#endif

static_assert(
    kAudioHalBufferProfileRaw <= 2,
    "Invalid ASFW_AUDIO_HAL_BUFFER_PROFILE; use 0 (aligned-512), "
    "1 (pre-dice-zts-192), or 2 (dice-working-1536)");

inline constexpr AudioHalBufferProfileId kActiveAudioHalBufferProfileId =
    static_cast<AudioHalBufferProfileId>(kAudioHalBufferProfileRaw);
inline constexpr AudioHalBufferProfile kActiveAudioHalBufferProfile =
    SelectAudioHalBufferProfile(kActiveAudioHalBufferProfileId);

} // namespace ASFW::IsochTransport
