// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "CoreAudioProjection.hpp"
#include <DriverKit/OSArray.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSString.h>
#include <bit>
#include <limits>
namespace ASFW::Audio::Controls {
inline constexpr const char* kControlsProperty = "ASFWParameterControls";
inline constexpr uint32_t kMaxControls = 128;
inline OSSharedPtr<OSArray> EncodeControls(const std::vector<CoreAudioControl>& controls) {
    auto result = OSSharedPtr(OSArray::withCapacity(static_cast<uint32_t>(controls.size())), OSNoRetain);
    if (!result || controls.size() > kMaxControls) return nullptr;
    for (const auto& c : controls) {
        auto d = OSSharedPtr(OSDictionary::withCapacity(12), OSNoRetain);
        if (!d) return nullptr;
        const auto number = [&](const char* key, uint64_t bits) {
            auto n = OSSharedPtr(OSNumber::withNumber(bits, 64), OSNoRetain);
            if (!n) return false;
            d->setObject(key, n.get()); return true;
        };
        auto name = OSSharedPtr(OSString::withCString(c.name.c_str()), OSNoRetain);
        if (!name || !number("Parameter", c.parameter.value) || !number("Kind", static_cast<uint8_t>(c.kind)) ||
            !number("Class", c.classId) || !number("Scope", c.scope) || !number("Element", c.element) ||
            !number("Settable", c.settable) || !number("Normalized", c.normalized)) return nullptr;
        d->setObject("Name", name.get());
        if (const auto* scalar = std::get_if<AudioModel::ScalarDomain>(&c.domain)) {
            if (!number("Minimum", std::bit_cast<uint64_t>(scalar->min)) ||
                !number("Maximum", std::bit_cast<uint64_t>(scalar->max)) ||
                !number("Step", std::bit_cast<uint64_t>(scalar->step.value_or(0))) ||
                !number("Unit", static_cast<uint8_t>(scalar->unit)) ||
                !number("Value", std::bit_cast<uint64_t>(std::get<double>(c.value)))) return nullptr;
        } else if (const auto* choices = std::get_if<AudioModel::EnumDomain>(&c.domain)) {
            auto values = OSSharedPtr(OSArray::withCapacity(static_cast<uint32_t>(choices->values.size())), OSNoRetain);
            if (!values || choices->values.size() > 256) return nullptr;
            for (const auto& item : choices->values) {
                auto entry = OSSharedPtr(OSDictionary::withCapacity(2), OSNoRetain);
                auto label = OSSharedPtr(OSString::withCString(item.name.c_str()), OSNoRetain);
                auto value = OSSharedPtr(OSNumber::withNumber(static_cast<uint64_t>(item.value), 64), OSNoRetain);
                if (!entry || !label || !value) return nullptr;
                entry->setObject("Name", label.get()); entry->setObject("Value", value.get());
                values->setObject(entry.get());
            }
            d->setObject("Choices", values.get());
            if (!number("Value", static_cast<uint64_t>(std::get<int64_t>(c.value)))) return nullptr;
        } else if (!number("Value", std::get<bool>(c.value))) return nullptr;
        result->setObject(d.get());
    }
    return result;
}
inline std::vector<CoreAudioControl> DecodeControls(OSArray* array) {
    std::vector<CoreAudioControl> result;
    if (!array || array->getCount() > kMaxControls) return result;
    for (uint32_t k = 0; k < array->getCount(); ++k) {
        auto* d = OSDynamicCast(OSDictionary, array->getObject(k));
        if (!d) return {};
        const auto get = [&](const char* key) -> uint64_t {
            auto* n = OSDynamicCast(OSNumber, d->getObject(key)); return n != nullptr ? n->unsigned64BitValue() : 0;
        };
        auto* name = OSDynamicCast(OSString, d->getObject("Name"));
        if (!name || get("Parameter") > UINT32_MAX || get("Kind") > 2) return {};
        CoreAudioControl c{AudioModel::ParameterId{static_cast<uint32_t>(get("Parameter"))},
            static_cast<CoreAudioControlKind>(get("Kind")), static_cast<uint32_t>(get("Class")),
            static_cast<uint32_t>(get("Scope")), static_cast<uint32_t>(get("Element")),
            get("Settable") != 0, name->getCStringNoCopy(), AudioModel::BooleanDomain{}, false, get("Normalized") != 0};
        switch (c.kind) {
            case CoreAudioControlKind::Boolean: c.value = get("Value") != 0; break;
            case CoreAudioControlKind::Level: {
                const double step = std::bit_cast<double>(get("Step"));
                c.domain = AudioModel::ScalarDomain{std::bit_cast<double>(get("Minimum")),
                    std::bit_cast<double>(get("Maximum")), step > 0 ? std::optional{step} : std::nullopt,
                    static_cast<AudioModel::ScalarUnit>(get("Unit"))};
                c.value = std::bit_cast<double>(get("Value"));
                if (!ScalarToPosition(std::get<AudioModel::ScalarDomain>(c.domain), std::get<double>(c.value))) return {};
                break;
            }
            case CoreAudioControlKind::Selector: {
                auto* items = OSDynamicCast(OSArray, d->getObject("Choices"));
                if (!items || items->getCount() == 0 || items->getCount() > 256) return {};
                AudioModel::EnumDomain domain;
                for (uint32_t i = 0; i < items->getCount(); ++i) {
                    auto* entry = OSDynamicCast(OSDictionary, items->getObject(i));
                    auto* label = entry ? OSDynamicCast(OSString, entry->getObject("Name")) : nullptr;
                    auto* value = entry ? OSDynamicCast(OSNumber, entry->getObject("Value")) : nullptr;
                    if (!label || value == nullptr) return {};
                    domain.values.push_back({static_cast<int64_t>(value->unsigned64BitValue()), label->getCStringNoCopy()});
                }
                c.domain = std::move(domain); c.value = static_cast<int64_t>(get("Value")); break;
            }
        }
        result.push_back(std::move(c));
    }
    return result;
}
} // namespace ASFW::Audio::Controls
