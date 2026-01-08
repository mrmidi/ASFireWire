#include <gtest/gtest.h>
#include "ASFWDriver/Protocols/AVC/Music/MusicSubunitCapabilities.hpp"

using namespace ASFW::Protocols::AVC::Music;

// ============================================================================
// Test Fixture
// ============================================================================

class MusicSubunitCapabilitiesTest : public ::testing::Test {
protected:
    MusicSubunitCapabilities caps;
};

// ============================================================================
// Basic Capability Flags Tests
// ============================================================================

TEST_F(MusicSubunitCapabilitiesTest, HasGeneralCapability_ReturnsTrueWhenSet) {
    caps.hasGeneralCapability = true;
    EXPECT_TRUE(caps.HasGeneralCapability());
}

TEST_F(MusicSubunitCapabilitiesTest, HasGeneralCapability_ReturnsFalseWhenNotSet) {
    caps.hasGeneralCapability = false;
    EXPECT_FALSE(caps.HasGeneralCapability());
}

TEST_F(MusicSubunitCapabilitiesTest, HasAudioCapability_ReturnsTrueWhenSet) {
    caps.hasAudioCapability = true;
    EXPECT_TRUE(caps.HasAudioCapability());
}

TEST_F(MusicSubunitCapabilitiesTest, HasMidiCapability_ReturnsTrueWhenSet) {
    caps.hasMidiCapability = true;
    EXPECT_TRUE(caps.HasMidiCapability());
}

TEST_F(MusicSubunitCapabilitiesTest, HasSmpteTimeCodeCapability_ReturnsTrueWhenSet) {
    caps.hasSmpteTimeCodeCapability = true;
    EXPECT_TRUE(caps.HasSmpteTimeCodeCapability());
}

TEST_F(MusicSubunitCapabilitiesTest, HasSampleCountCapability_ReturnsTrueWhenSet) {
    caps.hasSampleCountCapability = true;
    EXPECT_TRUE(caps.HasSampleCountCapability());
}

TEST_F(MusicSubunitCapabilitiesTest, HasAudioSyncCapability_ReturnsTrueWhenSet) {
    caps.hasAudioSyncCapability = true;
    EXPECT_TRUE(caps.HasAudioSyncCapability());
}

// ============================================================================
// General Capabilities Tests (Bit Checking)
// Reference: TA 2001007, Section 5.2.1, Table 5.5
// Bit 1 = Blocking, Bit 0 = Non-blocking
// ============================================================================

TEST_F(MusicSubunitCapabilitiesTest, SupportsBlockingTransmit_ChecksBit1) {
    // Bit 1 set (0x02)
    caps.transmitCapabilityFlags = 0x02;
    EXPECT_TRUE(caps.SupportsBlockingTransmit());
    
    // Bit 1 not set
    caps.transmitCapabilityFlags = 0x01;
    EXPECT_FALSE(caps.SupportsBlockingTransmit());
    
    // No flags set
    caps.transmitCapabilityFlags = std::nullopt;
    EXPECT_FALSE(caps.SupportsBlockingTransmit());
}

TEST_F(MusicSubunitCapabilitiesTest, SupportsNonBlockingTransmit_ChecksBit0) {
    // Bit 0 set (0x01)
    caps.transmitCapabilityFlags = 0x01;
    EXPECT_TRUE(caps.SupportsNonBlockingTransmit());
    
    // Bit 0 not set
    caps.transmitCapabilityFlags = 0x02;
    EXPECT_FALSE(caps.SupportsNonBlockingTransmit());
    
    // No flags set
    caps.transmitCapabilityFlags = std::nullopt;
    EXPECT_FALSE(caps.SupportsNonBlockingTransmit());
}

TEST_F(MusicSubunitCapabilitiesTest, SupportsBlockingReceive_ChecksBit1) {
    // Bit 1 set (0x02)
    caps.receiveCapabilityFlags = 0x02;
    EXPECT_TRUE(caps.SupportsBlockingReceive());
    
    // Bit 1 not set
    caps.receiveCapabilityFlags = 0x01;
    EXPECT_FALSE(caps.SupportsBlockingReceive());
    
    // No flags set
    caps.receiveCapabilityFlags = std::nullopt;
    EXPECT_FALSE(caps.SupportsBlockingReceive());
}

TEST_F(MusicSubunitCapabilitiesTest, SupportsNonBlockingReceive_ChecksBit0) {
    // Bit 0 set (0x01)
    caps.receiveCapabilityFlags = 0x01;
    EXPECT_TRUE(caps.SupportsNonBlockingReceive());
    
    // Bit 0 not set
    caps.receiveCapabilityFlags = 0x02;
    EXPECT_FALSE(caps.SupportsNonBlockingReceive());
    
    // No flags set
    caps.receiveCapabilityFlags = std::nullopt;
    EXPECT_FALSE(caps.SupportsNonBlockingReceive());
}

TEST_F(MusicSubunitCapabilitiesTest, SupportsBlockingAndNonBlocking_BothBitsSet) {
    // Both bits set (0x03)
    caps.transmitCapabilityFlags = 0x03;
    EXPECT_TRUE(caps.SupportsBlockingTransmit());
    EXPECT_TRUE(caps.SupportsNonBlockingTransmit());
}

// ============================================================================
// SMPTE Capabilities Tests
// ============================================================================

TEST_F(MusicSubunitCapabilitiesTest, SupportsSmpteTransmit_ChecksBit1) {
    // Bit 1 set (0x02)
    caps.smpteTimeCodeCapabilityFlags = 0x02;
    EXPECT_TRUE(caps.SupportsSmpteTransmit());
    
    // Bit 1 not set
    caps.smpteTimeCodeCapabilityFlags = 0x01;
    EXPECT_FALSE(caps.SupportsSmpteTransmit());
    
    // No flags set
    caps.smpteTimeCodeCapabilityFlags = std::nullopt;
    EXPECT_FALSE(caps.SupportsSmpteTransmit());
}

TEST_F(MusicSubunitCapabilitiesTest, SupportsSmpteReceive_ChecksBit0) {
    // Bit 0 set (0x01)
    caps.smpteTimeCodeCapabilityFlags = 0x01;
    EXPECT_TRUE(caps.SupportsSmpteReceive());
    
    // Bit 0 not set
    caps.smpteTimeCodeCapabilityFlags = 0x02;
    EXPECT_FALSE(caps.SupportsSmpteReceive());
    
    // No flags set
    caps.smpteTimeCodeCapabilityFlags = std::nullopt;
    EXPECT_FALSE(caps.SupportsSmpteReceive());
}

// ============================================================================
// Sample Count Capabilities Tests
// ============================================================================

TEST_F(MusicSubunitCapabilitiesTest, SupportsSampleCountTransmit_ChecksBit1) {
    // Bit 1 set (0x02)
    caps.sampleCountCapabilityFlags = 0x02;
    EXPECT_TRUE(caps.SupportsSampleCountTransmit());
    
    // Bit 1 not set
    caps.sampleCountCapabilityFlags = 0x01;
    EXPECT_FALSE(caps.SupportsSampleCountTransmit());
    
    // No flags set
    caps.sampleCountCapabilityFlags = std::nullopt;
    EXPECT_FALSE(caps.SupportsSampleCountTransmit());
}

TEST_F(MusicSubunitCapabilitiesTest, SupportsSampleCountReceive_ChecksBit0) {
    // Bit 0 set (0x01)
    caps.sampleCountCapabilityFlags = 0x01;
    EXPECT_TRUE(caps.SupportsSampleCountReceive());
    
    // Bit 0 not set
    caps.sampleCountCapabilityFlags = 0x02;
    EXPECT_FALSE(caps.SupportsSampleCountReceive());
    
    // No flags set
    caps.sampleCountCapabilityFlags = std::nullopt;
    EXPECT_FALSE(caps.SupportsSampleCountReceive());
}

// ============================================================================
// Audio SYNC Capabilities Tests
// ============================================================================

TEST_F(MusicSubunitCapabilitiesTest, SupportsAudioSyncBus_ChecksBit0) {
    // Bit 0 set (0x01)
    caps.audioSyncCapabilityFlags = 0x01;
    EXPECT_TRUE(caps.SupportsAudioSyncBus());
    
    // Bit 0 not set
    caps.audioSyncCapabilityFlags = 0x02;
    EXPECT_FALSE(caps.SupportsAudioSyncBus());
    
    // No flags set
    caps.audioSyncCapabilityFlags = std::nullopt;
    EXPECT_FALSE(caps.SupportsAudioSyncBus());
}

TEST_F(MusicSubunitCapabilitiesTest, SupportsAudioSyncExternal_ChecksBit1) {
    // Bit 1 set (0x02)
    caps.audioSyncCapabilityFlags = 0x02;
    EXPECT_TRUE(caps.SupportsAudioSyncExternal());
    
    // Bit 1 not set
    caps.audioSyncCapabilityFlags = 0x01;
    EXPECT_FALSE(caps.SupportsAudioSyncExternal());
    
    // No flags set
    caps.audioSyncCapabilityFlags = std::nullopt;
    EXPECT_FALSE(caps.SupportsAudioSyncExternal());
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(MusicSubunitCapabilitiesTest, AllCapabilitiesDisabled_ReturnsFalse) {
    // Verify all methods return false when nothing is set
    EXPECT_FALSE(caps.HasGeneralCapability());
    EXPECT_FALSE(caps.HasAudioCapability());
    EXPECT_FALSE(caps.HasMidiCapability());
    EXPECT_FALSE(caps.HasSmpteTimeCodeCapability());
    EXPECT_FALSE(caps.HasSampleCountCapability());
    EXPECT_FALSE(caps.HasAudioSyncCapability());
    EXPECT_FALSE(caps.SupportsBlockingTransmit());
    EXPECT_FALSE(caps.SupportsNonBlockingTransmit());
    EXPECT_FALSE(caps.SupportsBlockingReceive());
    EXPECT_FALSE(caps.SupportsNonBlockingReceive());
    EXPECT_FALSE(caps.SupportsSmpteTransmit());
    EXPECT_FALSE(caps.SupportsSmpteReceive());
    EXPECT_FALSE(caps.SupportsSampleCountTransmit());
    EXPECT_FALSE(caps.SupportsSampleCountReceive());
    EXPECT_FALSE(caps.SupportsAudioSyncBus());
    EXPECT_FALSE(caps.SupportsAudioSyncExternal());
}
