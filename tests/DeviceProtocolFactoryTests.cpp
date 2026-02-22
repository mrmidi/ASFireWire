// SPDX-License-Identifier: LGPL-3.0-or-later

#include <gtest/gtest.h>

#include "Protocols/Audio/DeviceProtocolFactory.hpp"

namespace {

using ASFW::Audio::DeviceIntegrationMode;
using ASFW::Audio::DeviceProtocolFactory;

TEST(DeviceProtocolFactoryTests, SelectsIntegrationModeForKnownDevices) {
    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  DeviceProtocolFactory::kFocusriteVendorId,
                  DeviceProtocolFactory::kSPro24DspModelId),
              DeviceIntegrationMode::kHardcodedNub);

    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(
                  DeviceProtocolFactory::kApogeeVendorId,
                  DeviceProtocolFactory::kApogeeDuetModelId),
              DeviceIntegrationMode::kAVCDriven);
}

TEST(DeviceProtocolFactoryTests, RejectsUnknownDevices) {
    EXPECT_EQ(DeviceProtocolFactory::LookupIntegrationMode(0x00ABCDEF, 0x00001234),
              DeviceIntegrationMode::kNone);
    EXPECT_FALSE(DeviceProtocolFactory::IsKnownDevice(0x00ABCDEF, 0x00001234));
}

TEST(DeviceProtocolFactoryTests, RecognizesKnownVendorModelPairs) {
    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        DeviceProtocolFactory::kFocusriteVendorId,
        DeviceProtocolFactory::kSPro24DspModelId));

    EXPECT_TRUE(DeviceProtocolFactory::IsKnownDevice(
        DeviceProtocolFactory::kApogeeVendorId,
        DeviceProtocolFactory::kApogeeDuetModelId));
}

} // namespace
