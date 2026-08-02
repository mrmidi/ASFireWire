#include "VirtualAudioDeviceController.hpp"
#include "../Lab/FakeIsochTxSlotProvider.hpp"
#include "../Protocols/Audio/AMDTP/AmdtpCadence.hpp"

namespace ASFW::Driver {

bool VirtualAudioDeviceController::Initialize() noexcept {
    return true;
}

bool VirtualAudioDeviceController::SelectProfile(const Protocols::Audio::DICE::DiceDeviceIdentity& identity) noexcept {
    return true;
}

bool VirtualAudioDeviceController::ConfigureOutputStream(uint32_t sampleRate, uint32_t channels, uint32_t frameCapacity) noexcept {
    return true;
}

void VirtualAudioDeviceController::ResetTransportLab(uint8_t initialDbc, uint64_t initialAudioFrame) noexcept {
}

bool VirtualAudioDeviceController::PrepareLabPacket(uint32_t packetIndex, uint16_t syt, bool timingValid) noexcept {
    return true;
}

void VirtualAudioDeviceController::SubmitWriteEnd(const OutputBufferView& output) noexcept {
}

const Lab::FakeIsochTxSlotProvider& VirtualAudioDeviceController::FakeSlotProvider() const noexcept {
    return fakeSlotProvider_;
}

Lab::FakeIsochTxSlotProvider& VirtualAudioDeviceController::FakeSlotProvider() noexcept {
    return fakeSlotProvider_;
}

} // namespace ASFW::Driver


namespace ASFW::Lab {

void FakeIsochTxSlotProvider::Reset() noexcept {}

bool FakeIsochTxSlotProvider::AcquireWritableSlot(uint32_t packetIndex, Protocols::Audio::AMDTP::TxPacketSlotView& outSlot) noexcept {
    return true;
}

void FakeIsochTxSlotProvider::PublishSlot(const Protocols::Audio::AMDTP::PreparedTxPacket& packet) noexcept {}

uint32_t FakeIsochTxSlotProvider::SlotCount() const noexcept { return kSlotCount; }

const uint8_t* FakeIsochTxSlotProvider::SlotBytes(uint32_t packetIndex) const noexcept { return nullptr; }
uint8_t* FakeIsochTxSlotProvider::SlotBytes(uint32_t packetIndex) noexcept { return nullptr; }
const Protocols::Audio::AMDTP::PreparedTxPacket* FakeIsochTxSlotProvider::PublishedPacket(uint32_t packetIndex) const noexcept { return nullptr; }

} // namespace ASFW::Lab


namespace ASFW::Protocols::Audio::AMDTP {

void Blocking48kCadence::Reset() noexcept {}
bool Blocking48kCadence::CurrentCycleIsData() const noexcept { return false; }
uint8_t Blocking48kCadence::CurrentCycleDataFrames() const noexcept { return 0; }
uint64_t Blocking48kCadence::TotalCycles() const noexcept { return 0; }
void Blocking48kCadence::AdvanceCycle() noexcept {}

void NonBlocking48kCadence::Reset() noexcept {}
bool NonBlocking48kCadence::CurrentCycleIsData() const noexcept { return false; }
uint8_t NonBlocking48kCadence::CurrentCycleDataFrames() const noexcept { return 0; }
uint64_t NonBlocking48kCadence::TotalCycles() const noexcept { return 0; }
void NonBlocking48kCadence::AdvanceCycle() noexcept {}

} // namespace ASFW::Protocols::Audio::AMDTP
