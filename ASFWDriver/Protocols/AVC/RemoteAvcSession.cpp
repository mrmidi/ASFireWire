// SPDX-License-Identifier: Apache-2.0

#include "RemoteAvcSession.hpp"

namespace ASFW::Protocols::AVC {

Discovery::UnitInstanceId RemoteAvcSession::UnitId() const noexcept {
    return unit_ ? unit_->GetUnitInstanceId() : Discovery::UnitInstanceId{};
}

Discovery::DeviceInstanceId RemoteAvcSession::DeviceId() const noexcept {
    return unit_ ? unit_->GetDeviceInstanceId() : Discovery::DeviceInstanceId{};
}

uint64_t RemoteAvcSession::ObservedGuid() const noexcept {
    return unit_ ? unit_->GetObservedGuid() : 0;
}

bool RemoteAvcSession::IsInitialized() const noexcept {
    return unit_ && unit_->IsInitialized();
}

std::shared_ptr<FCPTransport> RemoteAvcSession::Transport() const noexcept {
    return unit_ ? unit_->GetFCPTransportShared() : nullptr;
}

void RemoteAvcSession::Initialize(std::function<void(bool)> completion) {
    if (!unit_) {
        if (completion) completion(false);
        return;
    }
    unit_->Initialize(std::move(completion));
}

void RemoteAvcSession::ProbePlug0(Probe::ReadOnlyProbeCompletion completion) {
    if (!unit_) {
        if (completion) completion({});
        return;
    }
    // STATUS-only sequence; family code decides whether catalog safety permits
    // it. Cross-validated with Linux BeBoB format discovery:
    // references/linux-sound-firewire-stack/firewire/bebob/bebob_command.c:91-107.
    Probe::StartAVCPlug0Discovery(*unit_, unit_->GetObservedGuid(),
                                  std::move(completion));
}

bool RemoteAvcSession::HasAudioSubunit() const noexcept {
    if (!unit_) return false;
    for (const auto& subunit : unit_->GetSubunits()) {
        if (subunit && subunit->GetType() == AVCSubunitType::kAudio) return true;
    }
    return false;
}

bool RemoteAvcSession::HasMusicSubunit() const noexcept {
    if (!unit_) return false;
    for (const auto& subunit : unit_->GetSubunits()) {
        if (!subunit) continue;
        const auto type = subunit->GetType();
        if (type == AVCSubunitType::kMusic || type == AVCSubunitType::kMusic0C) {
            return true;
        }
    }
    return false;
}

} // namespace ASFW::Protocols::AVC
