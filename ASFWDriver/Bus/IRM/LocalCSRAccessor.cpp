// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// LocalCSRAccessor.cpp — see LocalCSRAccessor.hpp

#include "LocalCSRAccessor.hpp"

namespace ASFW::Bus {

using namespace ASFW::Driver::IRMCSR;

ASFW::Driver::LocalCSRReadResult LocalCSRAccessor::ReadBusManagerId() noexcept {
    return hw_.ReadLocalIRMResource(static_cast<uint32_t>(CSRSelector::BusManagerId));
}

ASFW::Driver::LocalCSRReadResult LocalCSRAccessor::ReadBandwidthAvailable() noexcept {
    return hw_.ReadLocalIRMResource(static_cast<uint32_t>(CSRSelector::BandwidthAvailable));
}

ASFW::Driver::LocalCSRReadResult LocalCSRAccessor::ReadChannelsAvailableHi() noexcept {
    return hw_.ReadLocalIRMResource(static_cast<uint32_t>(CSRSelector::ChannelsAvailableHi));
}

ASFW::Driver::LocalCSRReadResult LocalCSRAccessor::ReadChannelsAvailableLo() noexcept {
    return hw_.ReadLocalIRMResource(static_cast<uint32_t>(CSRSelector::ChannelsAvailableLo));
}

ASFW::Driver::LocalCSRLockResult LocalCSRAccessor::ProbeBusManagerIdNoChange(uint32_t expected) noexcept {
    return hw_.CompareSwapLocalIRMResource(static_cast<uint32_t>(CSRSelector::BusManagerId), expected, expected);
}

ASFW::Driver::LocalCSRLockResult LocalCSRAccessor::ProbeBandwidthNoChange(uint32_t expected) noexcept {
    return hw_.CompareSwapLocalIRMResource(static_cast<uint32_t>(CSRSelector::BandwidthAvailable), expected, expected);
}

ASFW::Driver::LocalCSRLockResult LocalCSRAccessor::ProbeChannelsHiNoChange(uint32_t expected) noexcept {
    return hw_.CompareSwapLocalIRMResource(static_cast<uint32_t>(CSRSelector::ChannelsAvailableHi), expected, expected);
}

ASFW::Driver::LocalCSRLockResult LocalCSRAccessor::ProbeChannelsLoNoChange(uint32_t expected) noexcept {
    return hw_.CompareSwapLocalIRMResource(static_cast<uint32_t>(CSRSelector::ChannelsAvailableLo), expected, expected);
}

ASFW::Driver::LocalCSRLockResult LocalCSRAccessor::CompareSwapBusManagerId(uint32_t compareValue,
                                                                          uint32_t newValue) noexcept {
    return hw_.CompareSwapLocalIRMResource(static_cast<uint32_t>(CSRSelector::BusManagerId), compareValue, newValue);
}

} // namespace ASFW::Bus
