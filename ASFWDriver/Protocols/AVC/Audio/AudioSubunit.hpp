//
// AudioSubunit.hpp
// ASFWDriver - AV/C Protocol Layer
//
// Audio Subunit (type 0x01) implementation
//

#pragma once

#include "../Subunit.hpp"
#include "../Commands/StreamFormatCommand.hpp"
#include "../Descriptors/AudioSubunitDescriptor.hpp"
#include <vector>
#include <optional>
#include "../Discovery/DiscoverySnapshot.hpp"

namespace ASFW::Protocols::AVC::Audio {

/// Audio plug information
struct AudioPlugInfo {
    uint8_t plugNumber{0};
    bool isInput{false};
    std::optional<ASFW::AVC::Cmd::StreamFormat> currentFormat;
    std::vector<ASFW::AVC::Cmd::StreamFormat> supportedFormats;
};

/// Audio Subunit class
class AudioSubunit : public Subunit {
public:
    AudioSubunit(AVCSubunitType type, uint8_t id)
        : Subunit(type, id) {}
    
    std::string GetName() const override { return "Audio"; }
    
    void LoadSnapshot(const ASFW::AVC::DiscoveryEngine::DiscoverySnapshot& snapshot);

    // Accessors
    uint8_t GetNumInputPlugs() const { return numInputPlugs_; }
    uint8_t GetNumOutputPlugs() const { return numOutputPlugs_; }
    const std::vector<AudioPlugInfo>& GetInputPlugs() const { return inputPlugs_; }
    const std::vector<AudioPlugInfo>& GetOutputPlugs() const { return outputPlugs_; }

    /// Parsed Audio Subunit Identifier Descriptor (§5.1, §8.1)
    const std::optional<Descriptors::AudioSubunitIdentifier>& GetIdentifier() const noexcept {
        return identifier_;
    }

    /// Raw descriptor data read during discovery
    const std::optional<std::vector<uint8_t>>& GetDescriptorData() const noexcept {
        return descriptorData_;
    }

private:
    uint8_t numInputPlugs_{0};
    uint8_t numOutputPlugs_{0};
    std::vector<AudioPlugInfo> inputPlugs_;
    std::vector<AudioPlugInfo> outputPlugs_;
    std::optional<Descriptors::AudioSubunitIdentifier> identifier_;
    std::optional<std::vector<uint8_t>> descriptorData_;

};

} // namespace ASFW::Protocols::AVC::Audio
