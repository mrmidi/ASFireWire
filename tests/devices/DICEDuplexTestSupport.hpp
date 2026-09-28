// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// DICEDuplexTestSupport.hpp - Shared test mocks and fixtures for DICE duplex and IRM tests

#pragma once

#include <gtest/gtest.h>

#include "FakeTimerScheduler.hpp"
#include "Testing/HostDriverKitStubs.hpp"
#include "Async/Interfaces/IFireWireBus.hpp"
#include "Common/WireFormat.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "Audio/Protocols/DICE/Core/DiceNotificationMailbox.hpp"
#include "Audio/Protocols/DICE/Core/DiceNotificationRouter.hpp"
#include "Audio/Protocols/DICE/Core/DICETransaction.hpp"
#include "Audio/Protocols/DICE/Core/DiceDeviceIo.hpp"
#include "Audio/Protocols/DICE/Core/DiceFamilyDriver.hpp"
#include "FakeDiceWaitClock.hpp"
#include "Protocols/Ports/ProtocolRegisterIO.hpp"
#include "RecordingFireWireBus.hpp"
#include "SimulatedDiceDevice.hpp"
#include "WireTrace.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ASFW::Testing::DICE {

using ::ASFW::Async::AsyncHandle;
using ::ASFW::Async::AsyncStatus;
using ::ASFW::Async::FWAddress;
using ::ASFW::Async::IFireWireBus;
using ::ASFW::Audio::AudioDuplexChannels;
using ::ASFW::Audio::DICE::ClockSource;
using ::ASFW::Audio::DICE::DICETransaction;
using ::ASFW::Audio::DICE::DICEBringupPolicy;
using ::ASFW::Audio::DuplexConfirmResult;
using ::ASFW::Audio::DuplexPrepareResult;
using ::ASFW::Audio::DuplexStageResult;
using ::ASFW::Audio::DICE::GeneralSections;
using ::ASFW::Audio::DICE::kOwnerNoOwner;
using ::ASFW::Audio::DICE::MakeDICEAddress;
using ::ASFW::Audio::DuplexRestartPhase;
using ::ASFW::Audio::DuplexRestartReason;
using ::ASFW::Audio::DICE::Section;
using ::ASFW::Audio::DICE::DiceDeviceIo;
using ::ASFW::Audio::DICE::DiceFamilyDriver;
using ::ASFW::Audio::DICE::DiceNotificationMailbox;
using ::ASFW::Audio::DICE::DiceNotificationRouter;
using ::ASFW::Audio::DICE::IsNotificationAddress;
using ::ASFW::Audio::DICE::kNotificationHandlerOffset;
using ::ASFW::Audio::DICE::DiceClockConfiguration;
using ::ASFW::FW::FwSpeed;
using ::ASFW::FW::Generation;
using ::ASFW::FW::LockOp;
using ::ASFW::FW::NodeId;
using ::ASFW::Protocols::Ports::ProtocolRegisterIO;
namespace ClockRateIndex = ::ASFW::Audio::DICE::ClockRateIndex;
namespace ClockSelectBits = ::ASFW::Audio::DICE::ClockSelect;
namespace GlobalOffset = ::ASFW::Audio::DICE::GlobalOffset;
namespace NotifyBits = ::ASFW::Audio::DICE::Notify;
namespace RxOffset = ::ASFW::Audio::DICE::RxOffset;
namespace StatusBits = ::ASFW::Audio::DICE::StatusBits;
namespace TxOffset = ::ASFW::Audio::DICE::TxOffset;

struct RouteState {
    ::ASFW::Discovery::DeviceRegistry registry;
    ::ASFW::Discovery::DeviceRouteToken route{};

    RouteState() {
        ::ASFW::Discovery::ConfigROM rom{};
        rom.bib.guid = 0xD1CE000000000002ULL;
        rom.gen = Generation{1};
        rom.nodeId = 0x02;
        (void)registry.UpsertFromROM(rom, ::ASFW::Discovery::LinkPolicy{});
        route = *registry.CurrentRoute(rom.bib.guid);
    }
};

constexpr uint32_t kGeneralSectionBytes = 40;
constexpr uint32_t kGlobalBytes = 380;
constexpr uint32_t kTxSectionOffset = 0x01A4;
constexpr uint32_t kRxSectionOffset = 0x03DC;
constexpr uint32_t kTxEntryQuadlets = 70;
constexpr uint32_t kRxEntryQuadlets = 70;
constexpr uint32_t kClockSelect48kInternal =
    (ClockRateIndex::k48000 << ClockSelectBits::kRateShift) |
    static_cast<uint32_t>(ClockSource::Internal);
constexpr uint32_t kLocked48kStatus =
    StatusBits::kSourceLocked |
    (ClockRateIndex::k48000 << StatusBits::kNominalRateShift);

using ::ASFW::Testing::OpKind;
using ::ASFW::Testing::RecordedOp;
using ::ASFW::Testing::ByteView;
using ::ASFW::Testing::ExpectedOp;
using ::ASFW::Testing::ExpectedRequest;
using ::ASFW::Testing::ResponseStep;

#include "ReferencePhase0ParityFixture.inc"

inline void PutBe32(uint8_t* dst, uint32_t value) {
    ::ASFW::FW::WriteBE32(dst, value);
}

inline void PutBe64(uint8_t* dst, uint64_t value) {
    ::ASFW::FW::WriteBE64(dst, value);
}

inline std::array<uint8_t, kGeneralSectionBytes> MakeGeneralSectionsWire() {
    std::array<uint8_t, kGeneralSectionBytes> bytes{};

    PutBe32(bytes.data() + 0x00, 0x0000000A);  // global offset 0x28
    PutBe32(bytes.data() + 0x04, 0x0000005F);  // global size 380
    PutBe32(bytes.data() + 0x08, 0x00000069);  // tx offset 0x1a4
    PutBe32(bytes.data() + 0x0C, 0x00000046);  // tx size 280
    PutBe32(bytes.data() + 0x10, 0x000000F7);  // rx offset 0x3dc
    PutBe32(bytes.data() + 0x14, 0x00000046);  // rx size 280

    return bytes;
}

inline GeneralSections MakeGeneralSections() {
    return GeneralSections{
        .global = Section{.offset = 0x0028, .size = kGlobalBytes},
        .txStreamFormat = Section{.offset = kTxSectionOffset, .size = kTxEntryQuadlets * 4},
        .rxStreamFormat = Section{.offset = kRxSectionOffset, .size = kRxEntryQuadlets * 4},
        .extSync = Section{},
        .reserved = Section{},
    };
}

// The single-device layout the controller tests were written against: a
// Saffire Pro 24 DSP-like section table and stream shape, with no clock-source
// names. Those tests and the reference-parity scripts run on it unchanged.
// (Channel names are now stored little-endian within each quadlet, as a real
// DICE device stores them; the old fake stored them in reading order.)
inline constexpr DiceDeviceImage kLegacyTestDeviceImage{
    .key = "legacy-test-device",
    .reportedModel = "DICEDuplexTestSupport legacy layout",
    .source = "DICEDuplexTestSupport.hpp",
    .guid = 0xD1CE000000000002ULL,
    .globalSection = {.offsetQuadlets = 0x0A, .sizeQuadlets = 0x5F},
    .txSection = {.offsetQuadlets = 0x69, .sizeQuadlets = 0x46},
    .rxSection = {.offsetQuadlets = 0xF7, .sizeQuadlets = 0x46},
    .extSyncSection = {},
    .hasExtension = false,
    .owner = kOwnerNoOwner,
    .notification = 0x00000010U,
    .nickname = "",
    .clockSelect = 0,
    .enable = 0,
    .status = kLocked48kStatus,
    .extStatus = 0,
    .sampleRate = 48000,
    .version = 0x01000C00,
    .clockCaps = 0x00001E06,
    .clockSourceNames = "",
    .txEntryQuadlets = kTxEntryQuadlets,
    .rxEntryQuadlets = kRxEntryQuadlets,
    .txCount = 1,
    .tx = {{{.iso = -1, .pcm = 16, .midi = 1, .speedOrSeqStart = 2, .names = "IP 1"}}},
    .rxCount = 1,
    .rx = {{{.iso = -1, .pcm = 8, .midi = 1, .speedOrSeqStart = 0, .names = "Mon 1"}}},
};

// IFireWireBus over one SimulatedDiceDevice. Records every request (as
// RecordedOp for the existing parity checks, and as a WireTrace for golden
// files), enforces the bus generation, and can inject one-shot faults.
// Scripted mode replays recorded responses instead of asking the device;
// writes and locks still reach the device so its state stays coherent.
class DiceRecordingFireWireBus final : public ::ASFW::Testing::RecordingFireWireBus {
public:
    explicit DiceRecordingFireWireBus(const DiceDeviceImage& image = kLegacyTestDeviceImage,
                                      SimulatedDiceOptions options = {})
        : device_(image, options), defaultClockResponse_(options.clockResponse) {
        generation_ = Generation{1};
        localNodeId_ = NodeId{0};
        speeds_[0x02] = FwSpeed::S400;
        device_.SetTraceSink([this](std::string_view line) { trace_.Add(line); });
    }

    DiceRecordingFireWireBus(const DiceRecordingFireWireBus&) = delete;
    DiceRecordingFireWireBus& operator=(const DiceRecordingFireWireBus&) = delete;

    // The simulated device sits at node 2 of bus 0x3FF.
    static constexpr uint16_t kDeviceSourceId = 0xFFC0 | 0x02;

    // Deliver the device's notification writes as the local request path
    // does: through the router, attributed by source node and generation.
    void RouteNotificationsTo(::ASFW::Audio::DICE::DiceNotificationRouter& router) {
        device_.SetNotifySink([this, &router](uint32_t bits) {
            (void)router.Deliver(generation_.value, kDeviceSourceId, bits);
        });
    }
    // Straight into one mailbox (driver-level tests have no router).
    void RouteNotificationsTo(::ASFW::Audio::DICE::DiceNotificationMailbox& mailbox) {
        device_.SetNotifySink([&mailbox](uint32_t bits) { mailbox.Publish(bits); });
    }

    [[nodiscard]] uint32_t TxSpeed() const {
        return device_.TxSpeed(0);
    }

    uint64_t Owner() const {
        return device_.Owner();
    }

    uint32_t Enable() const {
        return device_.Enable();
    }

    // Replace the device's own CLOCK_SELECT response with `handler`, which runs
    // after the write lands and before its completion is delivered.
    void SetClockSelectWriteHandler(std::function<void()> handler) {
        clockSelectWriteHandler_ = std::move(handler);
        auto options = device_.GetOptions();
        options.clockResponse = clockSelectWriteHandler_ ? DiceClockResponse::kNeverAccept
                                                         : defaultClockResponse_;
        device_.SetOptions(options);
    }

    void SetGlobalClockState(uint32_t status, uint32_t sampleRate,
                             uint32_t notification = 0) {
        device_.SetGlobalQuad(GlobalOffset::kStatus, status);
        device_.SetGlobalQuad(GlobalOffset::kSampleRate, sampleRate);
        device_.SetGlobalQuad(GlobalOffset::kNotification, notification);
    }

    void SetStreamIsoChannels(uint32_t txIso, uint32_t rxIso) {
        device_.SetTxIso(0, txIso);
        device_.SetRxIso(0, rxIso);
    }

    void PublishClockAccepted(uint32_t bits = NotifyBits::kClockAccepted) {
        device_.SetAchievedClock(ClockRateIndex::k48000, true);
        device_.RaiseNotification(bits);
    }

    void LatchClockAccepted(uint32_t bits = NotifyBits::kClockAccepted) {
        device_.SetAchievedClock(ClockRateIndex::k48000, true);
        device_.SetNotificationRegister(bits);
    }

    [[nodiscard]] SimulatedDiceDevice& Device() noexcept { return device_; }
    [[nodiscard]] const SimulatedDiceDevice& Device() const noexcept { return device_; }

protected:
    std::vector<uint8_t> ReadPayload(Async::FWAddress address, uint32_t length) override {
        if (address.addressHi == kDiceBaseAddressHi) {
            if (auto bytes = device_.Read(address.addressLo, length)) {
                return *bytes;
            }
        }
        return std::vector<uint8_t>(length, 0);
    }

    void WritePayload(Async::FWAddress address, std::span<const uint8_t> data) override {
        if (address.addressHi == kDiceBaseAddressHi) {
            (void)device_.Write(address.addressLo, data);
        }
        if (clockSelectWriteHandler_ && IsClockSelect(address)) {
            clockSelectWriteHandler_();
        }
    }

    std::optional<uint64_t> ApplyLock(Async::FWAddress address, bool compareSwap64,
                                      uint64_t expected, uint64_t desired) override {
        if (!compareSwap64 || address.addressHi != kDiceBaseAddressHi) {
            return std::nullopt;
        }
        return device_.CompareSwap64(address.addressLo, expected, desired);
    }

    void OnBusReset() override {
        device_.BusReset();
    }

private:
    [[nodiscard]] bool IsClockSelect(Async::FWAddress address) const {
        return address.addressHi == kDiceBaseAddressHi &&
               address.addressLo == kDiceBaseAddressLo + device_.GlobalBase() + GlobalOffset::kClockSelect;
    }

    SimulatedDiceDevice device_;
    DiceClockResponse defaultClockResponse_;
    std::function<void()> clockSelectWriteHandler_;
};

using RecordingFireWireBus = DiceRecordingFireWireBus;

struct HostClockResetGuard {
    ~HostClockResetGuard() {
        ::ASFW::Testing::ResetHostMonotonicClockForTesting();
    }
};

// Callback-style view of DiceFamilyDriver. The bring-up tests were written
// against the async DICEDuplexBringupController (removed in S1); running the
// same test bodies against the linear driver was the equivalence check. Each call
// completes before it returns, so the callback fires synchronously.
class CallbackDriver {
public:
    explicit CallbackDriver(DiceFamilyDriver& driver) noexcept : driver_(driver) {}

    // Reference-parity window: fixed 48 kHz internal clock, no caps refresh.
    void PrepareDuplex48k(const AudioDuplexChannels& channels, std::function<void(IOReturn)> callback) {
        const auto result = driver_.Prepare(channels, k48k, /*refreshRuntimeCaps=*/false);
        callback(result ? kIOReturnSuccess : result.error());
    }
    void PrepareDuplex(const AudioDuplexChannels& channels, const DiceClockConfiguration& clock,
                       std::function<void(IOReturn, DuplexPrepareResult)> callback) {
        Deliver(driver_.Prepare(channels, clock, /*refreshRuntimeCaps=*/true), callback);
    }
    void ProgramRxForDuplex48k(std::function<void(IOReturn)> callback) {
        const auto result = driver_.ProgramRx();
        callback(result ? kIOReturnSuccess : result.error());
    }
    void ProgramRx(std::function<void(IOReturn, DuplexStageResult)> callback) {
        Deliver(driver_.ProgramRx(), callback);
    }
    void ProgramTxAndEnableDuplex48k(std::function<void(IOReturn)> callback) {
        const auto result = driver_.ProgramTxAndEnable();
        callback(result ? kIOReturnSuccess : result.error());
    }
    void ProgramTxAndEnableDuplex(std::function<void(IOReturn, DuplexStageResult)> callback) {
        Deliver(driver_.ProgramTxAndEnable(), callback);
    }
    void ConfirmDuplex48kStart(std::function<void(IOReturn)> callback) {
        const auto result = driver_.Confirm();
        callback(result ? kIOReturnSuccess : result.error());
    }
    void ConfirmDuplexStart(std::function<void(IOReturn, DuplexConfirmResult)> callback) {
        Deliver(driver_.Confirm(), callback);
    }
    [[nodiscard]] IOReturn StopDuplex() { return driver_.Stop(); }

    [[nodiscard]] bool IsPrepared() const noexcept { return driver_.IsPrepared(); }
    [[nodiscard]] bool IsArmed() const noexcept { return driver_.IsArmed(); }
    [[nodiscard]] bool IsRunning() const noexcept { return driver_.IsRunning(); }
    [[nodiscard]] bool IsOwnerHeld() const noexcept { return driver_.IsOwnerHeld(); }

private:
    static constexpr DiceClockConfiguration k48k{
        .sampleRateHz = 48000U,
        .clockSelect = ::ASFW::Audio::DICE::kDiceClockSelect48kInternal,
    };

    template <typename T, typename Callback>
    static void Deliver(const std::expected<T, IOReturn>& result, Callback& callback) {
        callback(result ? kIOReturnSuccess : result.error(), result ? *result : T{});
    }

    DiceFamilyDriver& driver_;
};

struct DuplexRig {
    RecordingFireWireBus bus;
    RouteState routeState;
    ProtocolRegisterIO io;
    DICETransaction tx;
    ::ASFW::Testing::FakeTimerScheduler timer;
    ::ASFW::Testing::FakeDiceWaitClock clock{timer};
    DiceDeviceIo deviceIo;
    std::atomic<bool> cancel{false};
    DiceNotificationMailbox notifications;
    DiceFamilyDriver driver;
    CallbackDriver controller{driver};

    explicit DuplexRig(DICEBringupPolicy bringupPolicy = {})
        : io(bus, bus, routeState.registry, routeState.route)
        , tx(io)
        , deviceIo(io, tx, clock)
        , driver(deviceIo, bus, notifications, bringupPolicy) {
        driver.SetTeardownCancelToken(&cancel);
        bus.RouteNotificationsTo(notifications);
    }
};

inline std::vector<ExpectedOp> ExpectedStopOps() {
    return {
        {OpKind::Write, 0xE0000078U, 4, FwSpeed::S400},
        {OpKind::Read,  0xE00001A8U, 4, FwSpeed::S400},
        {OpKind::Write, 0xE00001ACU, 4, FwSpeed::S400},
        {OpKind::Write, 0xE00001B8U, 4, FwSpeed::S400},
        {OpKind::Read,  0xE00003E0U, 4, FwSpeed::S400},
        {OpKind::Write, 0xE00003E4U, 4, FwSpeed::S400},
        {OpKind::Write, 0xE00003E8U, 4, FwSpeed::S400},
    };
}

template <typename T>
inline std::vector<T> Concat(std::span<const T> first, std::span<const T> second) {
    std::vector<T> merged;
    merged.reserve(first.size() + second.size());
    merged.insert(merged.end(), first.begin(), first.end());
    merged.insert(merged.end(), second.begin(), second.end());
    return merged;
}

inline void ExpectRequests(const std::vector<RecordedOp>& actual,
                           std::span<const ExpectedRequest> expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(actual[i].kind, expected[i].kind) << "op " << i;
        EXPECT_EQ(actual[i].addressHi, expected[i].addressHi) << "op " << i;
        EXPECT_EQ(actual[i].addressLo, expected[i].addressLo) << "op " << i;
        EXPECT_EQ(actual[i].length, expected[i].length) << "op " << i;
        EXPECT_EQ(actual[i].speed, expected[i].speed) << "op " << i;
        EXPECT_EQ(actual[i].responseLength, expected[i].responseLength) << "op " << i;
        EXPECT_EQ(actual[i].payload.size(), expected[i].payload.size) << "op " << i;
        if (actual[i].payload.size() == expected[i].payload.size && expected[i].payload.size > 0) {
            EXPECT_TRUE(std::equal(actual[i].payload.begin(),
                                   actual[i].payload.end(),
                                   expected[i].payload.data))
                << "op " << i;
        }
    }
}

inline void ExpectOperations(const std::vector<RecordedOp>& actual,
                             const std::vector<ExpectedOp>& expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(actual[i].kind, expected[i].kind) << "op " << i;
        EXPECT_EQ(actual[i].addressLo, expected[i].addressLo) << "op " << i;
        EXPECT_EQ(actual[i].length, expected[i].length) << "op " << i;
        EXPECT_EQ(actual[i].speed, expected[i].speed) << "op " << i;
    }
}

} // namespace ASFW::Testing::DICE
