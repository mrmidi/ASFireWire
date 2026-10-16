//
// ASFWMidiNub.cpp
// ASFWDriver
//

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>

#include "ASFWMidiNub.h"
#include "ASFWDriver.h"

#include "../../Audio/Core/AudioEndpointRuntime.hpp"
#include "../../Audio/Core/AudioRuntimeRegistry.hpp"
#include "../../Controller/ControllerCore.hpp"
#include "../../Logging/Logging.hpp"

#include <atomic>

namespace {

// Registered receive actions are retained until the nub frees, never released
// when the MIDI service withdraws: the core driver's receive queue may be
// inside NotifyMidiReceived with the action it just loaded, and an OSAction
// released under it is a use-after-free (the FW-60 class). A MIDI service
// registers once per nub in practice; the cap bounds the worst case.
constexpr uint32_t kMaxRegisteredActions = 4;

} // namespace

struct ASFWMidiNub_IVars {
    IOService* parentDriver{nullptr};
    uint64_t endpointId{0};
    std::atomic<OSAction*> receiveAction{nullptr};
    OSAction* retained[kMaxRegisteredActions]{};
    uint32_t retainedCount{0};
    std::atomic<bool> published{false};
};

namespace {

[[nodiscard]] std::shared_ptr<ASFW::Audio::AudioEndpointRuntime> FindEndpointRuntime(
    const ASFWMidiNub_IVars* iv) noexcept {
    if (iv == nullptr || iv->endpointId == 0 || iv->parentDriver == nullptr) return nullptr;
    const auto* parent = OSDynamicCast(ASFWDriver, iv->parentDriver);
    if (parent == nullptr) return nullptr;
    const auto* core = static_cast<ASFW::Driver::ControllerCore*>(parent->GetControllerCore());
    if (core == nullptr) return nullptr;
    auto* registry = core->GetAudioRuntimeRegistry();
    return registry != nullptr ? registry->FindEndpointRuntime(iv->endpointId) : nullptr;
}

} // namespace

bool ASFWMidiNub::init() {
    if (!super::init()) return false;
    ivars = IONewZero(ASFWMidiNub_IVars, 1);
    return ivars != nullptr;
}

void ASFWMidiNub::free() {
    if (ivars != nullptr) {
        ivars->receiveAction.store(nullptr, std::memory_order_release);
        for (uint32_t i = 0; i < ivars->retainedCount; ++i) {
            if (ivars->retained[i] != nullptr) ivars->retained[i]->release();
        }
        ivars->retainedCount = 0;
    }
    IOSafeDeleteNULL(ivars, ASFWMidiNub_IVars, 1);
    super::free();
}

kern_return_t IMPL(ASFWMidiNub, Start) {
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Midi, "ASFWMidiNub: super::Start failed 0x%x", ret);
        return ret;
    }
    if (ivars != nullptr) ivars->parentDriver = provider;
    // Deliberately not registered here, and the endpoint id deliberately not read
    // here: the publisher has not set either yet. See Publish().
    ASFW_LOG(Midi, "ASFWMidiNub: started (awaiting publication)");
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWMidiNub, Stop) {
    ASFW_LOG(Midi, "ASFWMidiNub: stopping endpoint=%llu", ivars ? ivars->endpointId : 0ULL);
    if (ivars != nullptr) {
        // Stop notifying first; the retained actions stay alive until free().
        ivars->receiveAction.store(nullptr, std::memory_order_release);
        ivars->parentDriver = nullptr;
    }
    return Stop(provider, SUPERDISPATCH);
}

void ASFWMidiNub::SetEndpointId(uint64_t endpointId) {
    if (ivars != nullptr) ivars->endpointId = endpointId;
}

uint64_t ASFWMidiNub::GetEndpointId() const {
    return ivars != nullptr ? ivars->endpointId : 0;
}

kern_return_t ASFWMidiNub::Publish() {
    if (ivars == nullptr) return kIOReturnNotReady;
    if (ivars->endpointId == 0) {
        // The crash this guards against: a MIDI service started against an
        // endpoint id of zero cannot allocate its I/O buffer.
        ASFW_LOG_ERROR(Midi, "ASFWMidiNub: refusing to publish without an endpoint id");
        return kIOReturnNotReady;
    }
    if (ivars->published.exchange(true)) return kIOReturnSuccess;
    const kern_return_t kr = RegisterService();
    if (kr != kIOReturnSuccess) {
        ivars->published.store(false);
        ASFW_LOG_ERROR(Midi, "ASFWMidiNub: RegisterService failed 0x%x", kr);
        return kr;
    }
    ASFW_LOG(Midi, "ASFWMidiNub: published endpoint=%llu", ivars->endpointId);
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWMidiNub, CopyMidiTransportMemory) {
    if (outMemory != nullptr) *outMemory = nullptr;
    if (ivars == nullptr || outMemory == nullptr) return kIOReturnBadArgument;
    auto endpoint = FindEndpointRuntime(ivars);
    if (!endpoint) return kIOReturnNotReady;
    ASFW::Audio::MidiEndpointAttachment attachment{};
    return endpoint->CopyMidiTransportMemory(outMemory, attachment);
}

kern_return_t IMPL(ASFWMidiNub, RegisterMidiReceiveAction) {
    if (ivars == nullptr) return kIOReturnNotReady;
    if (action == nullptr) {
        // Withdrawal: stop notifying. The action itself is released in free().
        ivars->receiveAction.store(nullptr, std::memory_order_release);
        return kIOReturnSuccess;
    }
    if (ivars->retainedCount >= kMaxRegisteredActions) return kIOReturnNoSpace;
    action->retain();
    ivars->retained[ivars->retainedCount++] = action;
    ivars->receiveAction.store(action, std::memory_order_release);
    return kIOReturnSuccess;
}

void IMPL(ASFWMidiNub, MidiReceiveReady) {
    // Originated here, handled by ASFWMIDIDriver's override.
    (void)action;
}

bool ASFWMidiNub::NotifyMidiReceived() {
    if (ivars == nullptr) return false;
    OSAction* action = ivars->receiveAction.load(std::memory_order_acquire);
    if (action == nullptr) return false;
    // A one-way hand-off to the MIDI service's queue. Returns at once: the
    // receive queue must not be held while a CoreMIDI client is served.
    MidiReceiveReady(action);
    return true;
}
