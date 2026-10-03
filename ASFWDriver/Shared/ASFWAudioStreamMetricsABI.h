#ifndef ASFW_AUDIO_STREAM_METRICS_ABI_H
#define ASFW_AUDIO_STREAM_METRICS_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASFW_AUDIO_STREAM_METRICS_ABI_VERSION 1u

typedef enum ASFWAudioStreamMetricsStatus : uint32_t {
    ASFWAudioStreamMetricsStatusOK = 0,
    ASFWAudioStreamMetricsStatusUnavailable = 1,
    ASFWAudioStreamMetricsStatusBusy = 2
} ASFWAudioStreamMetricsStatus;

typedef enum ASFWAudioStreamMetricsStateFlags : uint32_t {
    ASFWAudioStreamMetricsStateConfigAvailable = 1u << 0,
    ASFWAudioStreamMetricsStateControlAvailable = 1u << 1,
    ASFWAudioStreamMetricsStateStreaming = 1u << 2,
    ASFWAudioStreamMetricsStateConsistent = 1u << 3
} ASFWAudioStreamMetricsStateFlags;

typedef struct ASFWAudioStreamMetricsRequestV1 {
    uint32_t abiVersion;
    uint32_t structSize;
    uint64_t guid;
} ASFWAudioStreamMetricsRequestV1;

// Read-only point sample of the shared direct-audio control block. Values are
// cumulative within streamGeneration and reset when the stream is re-armed.
// StateConsistent means the endpoint binding and reset sequence did not change
// while the atomics were copied; it does not turn independently updated
// counters into one transactional event.
typedef struct ASFWAudioStreamMetricsSnapshotV1 {
    uint32_t abiVersion;
    uint32_t structSize;
    uint32_t status;
    uint32_t stateFlags;

    uint64_t guid;
    uint64_t timestampNs;
    uint64_t endpointGeneration;
    uint64_t streamGeneration;

    uint32_t sampleRateHz;
    uint32_t outputChannels;
    uint32_t inputChannels;
    uint32_t reserved0;

    uint64_t ioCallbackGeneration;
    uint64_t ioCallbackErrorGeneration;
    uint64_t fatalGeneration;
    uint64_t discontinuities;

    uint32_t ioLastError;
    uint32_t fatalReason;
    uint32_t reserved1;
    uint32_t reserved2;

    uint64_t outputClientWriteEndFrame;
    uint64_t outputConsumedEndFrame;
    uint64_t outputUnderruns;
    uint64_t playbackRingWriteFrame;
    uint64_t playbackRingReadFrame;
    uint64_t playbackRingOldestValidFrame;
    uint64_t playbackRingUnderruns;
    uint64_t playbackRingOverruns;
    uint64_t txPackets;
    uint64_t txDataPackets;
    uint64_t txNoDataPackets;
    uint64_t txSilenceSubstitutions;
    uint64_t txPcmFramesEncoded;
    uint64_t txPcmNonzeroPackets;
    uint64_t txPcmAllZeroPackets;
    uint64_t txScheduledSampleFrame;
    uint64_t txCompletedSampleFrame;
    uint64_t txReplayEntries;
    uint64_t txReplayUnderflows;
    uint64_t txReplayInvalidSyt;

    uint64_t inputClientReadEndFrame;
    uint64_t inputProducedEndFrame;
    uint64_t inputOverruns;
    uint64_t rxDbcFrameCount;
    uint64_t captureRingWriteFrame;
    uint64_t captureRingReadFrame;
    uint64_t captureRingOverruns;
    uint64_t captureRingStarvations;
    uint64_t rxPackets;
    uint64_t rxDecodedFrames;
    uint64_t rxDiscontinuities;
    uint64_t rxReplayEntries;
    uint64_t rxReplayEpochResets;
    uint64_t ztsRxAdkPublished;

    uint32_t txMinimumPreparationDistance;
    uint32_t txMinimumCommittedMarginPackets;
    uint32_t rxTransferDelayTicks;
    uint32_t txTransferDelayTicks;
} ASFWAudioStreamMetricsSnapshotV1;

#ifdef __cplusplus
} // extern "C"

static_assert(sizeof(ASFWAudioStreamMetricsRequestV1) == 16);
static_assert(sizeof(ASFWAudioStreamMetricsSnapshotV1) == 400);
#endif

#endif // ASFW_AUDIO_STREAM_METRICS_ABI_H
