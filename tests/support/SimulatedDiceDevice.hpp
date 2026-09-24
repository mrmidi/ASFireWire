// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SimulatedDiceDevice.hpp - A host-side DICE device for characterization tests.
//
// The device holds the whole DICE general space as bytes, built from a recorded
// register image (DiceDeviceImage.hpp), and answers reads, writes and 64-bit
// compare-swap at the image's real section offsets. Writes to the registers a
// host drives (CLOCK_SELECT, ENABLE, per-stream ISOCHRONOUS/SPEED/SEQ_START)
// have the side effects a DICE firmware shows; everything else is plain memory.
//
// Behaviour is modelled only as far as the evidence goes, and each modelled
// behaviour cites it. Fault knobs let tests reproduce known field failures.
// An invariant observer records (never asserts) host behaviour that has wedged
// real hardware, so a characterization trace shows it.
//
// This file knows nothing about the bus or the notification mailbox: the bus
// wrapper (DICEDuplexTestSupport.hpp) owns generations, ops and delivery.

#pragma once

#include "DiceDeviceImage.hpp"

#include "Audio/Protocols/DICE/Core/DICETypes.hpp"
#include "Common/WireFormat.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ASFW::Testing::DICE {

inline constexpr uint32_t kDiceBaseAddressLo = 0xE0000000U;
inline constexpr uint16_t kDiceBaseAddressHi = 0xFFFF;
inline constexpr uint64_t kDiceNoOwner = 0xFFFF000000000000ULL;

// DICE rate index (CLOCK_SELECT[15:8], STATUS[15:8]) to Hz.
inline constexpr std::array<uint32_t, 7> kDiceRateHz{
    32000, 44100, 48000, 88200, 96000, 176400, 192000};

// Rate mode for a rate index: 0 = low (32-48k), 1 = middle, 2 = high.
[[nodiscard]] constexpr uint32_t DiceRateModeForIndex(uint32_t rateIndex) noexcept {
    return rateIndex <= 2 ? 0U : (rateIndex <= 4 ? 1U : 2U);
}

// What the device does after a CLOCK_SELECT write.
enum class DiceClockResponse {
    kAcceptAndLock,   // CLOCK_ACCEPTED, then locked at the requested rate (default)
    kAcceptNeverLock, // CLOCK_ACCEPTED, but STATUS stays unlocked
    kNeverAccept,     // no notification, STATUS unchanged
    kAcceptLater,     // held until DeliverPendingClockChange()
};

struct SimulatedDiceOptions {
    DiceClockResponse clockResponse{DiceClockResponse::kAcceptAndLock};
    // Deliver CLOCK_ACCEPTED inside the CLOCK_SELECT write, before the write
    // completes. This is what the legacy test bus did
    // (RecordingFireWireBus::ApplyWrite) and existing tests depend on it.
    bool publishClockAcceptedDuringWrite{true};
    // DICE firmware disables streaming on a bus reset (Linux
    // dice-stream.c:587-607, "the DICE firmware disables streaming").
    bool busResetClearsEnable{true};
    // TCAT re-claims ownership on every new generation
    // (MidasFW PopulateGlobalDeviceStruct 0xc25e -> GetOwnership 0xc624, with a
    // CAS that expects no owner), which implies the firmware drops the owner on
    // reset. Unverified on hardware, so it is an option.
    bool busResetClearsOwner{true};
};

class SimulatedDiceDevice final {
public:
    using ClockResponse = DiceClockResponse;
    using Options = SimulatedDiceOptions;
    using NotifySink = std::function<void(uint32_t bits)>;
    using TraceSink = std::function<void(std::string_view line)>;

    explicit SimulatedDiceDevice(const DiceDeviceImage& image, Options options = {})
        : image_(image), options_(options) {
        BuildMemory();
    }

    // ---- wiring -----------------------------------------------------------

    void SetNotifySink(NotifySink sink) { notify_ = std::move(sink); }
    void SetTraceSink(TraceSink sink) { trace_ = std::move(sink); }
    void SetOptions(Options options) { options_ = options; }
    [[nodiscard]] const Options& GetOptions() const noexcept { return options_; }
    [[nodiscard]] const DiceDeviceImage& Image() const noexcept { return image_; }

    // ---- bus-facing transactions (addressLo relative to 0xFFFF'0000'0000) ---

    // Returns nullopt for an address outside the modelled general space.
    [[nodiscard]] std::optional<std::vector<uint8_t>> Read(uint32_t addressLo,
                                                           uint32_t length) const {
        const auto offset = Offset(addressLo, length);
        if (!offset) {
            return std::nullopt;
        }
        return std::vector<uint8_t>(memory_.begin() + *offset,
                                    memory_.begin() + *offset + length);
    }

    // Returns false for an address outside the modelled general space.
    bool Write(uint32_t addressLo, std::span<const uint8_t> data) {
        const auto offset = Offset(addressLo, static_cast<uint32_t>(data.size()));
        if (!offset) {
            return false;
        }
        const bool quadlet = data.size() == 4;
        const uint32_t value = quadlet ? ::ASFW::FW::ReadBE32(data.data()) : 0;
        if (quadlet) {
            ObserveWrite(*offset, value);
        }
        std::copy(data.begin(), data.end(), memory_.begin() + *offset);
        if (quadlet) {
            ApplyWriteSideEffects(*offset, value);
        }
        return true;
    }

    // 64-bit compare-swap. Returns the previous value, or nullopt when the
    // address is not an octlet inside the modelled space.
    std::optional<uint64_t> CompareSwap64(uint32_t addressLo, uint64_t expected,
                                          uint64_t desired) {
        const auto offset = Offset(addressLo, 8);
        if (!offset) {
            return std::nullopt;
        }
        const uint64_t previous = ::ASFW::FW::ReadBE64(memory_.data() + *offset);
        if (previous == expected) {
            ::ASFW::FW::WriteBE64(memory_.data() + *offset, desired);
        }
        return previous;
    }

    // ---- scenario controls ------------------------------------------------

    // Put the device in the state it has on a fresh bus with no host: no owner,
    // not enabled, every stream disarmed, no pending notification. The images
    // are captures; the Saffire's was taken while streaming.
    void ResetToIdle() {
        SetGlobal64(GlobalField(::ASFW::Audio::DICE::GlobalOffset::kOwnerHi), kDiceNoOwner);
        SetGlobal(::ASFW::Audio::DICE::GlobalOffset::kNotification, 0);
        SetGlobal(::ASFW::Audio::DICE::GlobalOffset::kEnable, 0);
        for (uint32_t i = 0; i < TxCount(); ++i) {
            SetQuad(TxEntryBase(i) + kTxIso, 0xFFFFFFFFU);
        }
        for (uint32_t i = 0; i < RxCount(); ++i) {
            SetQuad(RxEntryBase(i) + kRxIso, 0xFFFFFFFFU);
        }
        pendingClockSelect_.reset();
    }

    // A bus reset as the device sees it. The bus wrapper bumps the generation.
    void BusReset() {
        if (options_.busResetClearsEnable) {
            SetGlobal(::ASFW::Audio::DICE::GlobalOffset::kEnable, 0);
        }
        if (options_.busResetClearsOwner) {
            SetGlobal64(GlobalField(::ASFW::Audio::DICE::GlobalOffset::kOwnerHi), kDiceNoOwner);
        }
        Trace("# device: bus reset");
    }

    void SetOwner(uint64_t owner) {
        SetGlobal64(GlobalField(::ASFW::Audio::DICE::GlobalOffset::kOwnerHi), owner);
    }

    // Set what the device reports it achieved, independently of CLOCK_SELECT
    // (DICE_STABILITY_REGRESSION.md §3: requested and achieved rate can differ).
    void SetAchievedClock(uint32_t rateIndex, bool locked) {
        SetGlobal(::ASFW::Audio::DICE::GlobalOffset::kStatus, StatusWord(rateIndex, locked));
        SetGlobal(::ASFW::Audio::DICE::GlobalOffset::kSampleRate, RateHz(rateIndex));
    }

    void SetNotificationRegister(uint32_t bits) {
        SetGlobal(::ASFW::Audio::DICE::GlobalOffset::kNotification, bits);
    }

    // Raise a notification as the firmware would: latch it into the
    // NOTIFICATION register and deliver it to the host's handler.
    void RaiseNotification(uint32_t bits) {
        SetNotificationRegister(bits);
        Deliver(bits);
    }

    // Apply a CLOCK_SELECT held back by ClockResponse::kAcceptLater.
    bool DeliverPendingClockChange() {
        if (!pendingClockSelect_) {
            return false;
        }
        const uint32_t value = *pendingClockSelect_;
        pendingClockSelect_.reset();
        AcceptClock(value, /*publish=*/true);
        return true;
    }

    // ---- direct inspection ------------------------------------------------

    [[nodiscard]] uint32_t GlobalQuad(uint32_t globalOffset) const {
        return ::ASFW::FW::ReadBE32(memory_.data() + GlobalField(globalOffset));
    }
    [[nodiscard]] uint64_t Owner() const {
        return ::ASFW::FW::ReadBE64(
            memory_.data() + GlobalField(::ASFW::Audio::DICE::GlobalOffset::kOwnerHi));
    }
    [[nodiscard]] uint32_t Enable() const {
        return GlobalQuad(::ASFW::Audio::DICE::GlobalOffset::kEnable);
    }
    [[nodiscard]] uint32_t TxCount() const { return Quad(TxSectionBase()); }
    [[nodiscard]] uint32_t RxCount() const { return Quad(RxSectionBase()); }
    [[nodiscard]] int32_t TxIso(uint32_t i) const {
        return static_cast<int32_t>(Quad(TxEntryBase(i) + kTxIso));
    }
    [[nodiscard]] int32_t RxIso(uint32_t i) const {
        return static_cast<int32_t>(Quad(RxEntryBase(i) + kRxIso));
    }
    [[nodiscard]] uint32_t TxSpeed(uint32_t i) const { return Quad(TxEntryBase(i) + kTxSpeed); }
    [[nodiscard]] uint32_t RxSeqStart(uint32_t i) const { return Quad(RxEntryBase(i) + kRxSeqStart); }
    [[nodiscard]] const std::vector<std::string>& Violations() const noexcept { return violations_; }

    // Raw register setters for tests that stage a specific device state. These
    // bypass side effects and the invariant observer.
    void SetGlobalQuad(uint32_t globalOffset, uint32_t value) { SetGlobal(globalOffset, value); }
    void SetTxIso(uint32_t i, uint32_t value) { SetQuad(TxEntryBase(i) + kTxIso, value); }
    void SetRxIso(uint32_t i, uint32_t value) { SetQuad(RxEntryBase(i) + kRxIso, value); }

    // Byte offsets (relative to the DICE base) for tests that address registers.
    [[nodiscard]] uint32_t GlobalBase() const { return image_.globalSection.offsetQuadlets * 4; }
    [[nodiscard]] uint32_t TxSectionBase() const { return image_.txSection.offsetQuadlets * 4; }
    [[nodiscard]] uint32_t RxSectionBase() const { return image_.rxSection.offsetQuadlets * 4; }
    [[nodiscard]] uint32_t TxEntryBase(uint32_t i) const {
        return TxSectionBase() + kStreamHeaderBytes + i * image_.txEntryQuadlets * 4;
    }
    [[nodiscard]] uint32_t RxEntryBase(uint32_t i) const {
        return RxSectionBase() + kStreamHeaderBytes + i * image_.rxEntryQuadlets * 4;
    }

private:
    // Per-stream layout (Linux dice-interface.h TX_*/RX_* minus the 8-byte
    // NUMBER/SIZE header; matches DICETypes TxOffset/RxOffset).
    static constexpr uint32_t kStreamHeaderBytes = 8;
    static constexpr uint32_t kTxIso = 0x00, kTxAudio = 0x04, kTxMidi = 0x08, kTxSpeed = 0x0C;
    static constexpr uint32_t kRxIso = 0x00, kRxSeqStart = 0x04, kRxAudio = 0x08, kRxMidi = 0x0C;
    static constexpr uint32_t kNamesOffset = 0x10;
    static constexpr uint32_t kNamesBytes = 256;
    static constexpr uint32_t kSourceNamesBytes = 256;
    static constexpr uint32_t kNicknameBytes = 64;
    // EXT_SYNC (Linux dice-interface.h EXT_SYNC_*).
    static constexpr uint32_t kExtSyncClockSource = 0x00, kExtSyncLocked = 0x04,
                              kExtSyncRate = 0x08, kExtSyncAdatUserData = 0x0C;

    static constexpr uint32_t RateHz(uint32_t rateIndex) {
        return rateIndex < kDiceRateHz.size() ? kDiceRateHz[rateIndex] : 0;
    }

    static constexpr uint32_t StatusWord(uint32_t rateIndex, bool locked) {
        return (locked ? ::ASFW::Audio::DICE::StatusBits::kSourceLocked : 0U) |
               (rateIndex << ::ASFW::Audio::DICE::StatusBits::kNominalRateShift);
    }

    [[nodiscard]] std::optional<uint32_t> Offset(uint32_t addressLo, uint32_t length) const {
        if (addressLo < kDiceBaseAddressLo) {
            return std::nullopt;
        }
        const uint32_t offset = addressLo - kDiceBaseAddressLo;
        if (length == 0 || offset + length > memory_.size()) {
            return std::nullopt;
        }
        return offset;
    }

    [[nodiscard]] uint32_t GlobalField(uint32_t field) const { return GlobalBase() + field; }
    [[nodiscard]] uint32_t Quad(uint32_t offset) const {
        return ::ASFW::FW::ReadBE32(memory_.data() + offset);
    }
    void SetQuad(uint32_t offset, uint32_t value) {
        ::ASFW::FW::WriteBE32(memory_.data() + offset, value);
    }
    void SetGlobal(uint32_t field, uint32_t value) { SetQuad(GlobalField(field), value); }
    void SetGlobal64(uint32_t offset, uint64_t value) {
        ::ASFW::FW::WriteBE64(memory_.data() + offset, value);
    }

    // DICE text is little-endian within each big-endian quadlet (DICETypes
    // DecodeDiceNickname; FFADO dice_avdevice.cpp "strings are little-endian").
    void PutText(uint32_t offset, uint32_t capacity, std::string_view text) {
        std::fill(memory_.begin() + offset, memory_.begin() + offset + capacity, uint8_t{0});
        const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(text.size()), capacity);
        for (uint32_t j = 0; j < n; ++j) {
            memory_[offset + (j & ~3U) + (3U - (j & 3U))] = static_cast<uint8_t>(text[j]);
        }
    }

    void BuildMemory() {
        // Cover every section and every stream entry: a section table may
        // declare a size smaller than its entries need (the legacy test layout
        // does), and a read there must still find the entry.
        uint32_t end = 0x28;
        for (const auto* s : {&image_.globalSection, &image_.txSection, &image_.rxSection,
                              &image_.extSyncSection}) {
            end = std::max(end, (s->offsetQuadlets + s->sizeQuadlets) * 4);
        }
        end = std::max(end, TxEntryBase(image_.txCount));
        end = std::max(end, RxEntryBase(image_.rxCount));
        memory_.assign(end, 0);

        // Section table: five (offset, size) quadlet pairs at the base.
        const DiceSectionImage table[5] = {image_.globalSection, image_.txSection,
                                           image_.rxSection, image_.extSyncSection, {}};
        for (uint32_t i = 0; i < 5; ++i) {
            SetQuad(i * 8, table[i].offsetQuadlets);
            SetQuad(i * 8 + 4, table[i].sizeQuadlets);
        }

        namespace G = ::ASFW::Audio::DICE::GlobalOffset;
        SetGlobal64(GlobalField(G::kOwnerHi), image_.owner);
        SetGlobal(G::kNotification, image_.notification);
        PutText(GlobalField(G::kNickname), kNicknameBytes, image_.nickname);
        SetGlobal(G::kClockSelect, image_.clockSelect);
        SetGlobal(G::kEnable, image_.enable);
        SetGlobal(G::kStatus, image_.status);
        SetGlobal(G::kExtStatus, image_.extStatus);
        SetGlobal(G::kSampleRate, image_.sampleRate);
        SetGlobal(G::kVersion, image_.version);
        SetGlobal(G::kClockCaps, image_.clockCaps);
        if (image_.globalSection.sizeQuadlets * 4 >= G::kClockSourceNames + kSourceNamesBytes) {
            PutText(GlobalField(G::kClockSourceNames), kSourceNamesBytes, image_.clockSourceNames);
        }

        SetQuad(TxSectionBase(), image_.txCount);
        SetQuad(TxSectionBase() + 4, image_.txEntryQuadlets);
        for (uint32_t i = 0; i < image_.txCount; ++i) {
            const auto& s = image_.tx[i];
            SetQuad(TxEntryBase(i) + kTxIso, static_cast<uint32_t>(s.iso));
            SetQuad(TxEntryBase(i) + kTxAudio, s.pcm);
            SetQuad(TxEntryBase(i) + kTxMidi, s.midi);
            SetQuad(TxEntryBase(i) + kTxSpeed, s.speedOrSeqStart);
            PutText(TxEntryBase(i) + kNamesOffset, kNamesBytes, s.names);
        }
        SetQuad(RxSectionBase(), image_.rxCount);
        SetQuad(RxSectionBase() + 4, image_.rxEntryQuadlets);
        for (uint32_t i = 0; i < image_.rxCount; ++i) {
            const auto& s = image_.rx[i];
            SetQuad(RxEntryBase(i) + kRxIso, static_cast<uint32_t>(s.iso));
            SetQuad(RxEntryBase(i) + kRxSeqStart, s.speedOrSeqStart);
            SetQuad(RxEntryBase(i) + kRxAudio, s.pcm);
            SetQuad(RxEntryBase(i) + kRxMidi, s.midi);
            PutText(RxEntryBase(i) + kNamesOffset, kNamesBytes, s.names);
        }

        if (image_.hasExtSync) {
            const uint32_t base = image_.extSyncSection.offsetQuadlets * 4;
            SetQuad(base + kExtSyncClockSource, image_.extSync.clockSource);
            SetQuad(base + kExtSyncLocked, image_.extSync.locked);
            SetQuad(base + kExtSyncRate, image_.extSync.rateIndex);
            SetQuad(base + kExtSyncAdatUserData, image_.extSync.adatUserData);
        }
    }

    // Rebuild the stream blocks for a rate mode, when the device reports its
    // per-mode formats (TCAT extension current_config). Armed channels survive.
    void ApplyRateModeGeometry(uint32_t mode) {
        if (!image_.hasRateModeFormats) {
            return;
        }
        const auto& fmt = image_.rateModes[mode];
        SetQuad(TxSectionBase(), fmt.tx.count);
        for (uint32_t i = 0; i < fmt.tx.count; ++i) {
            SetQuad(TxEntryBase(i) + kTxAudio, fmt.tx.streams[i].pcm);
            SetQuad(TxEntryBase(i) + kTxMidi, fmt.tx.streams[i].midi);
            PutText(TxEntryBase(i) + kNamesOffset, kNamesBytes, fmt.tx.streams[i].names);
        }
        SetQuad(RxSectionBase(), fmt.rx.count);
        for (uint32_t i = 0; i < fmt.rx.count; ++i) {
            SetQuad(RxEntryBase(i) + kRxAudio, fmt.rx.streams[i].pcm);
            SetQuad(RxEntryBase(i) + kRxMidi, fmt.rx.streams[i].midi);
            PutText(RxEntryBase(i) + kNamesOffset, kNamesBytes, fmt.rx.streams[i].names);
        }
    }

    [[nodiscard]] bool IsStreamRegister(uint32_t offset) const {
        auto inEntry = [](uint32_t off, uint32_t base, uint32_t count, uint32_t stride) {
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t start = base + i * stride;
                if (off >= start && off < start + kNamesOffset) {
                    return true;
                }
            }
            return false;
        };
        return inEntry(offset, TxEntryBase(0), TxCount(), image_.txEntryQuadlets * 4) ||
               inEntry(offset, RxEntryBase(0), RxCount(), image_.rxEntryQuadlets * 4);
    }

    // Records host behaviour that wedged real devices. Never changes behaviour.
    void ObserveWrite(uint32_t offset, uint32_t value) {
        namespace G = ::ASFW::Audio::DICE::GlobalOffset;
        const bool enabled = Enable() != 0;
        if (offset == GlobalField(G::kClockSelect) && enabled) {
            // DiceFamilyDriver::WriteClockSelect: rewriting CLOCK_SELECT
            // while streams are enabled wedged the device-side streams.
            Violation("clock-select-while-enabled");
        }
        if (enabled && IsStreamRegister(offset)) {
            Violation("stream-register-write-while-enabled");
        }
        if (offset == GlobalField(G::kEnable) && value != 0 && !enabled) {
            // DiceFamilyDriver::StopSequence: a start over a stale
            // duplicate channel wedges the device until a power cycle.
            std::vector<int32_t> armed;
            bool unarmed = false;
            const auto collect = [&](int32_t iso) {
                if (iso < 0) {
                    unarmed = true;
                } else {
                    armed.push_back(iso);
                }
            };
            for (uint32_t i = 0; i < TxCount(); ++i) {
                collect(TxIso(i));
            }
            for (uint32_t i = 0; i < RxCount(); ++i) {
                collect(RxIso(i));
            }
            std::sort(armed.begin(), armed.end());
            if (std::adjacent_find(armed.begin(), armed.end()) != armed.end()) {
                Violation("enable-with-duplicate-channel");
            }
            if (unarmed) {
                // DiceFamilyDriver::ProgramRxStreams: every advertised
                // stream must be armed before the single GLOBAL_ENABLE.
                Violation("enable-with-unarmed-stream");
            }
        }
    }

    void ApplyWriteSideEffects(uint32_t offset, uint32_t value) {
        namespace G = ::ASFW::Audio::DICE::GlobalOffset;
        if (offset != GlobalField(G::kClockSelect)) {
            return;
        }
        switch (options_.clockResponse) {
        case ClockResponse::kAcceptAndLock:
        case ClockResponse::kAcceptNeverLock:
            AcceptClock(value, options_.publishClockAcceptedDuringWrite);
            break;
        case ClockResponse::kNeverAccept:
            Trace("# device: CLOCK_SELECT ignored");
            break;
        case ClockResponse::kAcceptLater:
            pendingClockSelect_ = value;
            Trace("# device: CLOCK_SELECT held");
            break;
        }
    }

    void AcceptClock(uint32_t clockSelect, bool publish) {
        namespace G = ::ASFW::Audio::DICE::GlobalOffset;
        const uint32_t rateIndex = (clockSelect & ::ASFW::Audio::DICE::ClockSelect::kRateMask) >>
                                   ::ASFW::Audio::DICE::ClockSelect::kRateShift;
        const uint32_t previousStatus = GlobalQuad(G::kStatus);
        const uint32_t previousIndex =
            (previousStatus >> ::ASFW::Audio::DICE::StatusBits::kNominalRateShift) & 0xFFU;
        const bool locks = options_.clockResponse != ClockResponse::kAcceptNeverLock;

        uint32_t bits = ::ASFW::Audio::DICE::Notify::kClockAccepted;
        if (locks) {
            SetAchievedClock(rateIndex, true);
            if (rateIndex != previousIndex) {
                // A hardware-validated 44.1 -> 48 kHz change raised
                // RxCfgChg|TxCfgChg|ClockAccepted together
                // (DICE_STABILITY_REGRESSION.md §3), even within one rate mode.
                bits |= ::ASFW::Audio::DICE::Notify::kRxConfigChange |
                        ::ASFW::Audio::DICE::Notify::kTxConfigChange;
                if (DiceRateModeForIndex(rateIndex) != DiceRateModeForIndex(previousIndex)) {
                    ApplyRateModeGeometry(DiceRateModeForIndex(rateIndex));
                }
            }
        } else {
            SetGlobal(G::kStatus, previousStatus & ~::ASFW::Audio::DICE::StatusBits::kSourceLocked);
        }
        SetNotificationRegister(bits);
        if (publish) {
            Deliver(bits);
        }
    }

    void Deliver(uint32_t bits) {
        char line[40];
        std::snprintf(line, sizeof(line), "# notify 0x%02x", bits);
        Trace(line);
        if (notify_) {
            notify_(bits);
        }
    }

    void Violation(std::string_view what) {
        violations_.emplace_back(what);
        Trace(std::string("! ") + std::string(what));
    }

    void Trace(std::string_view line) const {
        if (trace_) {
            trace_(line);
        }
    }

    DiceDeviceImage image_;
    Options options_;
    std::vector<uint8_t> memory_;
    std::optional<uint32_t> pendingClockSelect_;
    std::vector<std::string> violations_;
    NotifySink notify_;
    TraceSink trace_;
};

} // namespace ASFW::Testing::DICE
