//
// LogRing.hpp
// ASFWDriver
//
// Driver-owned in-memory log ring: the queryable backend behind ASFW_LOG.
//
// os_log from a dext is expensive (per-call formatting + out-of-process
// delivery), unfilterable at the source, and everything lands in one
// undifferentiated unified-log stream. The ring keeps structured records
// (sequence, timestamp, category, level, formatted text) in ordinary driver
// heap memory so hot paths pay one atomic claim, one vsnprintf into a fixed
// slot, and nothing else. The user client drains it with category/level/
// substring filters so the MCP control plane can ask for "the last 200 [CMP]
// records" instead of replaying the whole console.
//
// Concurrency: multiple producers (Default queue, audio RT threads, isoch
// callbacks) claim a global sequence, then use a per-slot non-blocking atomic
// gate while formatting/copying the record. A producer never waits: if a
// reader or a lapping producer has the slot, that record is counted as dropped
// and the hot path continues. Readers likewise skip busy slots. This is
// deliberately not a C++ "seqlock": copying non-atomic payload bytes while a
// writer changes them would still be a data race even if a sequence were
// re-checked afterwards.
//

#pragma once

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>

namespace ASFW::Logging {

/// Stable category identifiers, mirroring the os_log category list in
/// Logging.hpp. Wire values are frozen: the user client and MCP layer encode
/// filters as (1u << category). Append new categories at the end only.
enum class LogCategory : uint8_t {
    Controller = 0,
    Hardware = 1,
    BusReset = 2,
    Topology = 3,
    Metrics = 4,
    Async = 5,
    UserClient = 6,
    Discovery = 7,
    IRM = 8,
    BusManager = 9,
    ConfigROM = 10,
    MusicSubunit = 11,
    FCP = 12,
    CMP = 13,
    AVC = 14,
    Isoch = 15,
    Audio = 16,
    DirectAudio = 17,
    DICE = 18,
    Zts = 19,
    TxSyt = 20,
    // 21 is retired, not reused. It was PayloadWriter, whose telemetry
    // pipeline lost its producer in 31ef0286 and has now been removed. IDs
    // are frozen and positional across the dext/app boundary
    // (ASFWLogRingCategories indexes its name array by this value), so the
    // slot keeps its number and a placeholder name; renumbering would
    // silently mislabel every later category when only one side is rebuilt.
    Reserved21 = 21,
    // Device backends get one category each, so a single filter isolates a
    // backend end to end. DICE (18) is the precedent; Oxfw covers the
    // OXFW-common layer and its device overlays (Apogee Duet).
    Oxfw = 22,
    Count = 23,
};

/// Severity scale (smaller = more severe), used for filtering.
enum class LogLevel : uint8_t {
    Error = 0,
    Warning = 1,
    Notice = 2,  // plain ASFW_LOG default
    Info = 3,
    Debug = 4,
};

inline constexpr uint32_t kLogRingCategoryCount =
    static_cast<uint32_t>(LogCategory::Count);
inline constexpr size_t kLogCategoryNameCapacity = 28;
inline constexpr size_t kLogCategoryPresetNameCapacity = 24;

[[nodiscard]] constexpr size_t CatalogNameLength(const char* name) noexcept {
    size_t length = 0;
    while (name != nullptr && name[length] != '\0') {
        ++length;
    }
    return length;
}

struct LogCategoryDefinition {
    LogCategory category;
    const char* name;
};

/// C++ is the source of truth for category IDs and display names. The table is
/// exported through selector 1014 so clients never need to mirror this enum.
inline constexpr LogCategoryDefinition kLogCategoryDefinitions[] = {
    {LogCategory::Controller, "Controller"},
    {LogCategory::Hardware, "Hardware"},
    {LogCategory::BusReset, "BusReset"},
    {LogCategory::Topology, "Topology"},
    {LogCategory::Metrics, "Metrics"},
    {LogCategory::Async, "Async"},
    {LogCategory::UserClient, "UserClient"},
    {LogCategory::Discovery, "Discovery"},
    {LogCategory::IRM, "IRM"},
    {LogCategory::BusManager, "BusManager"},
    {LogCategory::ConfigROM, "ConfigROM"},
    {LogCategory::MusicSubunit, "MusicSubunit"},
    {LogCategory::FCP, "FCP"},
    {LogCategory::CMP, "CMP"},
    {LogCategory::AVC, "AVC"},
    {LogCategory::Isoch, "Isoch"},
    {LogCategory::Audio, "Audio"},
    {LogCategory::DirectAudio, "DirectAudio"},
    {LogCategory::DICE, "DICE"},
    {LogCategory::Zts, "Zts"},
    {LogCategory::TxSyt, "TxSyt"},
    {LogCategory::Reserved21, "Reserved21"},
    {LogCategory::Oxfw, "Oxfw"},
};

[[nodiscard]] constexpr uint32_t CategoryBit(LogCategory category) noexcept {
    return 1U << static_cast<uint8_t>(category);
}

constexpr bool LogCategoryDefinitionsAreValid() noexcept {
    if (sizeof(kLogCategoryDefinitions) / sizeof(kLogCategoryDefinitions[0]) !=
        kLogRingCategoryCount) {
        return false;
    }
    for (uint32_t index = 0; index < kLogRingCategoryCount; ++index) {
        if (static_cast<uint8_t>(kLogCategoryDefinitions[index].category) != index ||
            CatalogNameLength(kLogCategoryDefinitions[index].name) == 0 ||
            CatalogNameLength(kLogCategoryDefinitions[index].name) >
                kLogCategoryNameCapacity) {
            return false;
        }
    }
    return true;
}
static_assert(LogCategoryDefinitionsAreValid(),
              "log category catalog must cover every frozen ID in order");

/// Fixed-size record copied into the packed user-client response.
struct LogRecord {
    uint64_t sequence{0};      // 0 = never written
    uint64_t timestampNs{0};   // NowNs() at emission
    uint8_t category{0};       // LogCategory
    uint8_t level{0};          // LogLevel
    uint16_t messageLength{0}; // formatted length before truncation
    uint32_t reserved{0};
    char message[232]{};       // NUL-terminated, truncated to fit
};
static_assert(sizeof(LogRecord) == 256, "wire format is frozen at 256 bytes");

inline constexpr size_t kLogRingBudgetBytes = size_t{10} * 1024 * 1024;

struct LogSlot {
    std::atomic_flag gate = ATOMIC_FLAG_INIT;
    LogRecord record{};
};

inline constexpr uint32_t kLogRingCapacityRecords =
    static_cast<uint32_t>(kLogRingBudgetBytes / sizeof(LogSlot));

/// Filter + cursor for a drain request. `afterSequence` is the last sequence
/// the caller has already seen (0 = from the oldest retained record).
struct LogRingQuery {
    uint64_t afterSequence{0};
    uint32_t categoryMask{0xFFFFFFFFU}; // bit (1 << LogCategory)
    uint8_t maxLevel{static_cast<uint8_t>(LogLevel::Debug)};
    char contains[48]{}; // empty = no substring filter
};

struct LogRingQueryResult {
    uint32_t recordCount{0};   // records copied to the caller's buffer
    uint32_t scannedCount{0};  // records visited (filter hit rate diagnostics)
    // The supplied cursor belonged to a later ring instance (or was otherwise
    // impossible for this instance), so scanning restarted at oldestSequence.
    bool cursorReset{false};
    uint64_t nextSequence{0};  // pass back as afterSequence to continue
    uint64_t latestSequence{0};
    uint64_t oldestSequence{0}; // oldest still retained when the scan started
};

struct LogRingStats {
    uint64_t totalEmitted{0};
    uint64_t droppedRecords{0};
    uint64_t latestSequence{0};
    uint64_t oldestSequence{0};
    uint32_t capacityRecords{0};
    uint64_t perCategory[kLogRingCategoryCount]{};
};

class LogRing;

// ---------------------------------------------------------------------------
// User-client drain wire format (selectors 1011/1012).
//
// IOConnectCallStructMethod output is capped at 4 KiB per call in the app's
// client, so records travel packed: a response header followed by
// recordCount × { LogRecordHeaderWire, messageLength bytes (no NUL) }.
// The caller loops with nextSequence until it catches up.
// ---------------------------------------------------------------------------

struct __attribute__((packed)) LogRingQueryRequestWire {
    uint64_t afterSequence{0};
    uint32_t categoryMask{0xFFFFFFFFU};
    uint32_t maxLevel{static_cast<uint32_t>(LogLevel::Debug)};
    uint32_t maxRecords{0}; // 0 = fill the byte budget
    uint32_t reserved{0};
    char contains[48]{};    // NUL-terminated substring filter; empty = all
};
static_assert(sizeof(LogRingQueryRequestWire) == 72,
              "log query request wire layout is frozen");

struct __attribute__((packed)) LogRingQueryResponseHeaderWire {
    uint32_t recordCount{0};
    uint32_t scannedCount{0};
    uint64_t nextSequence{0};
    uint64_t latestSequence{0};
    uint64_t oldestSequence{0};
    uint32_t payloadBytes{0}; // bytes following this header
    // Bit 0: request afterSequence was beyond this ring's latest sequence;
    // the response restarted at oldestSequence instead of silently skipping
    // the new driver's history. Other bits are reserved.
    uint32_t flags{0};
};
static_assert(sizeof(LogRingQueryResponseHeaderWire) == 40,
              "log query response header wire layout is frozen");

inline constexpr uint32_t kLogRingQueryFlagCursorReset = 1U << 0;

struct __attribute__((packed)) LogRecordHeaderWire {
    uint64_t sequence{0};
    uint64_t timestampNs{0};
    uint8_t category{0};
    uint8_t level{0};
    uint16_t messageLength{0}; // bytes of message text following this header
};
static_assert(sizeof(LogRecordHeaderWire) == 20,
              "log record wire layout is frozen");

struct __attribute__((packed)) LogRingStatsWire {
    uint64_t totalEmitted{0};
    uint64_t latestSequence{0};
    uint64_t oldestSequence{0};
    uint32_t capacityRecords{0};
    uint32_t categoryCount{kLogRingCategoryCount};
    uint64_t perCategory[kLogRingCategoryCount]{};
    uint64_t droppedRecords{0};
};
static_assert(sizeof(LogRingStatsWire) == 40 + (8 * kLogRingCategoryCount),
              "log stats wire layout is frozen");

// ---------------------------------------------------------------------------
// Category catalog wire format (selector 1014).
//
// Named presets are exported alongside categories because preset membership
// is semantic driver knowledge. In particular, TX diagnostics span several
// categories and must not be reconstructed independently by each UI client.
// ---------------------------------------------------------------------------

inline constexpr uint32_t kLogCategoryCatalogAbiVersion = 1;

struct LogCategoryPresetDefinition {
    const char* name;
    uint32_t categoryMask;
};

inline constexpr LogCategoryPresetDefinition kLogCategoryPresetDefinitions[] = {
    {
        "Audio Transmit",
        CategoryBit(LogCategory::Isoch) |
            CategoryBit(LogCategory::Audio) |
            CategoryBit(LogCategory::DirectAudio) |
            CategoryBit(LogCategory::Zts) |
            CategoryBit(LogCategory::TxSyt),
    },
};
inline constexpr uint32_t kLogCategoryPresetCount =
    static_cast<uint32_t>(sizeof(kLogCategoryPresetDefinitions) /
                          sizeof(kLogCategoryPresetDefinitions[0]));

constexpr bool LogCategoryPresetDefinitionsAreValid() noexcept {
    uint32_t knownCategoryMask = 0;
    for (const auto& definition : kLogCategoryDefinitions) {
        knownCategoryMask |= CategoryBit(definition.category);
    }
    for (const auto& preset : kLogCategoryPresetDefinitions) {
        const size_t nameLength = CatalogNameLength(preset.name);
        if (nameLength == 0 || nameLength > kLogCategoryPresetNameCapacity ||
            preset.categoryMask == 0 ||
            (preset.categoryMask & ~knownCategoryMask) != 0) {
            return false;
        }
    }
    return true;
}
static_assert(LogCategoryPresetDefinitionsAreValid(),
              "log presets must have valid names and known category bits");

struct __attribute__((packed)) LogCategoryCatalogHeaderWire {
    uint32_t abiVersion{kLogCategoryCatalogAbiVersion};
    uint16_t categoryCount{static_cast<uint16_t>(kLogRingCategoryCount)};
    uint16_t presetCount{static_cast<uint16_t>(kLogCategoryPresetCount)};
    uint16_t categoryRecordSize{32};
    uint16_t presetRecordSize{32};
    uint32_t totalBytes{0};
};
static_assert(sizeof(LogCategoryCatalogHeaderWire) == 16,
              "log category catalog header layout is frozen");

struct __attribute__((packed)) LogCategoryDescriptorWire {
    uint8_t category{0};
    uint8_t nameLength{0};
    uint16_t flags{0};
    char name[kLogCategoryNameCapacity]{};
};
static_assert(sizeof(LogCategoryDescriptorWire) == 32,
              "log category descriptor layout is frozen");

struct __attribute__((packed)) LogCategoryPresetWire {
    uint32_t categoryMask{0};
    uint8_t nameLength{0};
    uint8_t reserved[3]{};
    char name[kLogCategoryPresetNameCapacity]{};
};
static_assert(sizeof(LogCategoryPresetWire) == 32,
              "log category preset layout is frozen");

struct __attribute__((packed)) LogCategoryCatalogWire {
    LogCategoryCatalogHeaderWire header{};
    LogCategoryDescriptorWire categories[kLogRingCategoryCount]{};
    LogCategoryPresetWire presets[kLogCategoryPresetCount]{};
};
static_assert(sizeof(LogCategoryCatalogWire) ==
                  sizeof(LogCategoryCatalogHeaderWire) +
                      (sizeof(LogCategoryDescriptorWire) * kLogRingCategoryCount) +
                      (sizeof(LogCategoryPresetWire) * kLogCategoryPresetCount),
              "log category catalog must not contain padding");
static_assert(sizeof(LogCategoryCatalogWire) <= 4096,
              "log category catalog must fit inline user-client output");

/// Drains filtered records into `outBuffer` in the packed wire format
/// (header + records), bounded by `bufferCapacity`. Returns the total bytes
/// written (header included); 0 only if the buffer cannot hold the header.
size_t PackLogRecords(const LogRing& ring, const LogRingQueryRequestWire& request,
                      uint8_t* outBuffer, size_t bufferCapacity) noexcept;

/// Fills the stats wire struct from the ring.
void PackLogStats(const LogRing& ring, LogRingStatsWire& outStats) noexcept;

/// Fills the category and named-preset catalog exported to clients.
void PackLogCategoryCatalog(LogCategoryCatalogWire& outCatalog) noexcept;

/// Maps a raw os_log_type_t value onto the ring's severity scale.
[[nodiscard]] constexpr LogLevel RingLevelForOsType(uint8_t osLogType) noexcept {
    switch (osLogType) {
        case 0x10: // OS_LOG_TYPE_ERROR
        case 0x11: // OS_LOG_TYPE_FAULT
            return LogLevel::Error;
        case 0x01: // OS_LOG_TYPE_INFO
        case 0x02: // OS_LOG_TYPE_DEBUG
            return LogLevel::Info;
        default:
            return LogLevel::Notice;
    }
}

/// Rewrites an os_log format string into plain printf by dropping `{...}`
/// annotations ("%{public}s" -> "%s"). Always NUL-terminates.
void SanitizeOsLogFormat(const char* input, char* output,
                         size_t outputSize) noexcept;

/// Formats through `ring` after sanitizing the os_log-style format string.
void RingLogTo(LogRing& ring, LogCategory category, LogLevel level,
               const char* osLogFormat, ...) noexcept;

/// RingLogTo against the shared ring — the ASFW_LOG macro backend.
void RingLog(LogCategory category, LogLevel level, const char* osLogFormat,
             ...) noexcept;

class LogRing {
public:
    static LogRing& Shared() noexcept;

    /// Allocates slot storage. Safe to call repeatedly; the first successful
    /// call wins. Called from driver Start(); Append before/without
    /// initialization is a silent no-op (never blocks a log site).
    void Initialize() noexcept;
    [[nodiscard]] bool IsInitialized() const noexcept;

    /// Formats and publishes one record. The format string must already be
    /// plain printf (os_log `%{...}` annotations stripped by the caller).
    void Append(LogCategory category, LogLevel level, const char* format,
                va_list args) noexcept;

    /// Copies up to `maxRecords` filtered records at/after the cursor into
    /// `outRecords` (oldest first). Busy or reused slots are skipped.
    /// `scanBudget` bounds one call's work so a drain with a very selective
    /// filter cannot stall the caller; continue via nextSequence.
    LogRingQueryResult Query(const LogRingQuery& query, LogRecord* outRecords,
                             uint32_t maxRecords,
                             uint32_t scanBudget = 8192) const noexcept;

    LogRingStats Stats() const noexcept;

    /// Testing hook: a private ring instance with a small capacity.
    static LogRing* CreateForTest(uint32_t capacityRecords) noexcept;
    static void DestroyForTest(LogRing* ring) noexcept;

private:
    LogRing() = default;
    ~LogRing() = default;
    LogRing(const LogRing&) = delete;
    LogRing& operator=(const LogRing&) = delete;

    [[nodiscard]] bool InitializeWithCapacity(uint32_t capacityRecords) noexcept;

    std::atomic<LogSlot*> slots_{nullptr};
    uint32_t capacity_{0};
    std::atomic<uint64_t> nextSequence_{0}; // last claimed sequence (1-based)
    std::atomic<uint64_t> droppedRecords_{0};
    std::atomic<uint64_t> perCategory_[kLogRingCategoryCount]{};
};

} // namespace ASFW::Logging
