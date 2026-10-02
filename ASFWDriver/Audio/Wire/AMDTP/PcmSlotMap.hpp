// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../../../Common/PcmSlotMap.hpp"
#include "AmdtpRateGeometry.hpp"
namespace ASFW::Audio::Wire {
using PcmSlotMap = Common::PcmSlotMap;
static_assert(Encoding::kMaxPcmChannels == Common::kMaxPcmSlots);
}
