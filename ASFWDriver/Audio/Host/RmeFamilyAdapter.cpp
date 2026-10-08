// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// RmeFamilyAdapter.cpp - see RmeFamilyAdapter.hpp.

#include "RmeFamilyAdapter.hpp"

#include "../Protocols/IDeviceProtocol.hpp"

namespace ASFW::Audio::Host {

void RmeFamilyAdapter::Describe(const DescribeInput& in, DescribeDone done) {
    // The host only routes RME devices here; refuse anything else rather than
    // publish a Fireface description for it.
    if (!in.policy ||
        in.policy->plan.family != DeviceProfiles::Audio::AudioFamilyProviderId::RmeRegister) {
        done(DescribeRefusal{kIOReturnUnsupported, "not-an-rme-policy"});
        return;
    }
    // RmeAudioBackend published only once a protocol existed; the next record
    // update or restart describes again.
    if (!in.protocol) {
        done(DescribeRefusal{kIOReturnNotReady, "no-protocol"});
        return;
    }
    done(BuildNubConfig(in.record, in.policy->plan.profileBuilder, in.protocol->GetName()));
}

} // namespace ASFW::Audio::Host
