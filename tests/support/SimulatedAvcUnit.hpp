// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SimulatedAvcUnit.hpp - Simulated AV/C unit backed by captured device images.
//
// Answers AV/C commands either via the IAvcUnit seam directly or by intercepting
// FCP writes to 0xFFFFF0000B00 over a RecordingFireWireBus and routing responses
// back to the driver's FCP transport.
//
// Supports fault injection:
// - Forced timeout / dropped response
// - INTERIM response deferral (0x0F interim, followed by final response)
// - Bus reset during transaction
// - NOT IMPLEMENTED (0x08) and REJECTED (0x0A) response overrides

#pragma once

#include "AvcDeviceImages.inc"
#include "RecordingFireWireBus.hpp"

#include "ASFWDriver/Protocols/AVC/Core/AvcError.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcFrame.hpp"
#include "ASFWDriver/Protocols/AVC/Core/AvcTypes.hpp"
#include "ASFWDriver/Protocols/AVC/Core/IAvcUnit.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ASFW::AVC::Testing {

/// Fault injection configuration for SimulatedAvcUnit.
struct SimulatedAvcFaults {
    bool timeoutNext{false};
    bool interimNext{false};
    bool rejectNext{false};
    bool notImplementedNext{false};
    bool busResetNext{false};
};

/// Simulated AV/C Unit backed by an AvcDeviceImage.
class SimulatedAvcUnit : public IAvcUnit {
public:
    explicit SimulatedAvcUnit(const AvcDeviceImage& image)
        : image_(image),
          nodeId_(static_cast<uint8_t>(image.nodeId)),
          generation_(image.generation),
          guid_(image.guid) {}

    ~SimulatedAvcUnit() override = default;

    SimulatedAvcUnit(const SimulatedAvcUnit&) = delete;
    SimulatedAvcUnit& operator=(const SimulatedAvcUnit&) = delete;

    // --- IAvcUnit implementation ---

    void Submit(const CommandFrame& frame,
                FW::Generation generation,
                ResponseCallback completion) override;

    [[nodiscard]] FW::NodeId NodeId() const noexcept override { return nodeId_; }
    [[nodiscard]] FW::Generation CurrentGeneration() const noexcept override { return generation_; }
    [[nodiscard]] uint64_t Guid() const noexcept override { return guid_; }

    void SetNodeId(FW::NodeId node) noexcept { nodeId_ = node; }
    void SetGeneration(FW::Generation gen) noexcept { generation_ = gen; }

    // --- Bus integration ---

    using FcpResponseSink = std::function<void(uint16_t srcNodeId, uint32_t generation, std::span<const uint8_t> payload)>;
    using SimpleResponseSink = std::function<void(const uint8_t* data, size_t length)>;

    /// Intercept FCP writes to 0xFFFFF0000B00 on `bus` and route matching responses to `responseSink`.
    void AttachToBus(::ASFW::Testing::RecordingFireWireBus& bus, FcpResponseSink responseSink);

    void AttachToBus(::ASFW::Testing::RecordingFireWireBus& bus, SimpleResponseSink responseSink) {
        AttachToBus(bus, [sink = std::move(responseSink)](uint16_t, uint32_t, std::span<const uint8_t> payload) {
            sink(payload.data(), payload.size());
        });
    }

    // --- Fault knobs ---

    void SetTimeoutNext(bool enable = true) noexcept { faults_.timeoutNext = enable; }
    void SetInterimNext(bool enable = true) noexcept { faults_.interimNext = enable; }
    void SetRejectNext(bool enable = true) noexcept { faults_.rejectNext = enable; }
    void SetNotImplementedNext(bool enable = true) noexcept { faults_.notImplementedNext = enable; }
    void SetBusResetNext(bool enable = true) noexcept { faults_.busResetNext = enable; }
    void ClearFaults() noexcept { faults_ = {}; }

    /// Override responses for specific command prefixes.
    void SetResponseOverride(std::vector<uint8_t> commandPrefix, std::vector<uint8_t> response) {
        overrides_.push_back({std::move(commandPrefix), std::move(response)});
    }

    void ClearOverrides() { overrides_.clear(); }

    [[nodiscard]] std::optional<std::span<const uint8_t>> FindResponse(std::span<const uint8_t> command) const;

private:
    struct OverrideEntry {
        std::vector<uint8_t> commandPrefix;
        std::vector<uint8_t> response;
    };

    const AvcDeviceImage& image_;
    FW::NodeId nodeId_{0};
    FW::Generation generation_{1};
    uint64_t guid_{0};
    SimulatedAvcFaults faults_{};
    std::vector<OverrideEntry> overrides_;
    std::vector<uint8_t> dynamicResponseStorage_;
};

} // namespace ASFW::AVC::Testing
