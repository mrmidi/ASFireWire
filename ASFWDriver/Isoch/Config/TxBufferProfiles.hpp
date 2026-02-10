#pragma once

#include <cstdint>

namespace ASFW::Isoch::Config {

#define ASFW_TX_PROFILE_A 1 // 
#define ASFW_TX_PROFILE_B 2 // Balanced profile with moderate start wait and larger ring buffer targets, suitable for general use.
#define ASFW_TX_PROFILE_C 3 // Low-latency profile with minimal start wait and smaller ring buffer targets

#ifndef ASFW_TX_TUNING_PROFILE
#define ASFW_TX_TUNING_PROFILE ASFW_TX_PROFILE_A
#endif

struct TxBufferProfile {
    const char* name;
    uint32_t startWaitTargetFrames;
    uint32_t startupPrimeLimitFrames;    // 0 = unbounded pre-prime
    uint32_t legacyRbTargetFrames;
    uint32_t legacyRbMaxFrames;
    uint32_t legacyMaxChunksPerRefill;
};

inline constexpr uint32_t kSharedTxQueueCapacityFrames = 4096;
inline constexpr uint32_t kTransferChunkFrames = 256;

inline constexpr TxBufferProfile kTxProfileA{
    "A",
    256,   // startWaitTargetFrames
    512,   // startupPrimeLimitFrames
    512,   // legacyRbTargetFrames
    768,   // legacyRbMaxFrames
    6      // legacyMaxChunksPerRefill
};

inline constexpr TxBufferProfile kTxProfileB{
    "B",
    512,   // startWaitTargetFrames
    0,     // startupPrimeLimitFrames (unbounded)
    1024,  // legacyRbTargetFrames
    1536,  // legacyRbMaxFrames
    8      // legacyMaxChunksPerRefill
};

inline constexpr TxBufferProfile kTxProfileC{
    "C",
    128,   // startWaitTargetFrames
    256,   // startupPrimeLimitFrames
    256,   // legacyRbTargetFrames
    384,   // legacyRbMaxFrames
    4      // legacyMaxChunksPerRefill
};

constexpr bool IsValidProfile(const TxBufferProfile& profile) noexcept {
    return profile.startWaitTargetFrames > 0 &&
           profile.legacyRbTargetFrames > 0 &&
           profile.legacyRbTargetFrames <= profile.legacyRbMaxFrames &&
           profile.legacyMaxChunksPerRefill > 0;
}

static_assert(IsValidProfile(kTxProfileA), "Profile A is invalid");
static_assert(IsValidProfile(kTxProfileB), "Profile B is invalid");
static_assert(IsValidProfile(kTxProfileC), "Profile C is invalid");

static_assert(kTxProfileA.startWaitTargetFrames <= kSharedTxQueueCapacityFrames,
              "Profile A startWait exceeds shared queue capacity");
static_assert(kTxProfileB.startWaitTargetFrames <= kSharedTxQueueCapacityFrames,
              "Profile B startWait exceeds shared queue capacity");
static_assert(kTxProfileC.startWaitTargetFrames <= kSharedTxQueueCapacityFrames,
              "Profile C startWait exceeds shared queue capacity");

#if ASFW_TX_TUNING_PROFILE == ASFW_TX_PROFILE_A
inline constexpr TxBufferProfile kTxBufferProfile = kTxProfileA;
#elif ASFW_TX_TUNING_PROFILE == ASFW_TX_PROFILE_B
inline constexpr TxBufferProfile kTxBufferProfile = kTxProfileB;
#elif ASFW_TX_TUNING_PROFILE == ASFW_TX_PROFILE_C
inline constexpr TxBufferProfile kTxBufferProfile = kTxProfileC;
#else
#error "Invalid ASFW_TX_TUNING_PROFILE value. Use ASFW_TX_PROFILE_A/B/C."
#endif

static_assert(IsValidProfile(kTxBufferProfile), "Selected TX buffer profile is invalid");
static_assert(kTransferChunkFrames == 256, "Transfer chunk size must stay fixed at 256");

}  // namespace ASFW::Isoch::Config
