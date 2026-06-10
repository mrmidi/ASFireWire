#pragma once

#include <cstddef>
#include <cstdint>

namespace ASFW::Lab {

// Trace replay closes the pacing gap (README, "Key design decisions"):
// record real (sample_time, host_time, frame_count) sequences once — from
// the lab dext, later from bench ASFW — then replay them forever on host so
// regression tests run against genuine coreaudiod scheduling.
//
// Trace text format, one WriteEnd per line:
//
//     <sample_time> <host_time> <frame_count>
//
// '#' starts a comment, blank lines are skipped, malformed lines are counted
// and skipped (never silently absorbed). Parsing is allocation-free into a
// caller-provided buffer so the replayer stays dext-safe like the rest of
// Lab/ (no iostream, no heap).

struct WriteEndEvent final {
    uint64_t sampleTime{0};
    uint64_t hostTime{0};
    uint32_t frameCount{0};
};

struct TraceParseResult final {
    size_t eventCount{0};
    size_t malformedLines{0};
    bool truncated{false}; // capacity exhausted before the text ended
};

class WriteEndTraceReplayer final {
public:
    static TraceParseResult ParseTraceText(const char* text, size_t length,
                                           WriteEndEvent* outEvents,
                                           size_t capacity) noexcept {
        TraceParseResult result{};
        size_t pos = 0;
        while (pos < length) {
            // Isolate the line [lineStart, lineEnd).
            const size_t lineStart = pos;
            while (pos < length && text[pos] != '\n') {
                ++pos;
            }
            size_t lineEnd = pos;
            if (pos < length) {
                ++pos; // consume the newline
            }

            // Strip comment and trailing/leading whitespace.
            size_t cursor = lineStart;
            size_t contentEnd = lineEnd;
            for (size_t i = lineStart; i < lineEnd; ++i) {
                if (text[i] == '#') {
                    contentEnd = i;
                    break;
                }
            }
            while (cursor < contentEnd && IsSpace(text[cursor])) {
                ++cursor;
            }
            while (contentEnd > cursor && IsSpace(text[contentEnd - 1])) {
                --contentEnd;
            }
            if (cursor == contentEnd) {
                continue; // blank or comment-only line
            }

            WriteEndEvent event{};
            uint64_t frames = 0;
            const bool ok = ParseU64(text, cursor, contentEnd, event.sampleTime) &&
                            ParseU64(text, cursor, contentEnd, event.hostTime) &&
                            ParseU64(text, cursor, contentEnd, frames) &&
                            AllSpace(text, cursor, contentEnd) &&
                            frames <= 0xFFFFFFFFull;
            if (!ok) {
                ++result.malformedLines;
                continue;
            }
            event.frameCount = static_cast<uint32_t>(frames);

            if (result.eventCount == capacity) {
                result.truncated = true;
                return result;
            }
            outEvents[result.eventCount++] = event;
        }
        return result;
    }

    // Replays events through any callable taking (const WriteEndEvent&).
    template <typename Callback>
    static void Replay(const WriteEndEvent* events, size_t count,
                       Callback&& callback) {
        for (size_t i = 0; i < count; ++i) {
            callback(events[i]);
        }
    }

private:
    static bool IsSpace(char c) noexcept {
        return c == ' ' || c == '\t' || c == '\r';
    }

    static bool AllSpace(const char* text, size_t from, size_t to) noexcept {
        for (size_t i = from; i < to; ++i) {
            if (!IsSpace(text[i])) {
                return false;
            }
        }
        return true;
    }

    // Parses one base-10 u64 starting at `cursor` (after skipping spaces),
    // advancing `cursor` past it. Overflow-checked.
    static bool ParseU64(const char* text, size_t& cursor, size_t end,
                         uint64_t& out) noexcept {
        while (cursor < end && IsSpace(text[cursor])) {
            ++cursor;
        }
        if (cursor == end || text[cursor] < '0' || text[cursor] > '9') {
            return false;
        }
        uint64_t value = 0;
        while (cursor < end && text[cursor] >= '0' && text[cursor] <= '9') {
            const uint64_t digit = static_cast<uint64_t>(text[cursor] - '0');
            if (value > (0xFFFFFFFFFFFFFFFFull - digit) / 10ull) {
                return false; // overflow
            }
            value = value * 10ull + digit;
            ++cursor;
        }
        out = value;
        return true;
    }
};

} // namespace ASFW::Lab
