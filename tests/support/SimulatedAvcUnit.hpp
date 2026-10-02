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

    /// Register a raw descriptor image to be served dynamically for OPEN/READ/CLOSE commands.
    /// @param subunit Subunit address (e.g. 0x60 for Music Subunit, 0x08 for Audio Subunit, 0xFF for Unit)
    /// @param specifier Descriptor specifier bytes (e.g. {0x80} for Status, {0x00} for Identifier)
    /// @param descriptorRaw Complete descriptor bytes (including 2-byte descriptor_length header)
    void SetDescriptor(uint8_t subunit, std::vector<uint8_t> specifier, std::vector<uint8_t> descriptorRaw) {
        descriptors_.push_back(DescriptorEntry{
            .subunit = subunit,
            .specifier = std::move(specifier),
            .rawBytes = std::move(descriptorRaw),
        });
    }

    void ClearDescriptors() { descriptors_.clear(); }

    void SetDeferredResponses(bool deferred) noexcept { deferResponses_ = deferred; }
    void FlushDeferredResponses();

    [[nodiscard]] std::optional<std::span<const uint8_t>> FindResponse(std::span<const uint8_t> command) const;

    /// Every command the image had no measured answer for. The simulator still
    /// answers NOT IMPLEMENTED, but a real device was never asked: such a frame
    /// is untested on the wire (a bare UNIT INFO wedged a Phase 88 this way).
    [[nodiscard]] const std::vector<std::vector<uint8_t>>& UnmeasuredCommands() const noexcept {
        return unmeasured_;
    }

    /// Commands written to FCP_COMMAND while the unit's previous response
    /// transaction was still open, i.e. before the host's write response to
    /// it. The simulator drops them, as a Phase 88 does.
    [[nodiscard]] const std::vector<std::vector<uint8_t>>& CommandsWhileResponseOpen() const noexcept {
        return commandsWhileResponseOpen_;
    }

private:
    void Deliver(ResponseCallback completion, Expected<Response> response);
    void DeliverResponse(const CommandFrame& frame, ResponseCallback completion,
                         std::span<const uint8_t> bytes);

    struct DescriptorEntry {
        uint8_t subunit{0xFF};
        std::vector<uint8_t> specifier;
        std::vector<uint8_t> rawBytes;
        bool readOpen{false};
    };
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
    std::vector<std::vector<uint8_t>> unmeasured_;
    std::vector<std::vector<uint8_t>> commandsWhileResponseOpen_;
    bool responseTransactionOpen_{false};
    mutable std::vector<DescriptorEntry> descriptors_;
    mutable std::vector<uint8_t> dynamicResponseStorage_;
    bool deferResponses_{false};
    std::vector<std::function<void()>> deferredResponses_;
};

} // namespace ASFW::AVC::Testing
