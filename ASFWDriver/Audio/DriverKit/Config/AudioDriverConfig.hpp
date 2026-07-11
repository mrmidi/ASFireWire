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
// Covers high-channel-count interfaces (e.g. Midas Venice F32 = 32x32 duplex)
// so per-channel device labels can be carried for every element, not just the
// first 8. Each name is at most 64 bytes (see ParsedAudioDriverConfig).
constexpr uint32_t kMaxNamedChannels = 32;
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
    bool hasExplicitInputChannelCount{false};
    bool hasExplicitOutputChannelCount{false};

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

    // Per-channel device labels published by the core side (empty string =
    // none for that slot). BuildChannelNamesFromPlugs prefers these over the
    // synthesized "<plug> N" names so CoreAudio shows the device's real labels.
    char deviceInputChannelNames[kMaxNamedChannels][64]{};
    char deviceOutputChannelNames[kMaxNamedChannels][64]{};
};

void InitializeAudioDriverConfigDefaults(ParsedAudioDriverConfig& outConfig);

void ParseAudioDriverConfigFromProperties(OSDictionary* properties,
                                          ParsedAudioDriverConfig& inOutConfig);

// Fill inputChannelNames/outputChannelNames for the current channel counts,
// preferring per-channel device labels (deviceInput/OutputChannelNames) and
// falling back to synthesized "<plug> N". Idempotent; safe to re-run after the
// channel counts change.
void BuildChannelNamesFromPlugs(ParsedAudioDriverConfig& inOutConfig);

void BuildFallbackBoolControls(ParsedAudioDriverConfig& inOutConfig);

void ApplyBringupSingleFormatPolicy(ParsedAudioDriverConfig& inOutConfig);

void ClampAudioDriverChannels(ParsedAudioDriverConfig& inOutConfig,
                              uint32_t maxSupportedChannels);

// Device-published runtime counts win. Profile counts are fallback geometry for
// older nubs that do not publish directional channel properties.
void ApplyProfileChannelCountFallback(ParsedAudioDriverConfig& inOutConfig,
                                      uint32_t profileInputChannels,
                                      uint32_t profileOutputChannels);

[[nodiscard]] const char* ScopeLabel(uint32_t scopeFourCC);

} // namespace ASFW::Isoch::Audio
