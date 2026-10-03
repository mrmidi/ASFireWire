#ifndef ASFW_ISOCH_ORACLE_CAPTURE_ABI_H
#define ASFW_ISOCH_ORACLE_CAPTURE_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASFW_ISOCH_ORACLE_CAPTURE_ABI_VERSION 1u

// Records per chunk. The whole capture is 4096 records per direction; it is
// read in chunks so one external method never has to return ~100 KB.
#define ASFW_ISOCH_ORACLE_CAPTURE_CHUNK_RECORDS 128u

typedef enum ASFWIsochOracleCaptureStatus : uint32_t {
    ASFWIsochOracleCaptureStatusOK = 0,
    ASFWIsochOracleCaptureStatusUnavailable = 1,
    ASFWIsochOracleCaptureStatusBadDirection = 2,
    ASFWIsochOracleCaptureStatusOutOfRange = 3
} ASFWIsochOracleCaptureStatus;

typedef enum ASFWIsochOracleCaptureDirection : uint32_t {
    ASFWIsochOracleCaptureDirectionTx = 0, // host -> device (oracle ch33)
    ASFWIsochOracleCaptureDirectionRx = 1  // device -> host (oracle ch34)
} ASFWIsochOracleCaptureDirection;

// Mirrors ASFW::Audio::Runtime::IsochOracleRecordFlags. Absence is meaningful:
// a record whose cycle never arrived must stay distinguishable from one whose
// cycle really was zero.
typedef enum ASFWIsochOracleRecordFlags : uint8_t {
    ASFWIsochOracleFlagData = 1u << 0,
    ASFWIsochOracleFlagHasSph = 1u << 1,
    ASFWIsochOracleFlagHasCycle = 1u << 2,
    ASFWIsochOracleFlagHasDbc = 1u << 3,
    ASFWIsochOracleFlagValidCip = 1u << 4
} ASFWIsochOracleRecordFlags;

typedef struct ASFWIsochOracleCaptureRequestV1 {
    uint32_t abiVersion;
    uint32_t structSize;
    uint64_t guid;
    uint32_t direction;
    uint32_t startIndex;
} ASFWIsochOracleCaptureRequestV1;

// Byte-identical to ASFW::Audio::Runtime::IsochOracleRecord; the handler
// copies records verbatim and a static_assert on both sides pins that.
typedef struct ASFWIsochOracleRecordV1 {
    uint64_t packetIndex;
    uint32_t cycleTimestamp;
    uint32_t firstSph;
    uint16_t wireLengthBytes;
    uint16_t transferStatus;
    uint16_t residualCount;
    uint8_t dbc;
    uint8_t flags;
} ASFWIsochOracleRecordV1;

typedef struct ASFWIsochOracleCaptureChunkV1 {
    uint32_t abiVersion;
    uint32_t structSize;
    uint32_t status;
    uint32_t direction;

    uint64_t guid;
    uint64_t captureGeneration;

    // Whole-section state, repeated in every chunk so a reader never has to
    // correlate two calls to know whether the window it just read is complete.
    uint32_t sectionCount;    // records available in this direction
    uint32_t sectionCapacity; // records the section can ever hold
    uint32_t frozen;          // section reached its capacity
    uint32_t reserved0;

    uint64_t suppressed;  // packets seen after the freeze (expected, not a drop)
    uint64_t lostCycles;  // completion cycles that could not be attributed
    uint64_t backfilled;  // records that received their completion cycle

    uint32_t startIndex;
    uint32_t recordCount; // records actually returned in this chunk

    ASFWIsochOracleRecordV1 records[ASFW_ISOCH_ORACLE_CAPTURE_CHUNK_RECORDS];
} ASFWIsochOracleCaptureChunkV1;

#ifdef __cplusplus
} // extern "C"

static_assert(sizeof(ASFWIsochOracleRecordV1) == 24);
static_assert(sizeof(ASFWIsochOracleCaptureRequestV1) == 24);
static_assert(sizeof(ASFWIsochOracleCaptureChunkV1) == 3152);
#endif

#endif // ASFW_ISOCH_ORACLE_CAPTURE_ABI_H
