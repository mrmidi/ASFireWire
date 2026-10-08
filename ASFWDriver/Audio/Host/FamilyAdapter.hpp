// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FamilyAdapter.hpp - What the audio device host asks of a device family.
//
// documentation/AUDIO_DEVICE_HOST.md §4.2. The host owns the lifecycle shell
// (admission, route checks, queueing, dedupe, teardown); a family states only
// what differs in its sources or on the wire. Adapters hold no state of their
// own (§4.1 rule 6): every per-device fact arrives in the call's arguments, and
// every answer leaves as a value.
//
// Two-protocol rule (DEVICE_BACKEND_UNIFICATION.md): a method lives here only
// when two unrelated families implement it meaningfully. Describe: all four.
// JudgeRuntimeFault: DICE (device health) and AV/C (settle, then RX replay).

#pragma once

#include "../Model/ASFWAudioDevice.hpp"
#include "../Protocols/Duplex/DuplexControlTypes.hpp"
#include "../../Discovery/DiscoveryTypes.hpp"
#include "../../DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"

#include <DriverKit/IOReturn.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace ASFW::Audio {
class IDeviceProtocol;
}

namespace ASFW::Audio::Host {

/// Everything a family needs to describe one endpoint, as values.
struct DescribeInput {
    Discovery::DeviceRecord record;
    /// The current catalog decision for `record` (CurrentAudioPolicy). Non-null.
    std::shared_ptr<const DeviceProfiles::Audio::ResolvedDevicePolicy> policy;
    /// The device's protocol object, or null while none exists yet.
    std::shared_ptr<IDeviceProtocol> protocol;
    /// The live endpoint's configuration when a nub is already published.
    std::optional<Model::ASFWAudioDevice> committed;
    /// The last description discovery pushed for this device (AV/C), if any.
    std::optional<Model::ASFWAudioDevice> discovered;
};

/// Leave the live endpoint as it is: a confirmed configuration transaction
/// committed it after discovery, and a republish must not overwrite it
/// (§4.3; DiceAudioBackend's committed-formation guard).
struct KeepCommitted {};

/// The family refuses to describe the endpoint. `reason` is a static string;
/// the host prints it once, in its [AudioHost] line.
struct DescribeRefusal {
    IOReturn status{kIOReturnNotReady};
    const char* reason{"unspecified"};
};

using DescribeResult = std::variant<Model::ASFWAudioDevice, KeepCommitted, DescribeRefusal>;
/// Called exactly once, on any thread, possibly after Describe returned.
using DescribeDone = std::function<void(DescribeResult)>;

enum class FaultVerdict : uint8_t {
    kRestart,     // the fault is real: restart the streams
    kSelfHealed,  // the device or transport recovered on its own; drop it
    kDeviceLeft,  // the device stopped streaming or vanished while judging
};

/// What a fault judgement may ask about the live system. Implemented by the
/// host for one device and one fault; valid only during the call.
class FaultContext {
public:
    virtual ~FaultContext() = default;
    /// Service teardown, device retirement or removal. Waits must give up.
    [[nodiscard]] virtual bool Cancelled() const noexcept = 0;
    [[nodiscard]] virtual bool StillStreaming() const noexcept = 0;
    /// The host RX path has re-established replay (the transport self-healed).
    [[nodiscard]] virtual bool ReceiveReplayEstablished() const noexcept = 0;
    /// The family's own health read (FamilyDriver::ReadHealth). Blocks.
    [[nodiscard]] virtual std::expected<DuplexHealthResult, IOReturn> ReadHealth(uint32_t timeoutMs) = 0;
    /// Sleep on the host queue; a test context may advance a fake clock instead.
    virtual void Sleep(uint32_t milliseconds) noexcept = 0;
};

/// Neutral device events, raised by an adapter from its family's own signals.
enum class DeviceEvent : uint8_t {
    kStreamConfigChanged,  // the device changed its stream configuration
    kClockStatusChanged,   // lock or clock-reference status changed
};

class DeviceEventSink {
public:
    virtual ~DeviceEventSink() = default;
    /// `detail` carries the family's raw evidence (e.g. DICE notification bits)
    /// for the host's log line only; the host never decodes it.
    virtual void OnDeviceEvent(uint64_t guid, DeviceEvent event, uint32_t detail) noexcept = 0;
};

class FamilyAdapter {
public:
    virtual ~FamilyAdapter() = default;

    [[nodiscard]] virtual const char* Name() const noexcept = 0;

    /// Build the endpoint description. May complete later (DICE loads its
    /// geometry asynchronously). A refusal is not retried by the host; the next
    /// trigger (record update, restart) describes again.
    virtual void Describe(const DescribeInput& in, DescribeDone done) = 0;

    /// Is this runtime fault real? Runs on the host queue and may block, but
    /// must return promptly once `context.Cancelled()` reads true.
    [[nodiscard]] virtual FaultVerdict JudgeRuntimeFault(uint64_t guid,
                                                        DuplexRestartReason reason,
                                                        FaultContext& context) = 0;

    /// The host installs its sink once, before device callbacks begin; null
    /// detaches it during teardown. A family with no device events stores
    /// nothing and says why.
    virtual void SetEventSink(DeviceEventSink* sink) noexcept = 0;
};

} // namespace ASFW::Audio::Host
