//
// UmpToMidiByteStream.cpp
// ASFWDriver
//

#include "UmpToMidiByteStream.hpp"

namespace ASFW::Midi::Ump {

namespace {

/// A UMP field that must hold a MIDI 1.0 data byte.
///
/// Bit 7 set would become a status byte on the wire and desynchronise the
/// device's parser, so an out-of-range field rejects the whole packet rather
/// than being masked into something plausible.
[[nodiscard]] constexpr bool IsSevenBit(uint8_t value) noexcept {
    return (value & 0x80) == 0;
}

[[nodiscard]] constexpr uint8_t ByteAt(UmpWord word, unsigned shift) noexcept {
    return static_cast<uint8_t>((word >> shift) & 0xFF);
}

} // namespace

void UmpToMidiByteStream::Reset() noexcept {
    sysExActive_ = false;
}

void UmpToMidiByteStream::TranslateSysEx7(UmpWord word0, UmpWord word1,
                                          std::span<uint8_t> out,
                                          uint32_t& written) noexcept {
    const auto status = static_cast<SysExStatus>((word0 >> 20) & 0x0F);
    const auto numBytes = static_cast<uint8_t>((word0 >> 16) & 0x0F);

    if (numBytes > kMaxSysEx7PayloadBytes) {
        ++counters_.malformedPackets;
        return;
    }

    const uint8_t payload[kMaxSysEx7PayloadBytes] = {
        ByteAt(word0, 8), ByteAt(word0, 0),
        ByteAt(word1, 24), ByteAt(word1, 16),
        ByteAt(word1, 8),  ByteAt(word1, 0),
    };
    for (uint8_t i = 0; i < numBytes; ++i) {
        if (!IsSevenBit(payload[i])) {
            ++counters_.malformedPackets;
            return;
        }
    }

    switch (status) {
        case SysExStatus::kComplete:
        case SysExStatus::kStart:
            // Starting while one is open abandons the previous message. A
            // device reading 0xF0 mid-System-Exclusive restarts too, so
            // emitting is the recovery; the counter records that it happened.
            if (sysExActive_) ++counters_.sysExInterrupted;
            out[written++] = kSysExStart;
            break;
        case SysExStatus::kContinue:
        case SysExStatus::kEnd:
            if (!sysExActive_) {
                // Bare payload with no 0xF0 would be read as data bytes under
                // whatever running status the device last saw. Drop it.
                ++counters_.sysExOutOfSequence;
                return;
            }
            break;
        default:
            // Mixed Data Set headers and payloads are MIDI 2.0 only.
            ++counters_.malformedPackets;
            return;
    }

    for (uint8_t i = 0; i < numBytes; ++i) out[written++] = payload[i];

    if (status == SysExStatus::kComplete || status == SysExStatus::kEnd) {
        out[written++] = kSysExEnd;
        sysExActive_ = false;
    } else {
        sysExActive_ = true;
    }
}

void UmpToMidiByteStream::TranslatePacket(uint8_t nibble, UmpWord word0,
                                          UmpWord word1,
                                          std::span<uint8_t> out,
                                          uint32_t& written) noexcept {
    switch (static_cast<MessageType>(nibble)) {
        case MessageType::kUtility:
            // NOOP, jitter-reduction clock and timestamps, delta clockstamps.
            // Transport metadata with no DIN byte equivalent. Declaring
            // MIDIProtocol_1_0 does not stop a client sending these, so count
            // and skip rather than assume they cannot arrive.
            ++counters_.utilityPacketsIgnored;
            return;

        case MessageType::kSystem: {
            const uint8_t status = ByteAt(word0, 16);
            const uint8_t b1 = ByteAt(word0, 8);
            const uint8_t b2 = ByteAt(word0, 0);

            if (IsSystemRealTime(status)) {
                out[written++] = status;
                return;
            }
            const uint8_t dataBytes = SystemCommonDataBytes(status);
            if (dataBytes == 0xFF) {
                // 0xF0 and 0xF7 belong to the SysEx7 path, 0xF4 and 0xF5 are
                // undefined in MIDI 1.0, and anything below 0xF0 is not a
                // System message at all.
                ++counters_.malformedPackets;
                return;
            }
            if ((dataBytes >= 1 && !IsSevenBit(b1)) ||
                (dataBytes >= 2 && !IsSevenBit(b2))) {
                ++counters_.malformedPackets;
                return;
            }
            out[written++] = status;
            if (dataBytes >= 1) out[written++] = b1;
            if (dataBytes >= 2) out[written++] = b2;
            return;
        }

        case MessageType::kMidi1ChannelVoice: {
            const auto statusNibble = static_cast<uint8_t>((word0 >> 20) & 0x0F);
            const auto channel = static_cast<uint8_t>((word0 >> 16) & 0x0F);
            const uint8_t d1 = ByteAt(word0, 8);
            const uint8_t d2 = ByteAt(word0, 0);

            if (statusNibble < 0x8 || statusNibble > 0xE) {
                // 0xF as a Channel Voice status nibble would encode a System
                // message, which belongs to MT 0x1.
                ++counters_.malformedPackets;
                return;
            }
            const auto status =
                static_cast<uint8_t>((statusNibble << 4) | channel);
            const uint8_t dataBytes = ChannelMessageDataBytes(status);
            if (!IsSevenBit(d1) || (dataBytes >= 2 && !IsSevenBit(d2))) {
                ++counters_.malformedPackets;
                return;
            }
            out[written++] = status;
            out[written++] = d1;
            if (dataBytes >= 2) out[written++] = d2;
            return;
        }

        case MessageType::kData64:
            TranslateSysEx7(word0, word1, out, written);
            return;

        default:
            // MIDI 2.0 Channel Voice, Data128, Flex Data, UMP Stream and the
            // undefined types. Skipped by exact packet length, never
            // reinterpreted as shorter messages.
            ++counters_.unsupportedMessageType;
            return;
    }
}

UmpToMidiByteStream::PullResult UmpToMidiByteStream::Pull(
    std::span<const UmpWord> words, std::span<uint8_t> out) noexcept {
    PullResult result{};
    uint32_t written = 0;
    uint32_t consumed = 0;

    while (consumed < words.size()) {
        const UmpWord word0 = words[consumed];
        const uint8_t nibble = MessageTypeNibble(word0);
        const uint8_t packetWords = UmpPacketWords(nibble);

        if (words.size() - consumed < packetWords) {
            // Never read past the end of a truncated multi-word packet, and
            // never treat its tail as a new message.
            result.needsMoreWords = true;
            break;
        }
        // Reserve the worst-case expansion so a packet is either fully written
        // or not consumed at all.
        if (out.size() - written < kMaxBytesPerPacket) break;

        if (IsGroupScoped(nibble) && GroupOf(word0) != group_) {
            ++counters_.foreignGroupPackets;
        } else {
            const UmpWord word1 = packetWords > 1 ? words[consumed + 1] : 0;
            TranslatePacket(nibble, word0, word1, out, written);
        }
        consumed += packetWords;
    }

    result.wordsConsumed = consumed;
    result.bytesWritten = written;
    counters_.wordsConsumed += consumed;
    counters_.bytesEmitted += written;
    return result;
}

} // namespace ASFW::Midi::Ump
