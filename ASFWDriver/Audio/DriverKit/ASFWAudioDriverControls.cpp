//
// ASFWAudioDriverControls.cpp
// ASFWDriver
//
// CoreAudio control writes and confirmed-value pushes for ASFWAudioDriver.
//

#include "ASFWAudioDriverPrivate.hpp"
#include <bit>
#include <cmath>
#include <algorithm>

kern_return_t ASFWAudioDriver::ApplyControlParameter(uint32_t parameterId, uint64_t kind, uint64_t bits,
    uint64_t* confirmedKind, uint64_t* confirmedBits) {
    if (!ivars || !ivars->device.audioNub) return kIOReturnNotReady;
    return ivars->device.audioNub->SetControlParameter(parameterId, kind, bits, confirmedKind, confirmedBits);
}

namespace {
// A pushed or re-read level the HAL can show: finite, and for a dB control
// inside the range the control was published with. A normalized control
// carries a 0..1 position instead.
bool LevelValueFits(const ASFW::Audio::Controls::CoreAudioControl& descriptor, double value) {
    if (!std::isfinite(value)) return false;
    if (descriptor.normalized) return value >= 0 && value <= 1;
    const auto* range = std::get_if<ASFW::AudioModel::ScalarDomain>(&descriptor.domain);
    return range != nullptr && value >= range->min && value <= range->max;
}
} // namespace

// Delivered on the same Default queue as graph construction and teardown.
// Set*Value updates HAL only; HandleChange* is the hardware-write entry point.
void IMPL(ASFWAudioDriver, ControlParameterChanged) {
    if (!ivars || action != ivars->controlChangeAction.get() || !ivars->audioDevice ||
        guid != ivars->device.guid || origin != 0 || revision < ivars->device.controlRevision) return;
    if (!(kind == 4 && parameterId == 0) && std::ranges::none_of(ivars->device.parameterControls,
        [parameterId](const auto& slot) { return slot.descriptor.parameter.value == parameterId; })) return;
    if (revision > ivars->device.controlRevision) {
        ivars->device.controlRevision = revision;
        for (auto& slot : ivars->device.parameterControls) slot.sequence = 0;
    }
    for (auto& slot : ivars->device.parameterControls) {
        if (kind == 4 && parameterId == 0) {
            if (sequence <= slot.sequence) continue;
            uint64_t currentKind{}, currentBits{};
            if (ivars->device.audioNub->ReadControlParameter(slot.descriptor.parameter.value,
                    slot.descriptor.normalized, &currentKind, &currentBits) != kIOReturnSuccess) continue;
            if (slot.boolean && currentKind == 0) (void)slot.boolean->SetControlValue(currentBits != 0);
            else if (slot.level && currentKind == (slot.descriptor.normalized ? 4u : 1u) &&
                     LevelValueFits(slot.descriptor, std::bit_cast<double>(currentBits)))
                (void)slot.level->SetDecibelValue(static_cast<float>(std::bit_cast<double>(currentBits)));
            else if (slot.selector && currentKind == 2) {
                const uint32_t selected = static_cast<uint32_t>(currentBits);
                (void)slot.selector->SetCurrentSelectedValues(&selected, 1);
            }
            slot.sequence = sequence;
            continue;
        }
        if (slot.descriptor.parameter.value != parameterId) continue;
        if ((sequence == 0 && slot.sequence != 0) || (sequence != 0 && sequence <= slot.sequence)) return;
        kern_return_t status = kIOReturnUnsupported;
        if (kind == 0 && bits <= 1 && slot.boolean) status = slot.boolean->SetControlValue(bits != 0);
        else if (kind == 2 && slot.selector) {
            const uint32_t value = static_cast<uint32_t>(bits);
            status = slot.selector->SetCurrentSelectedValues(&value, 1);
        } else if (kind == 1 && slot.level) {
            double value = std::bit_cast<double>(bits);
            if (!std::isfinite(value)) return;
            if (slot.descriptor.normalized) {
                uint64_t currentKind{}, currentBits{};
                status = ivars->device.audioNub->ReadControlParameter(parameterId, true, &currentKind, &currentBits);
                if (status != kIOReturnSuccess || currentKind != 4) return;
                value = std::bit_cast<double>(currentBits);
            }
            if (!LevelValueFits(slot.descriptor, value)) return;
            status = slot.level->SetDecibelValue(static_cast<float>(value));
        }
        if (status == kIOReturnSuccess) slot.sequence = sequence;
        else ASFW_LOG(Audio, "[ControlChanged] HAL update failed parameter=%u kr=0x%x", parameterId, status);
        return;
    }
}
