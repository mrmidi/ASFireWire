// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "DuplexControlAdapter.hpp"

#include "../Protocols/Backends/SyncAsyncBridge.hpp"

#include <utility>

namespace ASFW::Audio::Session {

namespace {

template <typename T>
std::expected<T, IOReturn> ToExpected(const SyncResult<T>& result) {
    if (result.status != kIOReturnSuccess) {
        return std::unexpected(result.status);
    }
    return result.value;
}

} // namespace

DuplexControlAdapter::DuplexControlAdapter(std::shared_ptr<IDeviceProtocol> protocol,
                                           IDuplexDeviceControl& control,
                                           const std::atomic<bool>* cancel) noexcept
    : protocol_(std::move(protocol)), control_(control), cancel_(cancel) {
    control_.SetTeardownCancelToken(cancel_);
}

::ASFW::IRM::IRMClient* DuplexControlAdapter::IrmClient() const noexcept {
    return control_.GetIRMClient();
}

IOReturn DuplexControlAdapter::LoadGeometry() {
    return WaitForAsyncStatus(
        [&](auto callback) { control_.EnsureRuntimeStreamGeometry(std::move(callback)); },
        kStageTimeoutMs, kIOReturnTimeout, cancel_);
}

std::optional<AudioStreamRuntimeCaps> DuplexControlAdapter::RuntimeCaps() const {
    AudioStreamRuntimeCaps caps{};
    if (protocol_ == nullptr || !protocol_->GetRuntimeAudioStreamCaps(caps)) {
        return std::nullopt;
    }
    return caps;
}

std::expected<DuplexPrepareResult, IOReturn> DuplexControlAdapter::Configure(
    const AudioDuplexChannels& channels, const AudioClockConfig& clock) {
    return ToExpected(WaitForAsyncResult<DuplexPrepareResult>(
        [&](auto callback) { control_.PrepareDuplex(channels, clock, std::move(callback)); },
        kStageTimeoutMs, kIOReturnTimeout, cancel_));
}

void DuplexControlAdapter::AssignChannels(const AudioDuplexChannels& channels) {
    control_.SetAssignedChannels(channels);
}

std::expected<DuplexHealthResult, IOReturn> DuplexControlAdapter::ReadHealth(uint32_t timeoutMs) {
    return ToExpected(WaitForAsyncResult<DuplexHealthResult>(
        [&](auto callback) { control_.ReadDuplexHealth(std::move(callback)); }, timeoutMs,
        kIOReturnTimeout, cancel_));
}

std::expected<DuplexStageResult, IOReturn> DuplexControlAdapter::ArmDeviceRx() {
    return ToExpected(WaitForAsyncResult<DuplexStageResult>(
        [&](auto callback) { control_.ProgramRx(std::move(callback)); }, kStageTimeoutMs,
        kIOReturnTimeout, cancel_));
}

std::expected<DuplexStageResult, IOReturn> DuplexControlAdapter::ArmDeviceTxAndEnable() {
    return ToExpected(WaitForAsyncResult<DuplexStageResult>(
        [&](auto callback) { control_.ProgramTxAndEnableDuplex(std::move(callback)); },
        kStageTimeoutMs, kIOReturnTimeout, cancel_));
}

std::expected<DuplexConfirmResult, IOReturn> DuplexControlAdapter::Confirm() {
    return ToExpected(WaitForAsyncResult<DuplexConfirmResult>(
        [&](auto callback) { control_.ConfirmDuplexStart(std::move(callback)); }, kStageTimeoutMs,
        kIOReturnTimeout, cancel_));
}

std::expected<DuplexClockApplyResult, IOReturn> DuplexControlAdapter::ApplyClockIdle(
    const AudioClockConfig& clock) {
    return ToExpected(WaitForAsyncResult<DuplexClockApplyResult>(
        [&](auto callback) { control_.ApplyClockConfig(clock, std::move(callback)); },
        kStageTimeoutMs, kIOReturnTimeout, cancel_));
}

IOReturn DuplexControlAdapter::DisconnectPlayback() {
    return WaitForAsyncStatus(
        [&](auto callback) { control_.DisconnectPlayback(std::move(callback)); }, kStageTimeoutMs,
        kIOReturnTimeout, cancel_);
}

IOReturn DuplexControlAdapter::DisconnectCapture() {
    return WaitForAsyncStatus(
        [&](auto callback) { control_.DisconnectCapture(std::move(callback)); }, kStageTimeoutMs,
        kIOReturnTimeout, cancel_);
}

IOReturn DuplexControlAdapter::BreakConnections() {
    return WaitForAsyncStatus(
        [&](auto callback) { control_.BreakBothConnections(std::move(callback)); },
        kStageTimeoutMs, kIOReturnTimeout, cancel_);
}

IOReturn DuplexControlAdapter::Stop() {
    return control_.StopDuplex();
}

} // namespace ASFW::Audio::Session
