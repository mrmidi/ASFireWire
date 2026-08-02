//
// LogRing.cpp
// ASFWDriver
//

#include "LogRing.hpp"

#include <cstdio>
#include <cstring>
#include <new>

#ifdef ASFW_HOST_TEST
#include <mach/mach_time.h>
#else
#include <DriverKit/IOLib.h>
#endif

namespace ASFW::Logging {

namespace {

uint64_t NowNs() noexcept {
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t value{};
        mach_timebase_info(&value);
        return value;
    }();
    const uint64_t t = mach_absolute_time();
    return (t * tb.numer) / tb.denom;
}

bool Contains(const char* haystack, const char* needle) noexcept {
    if (needle[0] == '\0') {
        return true;
    }
    return strstr(haystack, needle) != nullptr;
}

} // namespace

LogRing& LogRing::Shared() noexcept {
    static LogRing ring;
    return ring;
}

void LogRing::Initialize() noexcept {
    (void)InitializeWithCapacity(kLogRingCapacityRecords);
}

bool LogRing::IsInitialized() const noexcept {
    return slots_.load(std::memory_order_acquire) != nullptr;
}

bool LogRing::InitializeWithCapacity(uint32_t capacityRecords) noexcept {
    if (capacityRecords == 0) {
        return false;
    }
    if (slots_.load(std::memory_order_acquire) != nullptr) {
        return true; // first initialization wins
    }
    auto* storage = new (std::nothrow) LogSlot[capacityRecords]();
    if (storage == nullptr) {
        return false;
    }
    capacity_ = capacityRecords;
    LogSlot* expected = nullptr;
    if (!slots_.compare_exchange_strong(expected, storage,
                                        std::memory_order_release,
                                        std::memory_order_acquire)) {
        delete[] storage; // lost a concurrent init race
    }
    return true;
}

void LogRing::Append(LogCategory category, LogLevel level, const char* format,
                     va_list args) noexcept {
    LogSlot* slots = slots_.load(std::memory_order_acquire);
    if (slots == nullptr || format == nullptr) {
        return;
    }

    const uint64_t sequence =
        nextSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    LogSlot& slot = slots[(sequence - 1) % capacity_];
    if (slot.gate.test_and_set(std::memory_order_acquire)) {
        droppedRecords_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    LogRecord& record = slot.record;
    record.timestampNs = NowNs();
    record.category = static_cast<uint8_t>(category);
    record.level = static_cast<uint8_t>(level);
    const int written =
        vsnprintf(record.message, sizeof(record.message), format, args);
    record.messageLength = written < 0 ? 0
        : (written >= static_cast<int>(sizeof(record.message))
               ? static_cast<uint16_t>(sizeof(record.message) - 1)
               : static_cast<uint16_t>(written));
    record.sequence = sequence;
    slot.gate.clear(std::memory_order_release);

    const auto categoryIndex = static_cast<uint32_t>(category);
    if (categoryIndex < kLogRingCategoryCount) {
        perCategory_[categoryIndex].fetch_add(1, std::memory_order_relaxed);
    }
}

LogRingQueryResult LogRing::Query(const LogRingQuery& query,
                                  LogRecord* outRecords, uint32_t maxRecords,
                                  uint32_t scanBudget) const noexcept {
    LogRingQueryResult result{};
    LogSlot* slots = slots_.load(std::memory_order_acquire);
    if (slots == nullptr || outRecords == nullptr || maxRecords == 0) {
        return result;
    }

    const uint64_t latest = nextSequence_.load(std::memory_order_acquire);
    result.latestSequence = latest;
    if (latest == 0) {
        return result;
    }

    // The oldest retained record: writers may still be lapping, so keep one
    // slot of slack away from the live claim frontier.
    const uint64_t oldest =
        latest > capacity_ ? latest - capacity_ + 1 : 1;
    result.oldestSequence = oldest;

    uint64_t cursor = 0;
    if (query.afterSequence > latest) {
        // A controller/dext reconnect can create a fresh ring whose sequence
        // space starts below the caller's saved cursor. Replay retained
        // history instead of returning a deceptive empty page with the old
        // cursor still intact.
        result.cursorReset = true;
        cursor = oldest;
    } else {
        cursor = query.afterSequence + 1;
        if (cursor < oldest) {
            cursor = oldest;
        }
    }
    if (scanBudget == 0) {
        scanBudget = 1;
    }

    while (cursor <= latest && result.recordCount < maxRecords &&
           result.scannedCount < scanBudget) {
        LogSlot& slot = slots[(cursor - 1) % capacity_];

        ++result.scannedCount;
        if (!slot.gate.test_and_set(std::memory_order_acquire)) {
            LogRecord copy = slot.record;
            slot.gate.clear(std::memory_order_release);
            const bool intact = copy.sequence == cursor;
            if (!intact) {
                ++cursor;
                continue;
            }
            copy.message[sizeof(copy.message) - 1] = '\0';
            const bool categoryMatch =
                copy.category < kLogRingCategoryCount &&
                (query.categoryMask & (1U << copy.category)) != 0;
            if (categoryMatch && copy.level <= query.maxLevel &&
                Contains(copy.message, query.contains)) {
                outRecords[result.recordCount++] = copy;
            }
        }
        ++cursor;
    }

    result.nextSequence = cursor - 1;
    return result;
}

LogRingStats LogRing::Stats() const noexcept {
    LogRingStats stats{};
    stats.capacityRecords = capacity_;
    stats.droppedRecords = droppedRecords_.load(std::memory_order_relaxed);
    stats.latestSequence = nextSequence_.load(std::memory_order_acquire);
    stats.oldestSequence = stats.latestSequence > capacity_
        ? stats.latestSequence - capacity_ + 1
        : (stats.latestSequence == 0 ? 0 : 1);
    uint64_t total = 0;
    for (uint32_t index = 0; index < kLogRingCategoryCount; ++index) {
        stats.perCategory[index] =
            perCategory_[index].load(std::memory_order_relaxed);
        total += stats.perCategory[index];
    }
    stats.totalEmitted = total;
    return stats;
}

size_t PackLogRecords(const LogRing& ring, const LogRingQueryRequestWire& request,
                      uint8_t* outBuffer, size_t bufferCapacity) noexcept {
    if (outBuffer == nullptr ||
        bufferCapacity < sizeof(LogRingQueryResponseHeaderWire)) {
        return 0;
    }

    LogRingQuery query{};
    query.afterSequence = request.afterSequence;
    query.categoryMask = request.categoryMask;
    query.maxLevel = request.maxLevel > 255U
        ? static_cast<uint8_t>(LogLevel::Debug)
        : static_cast<uint8_t>(request.maxLevel);
    memcpy(query.contains, request.contains, sizeof(query.contains));
    query.contains[sizeof(query.contains) - 1] = '\0';

    LogRingQueryResponseHeaderWire header{};
    uint8_t* payload = outBuffer + sizeof(header);
    size_t payloadCapacity = bufferCapacity - sizeof(header);
    size_t payloadUsed = 0;

    // Drain in small batches so one call packs until the byte budget or the
    // caller's record cap is exhausted, whichever comes first.
    uint32_t remainingRecords = request.maxRecords == 0
        ? 0xFFFFFFFFU
        : request.maxRecords;
    uint32_t scanRemaining = 8192;
    bool budgetExhausted = false;
    while (remainingRecords > 0 && scanRemaining > 0 && !budgetExhausted) {
        LogRecord batch[16];
        const uint32_t want = remainingRecords < 16U ? remainingRecords : 16U;
        const LogRingQueryResult result = ring.Query(
            query, batch, want, scanRemaining);
        header.latestSequence = result.latestSequence;
        header.oldestSequence = result.oldestSequence;
        if (result.cursorReset) {
            header.flags |= kLogRingQueryFlagCursorReset;
        }
        header.scannedCount += result.scannedCount;
        scanRemaining -= result.scannedCount;

        uint32_t consumed = 0;
        for (; consumed < result.recordCount; ++consumed) {
            const LogRecord& record = batch[consumed];
            const size_t messageLength = record.messageLength;
            const size_t need = sizeof(LogRecordHeaderWire) + messageLength;
            if (payloadUsed + need > payloadCapacity) {
                budgetExhausted = true;
                break;
            }
            LogRecordHeaderWire recordHeader{};
            recordHeader.sequence = record.sequence;
            recordHeader.timestampNs = record.timestampNs;
            recordHeader.category = record.category;
            recordHeader.level = record.level;
            recordHeader.messageLength = static_cast<uint16_t>(messageLength);
            memcpy(payload + payloadUsed, &recordHeader, sizeof(recordHeader));
            memcpy(payload + payloadUsed + sizeof(recordHeader), record.message,
                   messageLength);
            payloadUsed += need;
            ++header.recordCount;
            header.nextSequence = record.sequence;
        }
        if (consumed > 0) {
            remainingRecords -= consumed;
        }
        if (!budgetExhausted) {
            // Everything the query returned fit; advance past what was
            // scanned (including filtered-out records) and stop when the
            // scan reached the newest record.
            header.nextSequence = result.nextSequence;
            query.afterSequence = result.nextSequence;
            if (result.nextSequence >= result.latestSequence ||
                result.scannedCount == 0) {
                break;
            }
        }
    }

    header.payloadBytes = static_cast<uint32_t>(payloadUsed);
    memcpy(outBuffer, &header, sizeof(header));
    return sizeof(header) + payloadUsed;
}

void PackLogStats(const LogRing& ring, LogRingStatsWire& outStats) noexcept {
    const LogRingStats stats = ring.Stats();
    outStats = LogRingStatsWire{};
    outStats.totalEmitted = stats.totalEmitted;
    outStats.droppedRecords = stats.droppedRecords;
    outStats.latestSequence = stats.latestSequence;
    outStats.oldestSequence = stats.oldestSequence;
    outStats.capacityRecords = stats.capacityRecords;
    outStats.categoryCount = kLogRingCategoryCount;
    for (uint32_t index = 0; index < kLogRingCategoryCount; ++index) {
        outStats.perCategory[index] = stats.perCategory[index];
    }
}

void SanitizeOsLogFormat(const char* input, char* output,
                         size_t outputSize) noexcept {
    if (output == nullptr || outputSize == 0) {
        return;
    }
    size_t out = 0;
    for (size_t in = 0; input != nullptr && input[in] != '\0' &&
                        out + 1 < outputSize; ++in) {
        output[out++] = input[in];
        if (input[in] == '%' && input[in + 1] == '{') {
            in += 2; // skip '{'
            while (input[in] != '\0' && input[in] != '}') {
                ++in;
            }
            if (input[in] == '\0') {
                break; // malformed annotation: keep what we have
            }
            // loop increment steps past '}'
        }
    }
    output[out] = '\0';
}

void RingLogTo(LogRing& ring, LogCategory category, LogLevel level,
               const char* osLogFormat, ...) noexcept {
    if (!ring.IsInitialized() || osLogFormat == nullptr) {
        return;
    }
    char sanitized[256];
    SanitizeOsLogFormat(osLogFormat, sanitized, sizeof(sanitized));
    va_list args;
    va_start(args, osLogFormat);
    ring.Append(category, level, sanitized, args);
    va_end(args);
}

void RingLog(LogCategory category, LogLevel level, const char* osLogFormat,
             ...) noexcept {
    LogRing& ring = LogRing::Shared();
    if (!ring.IsInitialized() || osLogFormat == nullptr) {
        return;
    }
    char sanitized[256];
    SanitizeOsLogFormat(osLogFormat, sanitized, sizeof(sanitized));
    va_list args;
    va_start(args, osLogFormat);
    ring.Append(category, level, sanitized, args);
    va_end(args);
}

LogRing* LogRing::CreateForTest(uint32_t capacityRecords) noexcept {
    auto* ring = new (std::nothrow) LogRing();
    if (ring != nullptr && !ring->InitializeWithCapacity(capacityRecords)) {
        delete ring;
        return nullptr;
    }
    return ring;
}

void LogRing::DestroyForTest(LogRing* ring) noexcept {
    if (ring != nullptr) {
        delete[] ring->slots_.exchange(nullptr, std::memory_order_acq_rel);
        delete ring;
    }
}

} // namespace ASFW::Logging
