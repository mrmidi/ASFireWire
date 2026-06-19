// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// CSRHardwareAdapters.hpp — production adapters binding CSRResponder's injected
// interfaces to the real OHCI HardwareInterface. Not compiled under ASFW_HOST_TEST
// (the tests use fakes instead).

#pragma once

#include "CSRResponder.hpp"

#include "../../Hardware/HardwareInterface.hpp"
#include "../../Hardware/RegisterMap.hpp"

namespace ASFW::Bus {

// Local-root status from the OHCI NodeID register (NodeIDBits::kRoot).
class HardwareRootStatus final : public IRootStatus {
public:
    explicit HardwareRootStatus(ASFW::Driver::HardwareInterface* hw) noexcept : hw_(hw) {}

    [[nodiscard]] bool IsLocalRoot() const noexcept override {
        if (hw_ == nullptr) {
            return false;
        }
        return (hw_->Read(ASFW::Driver::Register32::kNodeID) & ASFW::Driver::NodeIDBits::kRoot) != 0U;
    }

private:
    ASFW::Driver::HardwareInterface* hw_;
};

// Cycle-master control via OHCI LinkControlSet/Clear (LinkControlBits::kCycleMaster).
class HardwareCycleMasterControl final : public ICycleMasterControl {
public:
    explicit HardwareCycleMasterControl(ASFW::Driver::HardwareInterface* hw) noexcept : hw_(hw) {}

    void SetCycleMaster(bool enable) noexcept override {
        if (hw_ == nullptr) {
            return;
        }
        if (enable) {
            hw_->SetLinkControlBits(ASFW::Driver::LinkControlBits::kCycleMaster);
        } else {
            hw_->ClearLinkControlBits(ASFW::Driver::LinkControlBits::kCycleMaster);
        }
    }

    [[nodiscard]] bool IsCycleMasterEnabled() const noexcept override {
        if (hw_ == nullptr) {
            return false;
        }
        return (hw_->ReadLinkControl() & ASFW::Driver::LinkControlBits::kCycleMaster) != 0U;
    }

private:
    ASFW::Driver::HardwareInterface* hw_;
};

// Physical bus reset trigger adapter.
class HardwareBusResetTrigger final : public IBusResetTrigger {
public:
    explicit HardwareBusResetTrigger(ASFW::Driver::HardwareInterface* hw) noexcept : hw_(hw) {}

    void TriggerBusReset(bool shortReset) noexcept override {
        if (hw_ == nullptr) {
            return;
        }
        hw_->InitiateBusReset(shortReset);
    }

private:
    ASFW::Driver::HardwareInterface* hw_;
};

} // namespace ASFW::Bus
