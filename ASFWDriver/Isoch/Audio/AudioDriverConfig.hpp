#pragma once

#include <cstdint>

// Forward declarations to avoid pulling in DriverKit headers
class OSArray;
class OSBoolean;
class OSDictionary;
class OSNumber;
class OSString;

namespace ASFW::Isoch::Audio {

constexpr double kDefaultSampleRate = 48000.0;
constexpr uint32_t kDefaultChannelCount = 2;
constexpr uint32_t kMaxSampleRates = 8;
constexpr uint32_t kMaxNamedChannels = 8;
constexpr uint32_t kMaxBoolControls = 16;

constexpr uint32_t kClassIdPhantomPower = static_cast<uint32_t>('phan');
constexpr uint32_t kClassIdPhaseInvert = static_cast<uint32_t>('phsi');
constexpr uint32_t kScopeInput = static_cast<uint32_t>('inpt');

enum class StreamMode : uint32_t {
    kNonBlocking = 0,
    kBlocking = 1,
};

struct BoolControlDescriptor {
    uint32_t classIdFourCC{0};
    uint32_t scopeFourCC{0};
    uint32_t element{0};
    bool isSettable{false};
    bool initialValue{false};
};

struct ParsedAudioDriverConfig {
    uint64_t guid{0};
    uint32_t vendorId{0};
    uint32_t modelId{0};

    char deviceName[128]{};
    uint32_t channelCount{kDefaultChannelCount};
    uint32_t inputChannelCount{kDefaultChannelCount};
    uint32_t outputChannelCount{kDefaultChannelCount};

    double sampleRates[kMaxSampleRates]{};
    uint32_t sampleRateCount{1};
    double currentSampleRate{kDefaultSampleRate};

    StreamMode streamMode{StreamMode::kNonBlocking};

    bool hasPhantomOverride{false};
    uint32_t phantomSupportedMask{0};
    uint32_t phantomInitialMask{0};

    uint32_t boolControlCount{0};
    BoolControlDescriptor boolControls[kMaxBoolControls]{};

    char inputPlugName[64]{};
    char outputPlugName[64]{};
    char inputChannelNames[kMaxNamedChannels][64]{};
    char outputChannelNames[kMaxNamedChannels][64]{};
};

void InitializeAudioDriverConfigDefaults(ParsedAudioDriverConfig& outConfig);

void ParseAudioDriverConfigFromProperties(OSDictionary* properties,
                                          ParsedAudioDriverConfig& inOutConfig);

void BuildFallbackBoolControls(ParsedAudioDriverConfig& inOutConfig);

void ApplyBringupSingleFormatPolicy(ParsedAudioDriverConfig& inOutConfig);

void ClampAudioDriverChannels(ParsedAudioDriverConfig& inOutConfig,
                              uint32_t maxSupportedChannels);

[[nodiscard]] const char* ScopeLabel(uint32_t scopeFourCC);

} // namespace ASFW::Isoch::Audio
