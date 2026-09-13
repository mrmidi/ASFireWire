//
// MidiByteStreamToUmp.cpp
// ASFWDriver
//

#include "MidiByteStreamToUmp.hpp"

namespace ASFW::Midi::Ump {

void MidiByteStreamToUmp::Reset() noexcept {
    const bool hadPartial =
        pendingCount_ != 0 || pendingRemaining_ != 0 || sysExActive_;
    if (hadPartial) ++counters_.partialMessagesDiscarded;

    pendingStatus_ = 0;
    pendingRemaining_ = 0;
    pendingCount_ = 0;
    pendingData_[0] = 0;
    pendingData_[1] = 0;
    runningStatus_ = 0;
    sysExActive_ = false;
    sysExAnyEmitted_ = false;
    sysExCount_ = 0;
}

void MidiByteStreamToUmp::FlushSysEx(SysExStatus status,
                                     std::span<UmpWord> out,
                                     uint32_t& written) noexcept {
    const uint8_t n = sysExCount_;
    out[written++] = MakeSysEx7Word0(
        group_, status, n,
        n > 0 ? sysExBuf_[0] : 0,
        n > 1 ? sysExBuf_[1] : 0);
    out[written++] = MakeSysEx7Word1(
        n > 2 ? sysExBuf_[2] : 0,
        n > 3 ? sysExBuf_[3] : 0,
        n > 4 ? sysExBuf_[4] : 0,
        n > 5 ? sysExBuf_[5] : 0);
    sysExCount_ = 0;
    sysExAnyEmitted_ = true;
}

void MidiByteStreamToUmp::TerminateSysEx(std::span<UmpWord> out,
                                         uint32_t& written) noexcept {
    if (!sysExActive_) return;
    ++counters_.sysExTruncated;
    FlushSysEx(sysExAnyEmitted_ ? SysExStatus::kEnd : SysExStatus::kComplete,
               out, written);
    sysExActive_ = false;
    sysExAnyEmitted_ = false;
}

void MidiByteStreamToUmp::HandleStatusByte(uint8_t byte,
                                           std::span<UmpWord> out,
                                           uint32_t& written) noexcept {
    // System Real Time may arrive between any two bytes of another message,
    // including inside a System Exclusive, and disturbs neither the partial
    // message nor running status.
    if (IsSystemRealTime(byte)) {
        out[written++] = MakeSystem(group_, byte, 0, 0);
        return;
    }

    if (byte == kSysExStart) {
        // A new System Exclusive while one is open: the previous one is
        // unterminated.
        TerminateSysEx(out, written);
        sysExActive_ = true;
        sysExAnyEmitted_ = false;
        sysExCount_ = 0;
        // System Common clears running status, and abandons any partial
        // channel message.
        runningStatus_ = 0;
        pendingStatus_ = 0;
        pendingRemaining_ = 0;
        pendingCount_ = 0;
        return;
    }

    if (byte == kSysExEnd) {
        if (sysExActive_) {
            FlushSysEx(
                sysExAnyEmitted_ ? SysExStatus::kEnd : SysExStatus::kComplete,
                out, written);
            sysExActive_ = false;
            sysExAnyEmitted_ = false;
        } else {
            // 0xF7 with no System Exclusive open carries no message.
            ++counters_.unsupportedStatusBytes;
        }
        runningStatus_ = 0;
        pendingStatus_ = 0;
        pendingRemaining_ = 0;
        pendingCount_ = 0;
        return;
    }

    // Any other status byte ends an open System Exclusive.
    TerminateSysEx(out, written);

    if (IsChannelStatus(byte)) {
        pendingStatus_ = byte;
        pendingRemaining_ = ChannelMessageDataBytes(byte);
        pendingCount_ = 0;
        runningStatus_ = byte;
        return;
    }

    // Remaining range is System Common 0xF1-0xF6.
    const uint8_t dataBytes = SystemCommonDataBytes(byte);
    runningStatus_ = 0;
    if (dataBytes == 0xFF) {
        // 0xF4 and 0xF5 are undefined in MIDI 1.0. Forwarding an undefined
        // status to an arbitrary device is exactly what this converter is
        // supposed to avoid, so drop and count.
        ++counters_.unsupportedStatusBytes;
        pendingStatus_ = 0;
        pendingRemaining_ = 0;
        pendingCount_ = 0;
        return;
    }
    if (dataBytes == 0) {
        out[written++] = MakeSystem(group_, byte, 0, 0);
        pendingStatus_ = 0;
        pendingRemaining_ = 0;
        pendingCount_ = 0;
        return;
    }
    pendingStatus_ = byte;
    pendingRemaining_ = dataBytes;
    pendingCount_ = 0;
}

void MidiByteStreamToUmp::HandleDataByte(uint8_t byte,
                                         std::span<UmpWord> out,
                                         uint32_t& written) noexcept {
    if (sysExActive_) {
        if (sysExCount_ == kMaxSysEx7PayloadBytes) {
            FlushSysEx(sysExAnyEmitted_ ? SysExStatus::kContinue
                                        : SysExStatus::kStart,
                       out, written);
        }
        sysExBuf_[sysExCount_++] = byte;
        return;
    }

    if (pendingRemaining_ == 0) {
        // Not mid-message. Running status lets a Channel Voice message repeat
        // without resending its status byte.
        if (runningStatus_ == 0) {
            ++counters_.orphanDataBytes;
            return;
        }
        pendingStatus_ = runningStatus_;
        pendingRemaining_ = ChannelMessageDataBytes(runningStatus_);
        pendingCount_ = 0;
    }

    pendingData_[pendingCount_++] = byte;
    if (--pendingRemaining_ != 0) return;

    const uint8_t status = pendingStatus_;
    const uint8_t d0 = pendingCount_ > 0 ? pendingData_[0] : 0;
    const uint8_t d1 = pendingCount_ > 1 ? pendingData_[1] : 0;
    pendingCount_ = 0;

    if (IsChannelStatus(status)) {
        out[written++] = MakeMidi1ChannelVoice(
            group_, static_cast<uint8_t>(status >> 4),
            static_cast<uint8_t>(status & 0x0F), d0, d1);
        // pendingStatus_ stays set only through runningStatus_; the message
        // itself is complete.
        pendingStatus_ = 0;
        return;
    }

    // System Common with data bytes: 0xF1, 0xF2, 0xF3.
    out[written++] = MakeSystem(group_, status, d0, d1);
    pendingStatus_ = 0;
}

MidiByteStreamToUmp::PushResult MidiByteStreamToUmp::Push(
    std::span<const uint8_t> bytes, std::span<UmpWord> out) noexcept {
    PushResult result{};
    uint32_t written = 0;

    for (const uint8_t byte : bytes) {
        // Stop before a byte that could need more words than remain free, so
        // no message is ever split across calls and bounds are never exceeded.
        // An 0xF6 (Tune Request) byte during an active System Exclusive flushes
        // the SysEx (2 words) and emits the Tune Request (1 word), needing 3 words.
        // Every other byte needs at most 2 words.
        const uint32_t neededWords = (sysExActive_ && byte == 0xF6) ? 3 : 2;
        if (out.size() - written < neededWords) break;

        if (IsStatusByte(byte)) {
            HandleStatusByte(byte, out, written);
        } else {
            HandleDataByte(byte, out, written);
        }
        ++result.bytesConsumed;
    }

    result.wordsWritten = written;
    counters_.bytesConsumed += result.bytesConsumed;
    counters_.wordsEmitted += written;
    return result;
}

} // namespace ASFW::Midi::Ump
