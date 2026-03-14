// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// SPro24DspProtocol.cpp - Focusrite Saffire Pro 24 DSP protocol implementation

#include "SPro24DspProtocol.hpp"
#include "SPro24DspRouting.hpp"
#include "../Core/DICENotificationMailbox.hpp"
#include "../../../../Logging/Logging.hpp"
#include <DriverKit/IOLib.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ASFW::Audio::DICE::Focusrite {

// ============================================================================
// Wire Format Helpers
// ============================================================================

namespace {

namespace Routing = ASFW::Audio::DICE::Focusrite::SPro24DspRouting;

constexpr uint32_t kAsyncTimeoutMs = 3000;
constexpr uint32_t kPollIntervalMs = 10;
constexpr uint32_t kReadyTimeoutMs = 200;
constexpr uint32_t kTxSpeedS400 = 2;
constexpr uint32_t kMaxRouterEntries = 128;
constexpr uint32_t kDisabledIsoChannel = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kRxSeqStartDefault = 0;
constexpr uint32_t kRoutingRefreshDelayMs = 100;
constexpr size_t kHexPreviewBytes = 48;

std::atomic<uint32_t> gBringupTraceDepth{0};

bool BringupTraceEnabled() noexcept {
    return gBringupTraceDepth.load(std::memory_order_relaxed) != 0;
}

std::string HexPreview(const uint8_t* data, size_t size, size_t maxBytes = kHexPreviewBytes) {
    if (data == nullptr || size == 0) {
        return "<empty>";
    }

    const size_t previewBytes = (size < maxBytes) ? size : maxBytes;
    std::string text;
    text.reserve(previewBytes * 3 + 16);

    for (size_t i = 0; i < previewBytes; ++i) {
        char chunk[4];
        std::snprintf(chunk, sizeof(chunk), "%02x", data[i]);
        if (i != 0) {
            text.push_back(' ');
        }
        text.append(chunk);
    }

    if (previewBytes < size) {
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), " ... (%zu bytes)", size);
        text.append(suffix);
    }

    return text;
}

struct ScopedBringupTrace {
    explicit ScopedBringupTrace(const char* phase)
        : phase_(phase) {
        const uint32_t depth = gBringupTraceDepth.fetch_add(1, std::memory_order_relaxed) + 1;
        ASFW_LOG(DICE,
                 "TRACE begin %{public}s (depth=%u)",
                 phase_,
                 depth);
    }

    ~ScopedBringupTrace() {
        const uint32_t depth = gBringupTraceDepth.fetch_sub(1, std::memory_order_relaxed) - 1;
        ASFW_LOG(DICE,
                 "TRACE end %{public}s (depth=%u)",
                 phase_,
                 depth);
    }

private:
    const char* phase_;
};

struct SyncVoidState {
    std::atomic<bool> done{false};
    std::atomic<IOReturn> status{kIOReturnTimeout};
};

template <typename StartFn>
IOReturn WaitForVoid(StartFn&& fn, uint32_t timeoutMs = kAsyncTimeoutMs) {
    auto state = std::make_shared<SyncVoidState>();
    fn([state](IOReturn status) {
        state->status.store(status, std::memory_order_relaxed);
        state->done.store(true, std::memory_order_release);
    });

    for (uint32_t waited = 0; waited < timeoutMs; waited += kPollIntervalMs) {
        if (state->done.load(std::memory_order_acquire)) {
            return state->status.load(std::memory_order_relaxed);
        }
        IOSleep(kPollIntervalMs);
    }

    return state->done.load(std::memory_order_acquire)
        ? state->status.load(std::memory_order_relaxed)
        : kIOReturnTimeout;
}

template <typename T>
struct SyncValueState {
    std::atomic<bool> done{false};
    std::atomic<IOReturn> status{kIOReturnTimeout};
    T value{};
};

template <typename T, typename StartFn>
std::pair<IOReturn, T> WaitForValue(StartFn&& fn, uint32_t timeoutMs = kAsyncTimeoutMs) {
    auto state = std::make_shared<SyncValueState<T>>();
    fn([state](IOReturn status, T value) {
        state->value = std::move(value);
        state->status.store(status, std::memory_order_relaxed);
        state->done.store(true, std::memory_order_release);
    });

    for (uint32_t waited = 0; waited < timeoutMs; waited += kPollIntervalMs) {
        if (state->done.load(std::memory_order_acquire)) {
            return {
                state->status.load(std::memory_order_relaxed),
                std::move(state->value),
            };
        }
        IOSleep(kPollIntervalMs);
    }

    return {
        state->done.load(std::memory_order_acquire)
            ? state->status.load(std::memory_order_relaxed)
            : kIOReturnTimeout,
        std::move(state->value),
    };
}

uint32_t Am824SlotsFor(const StreamFormatEntry& entry) noexcept {
    return entry.Am824Slots();
}

void LogDiceStreamEntryDetail(const char* dir, uint32_t index, const StreamFormatEntry& entry) {
    if (entry.hasSeqStart) { // NOSONAR(cpp:S3923): branches log different diagnostic messages
        ASFW_LOG(DICE,
                 "  %{public}s[%u]: iso=%d start=%u pcm=%u midi=%u am824Slots=%u labels='%{public}s'",
                 dir,
                 index,
                 entry.isoChannel,
                 entry.seqStart,
                 entry.pcmChannels,
                 entry.midiPorts,
                 Am824SlotsFor(entry),
                 entry.labels);
    } else {
        ASFW_LOG(DICE,
                 "  %{public}s[%u]: iso=%d speed=%u pcm=%u midi=%u am824Slots=%u labels='%{public}s'",
                 dir,
                 index,
                 entry.isoChannel,
                 entry.speed,
                 entry.pcmChannels,
                 entry.midiPorts,
                 Am824SlotsFor(entry),
                 entry.labels);
    }
}

float FloatFromWire(const uint8_t* data) {
    uint32_t bits = DICETransaction::QuadletFromWire(data);
    float f;
    static_assert(sizeof(float) == sizeof(uint32_t));
    __builtin_memcpy(&f, &bits, sizeof(float));
    return f;
}

void FloatToWire(float value, uint8_t* data) {
    uint32_t bits;
    static_assert(sizeof(float) == sizeof(uint32_t));
    __builtin_memcpy(&bits, &value, sizeof(float));
    DICETransaction::QuadletToWire(bits, data);
}

std::pair<IOReturn, uint32_t> ReadQuadletSync(DICETransaction& tx, uint32_t offset) {
    auto result = WaitForValue<uint32_t>([&tx, offset](auto callback) {
        tx.ReadQuadlet(offset, std::move(callback));
    });
    if (BringupTraceEnabled()) {
        ASFW_LOG(DICE,
                 "TRACE rd32 off=0x%08x status=0x%x value=0x%08x",
                 offset,
                 result.first,
                 result.second);
    }
    return result;
}

IOReturn WriteQuadletSync(DICETransaction& tx, uint32_t offset, uint32_t value) {
    const IOReturn status = WaitForVoid([&tx, offset, value](auto callback) {
        tx.WriteQuadlet(offset, value, std::move(callback));
    });
    if (BringupTraceEnabled()) {
        ASFW_LOG(DICE,
                 "TRACE wr32 off=0x%08x value=0x%08x status=0x%x",
                 offset,
                 value,
                 status);
    }
    return status;
}

std::pair<IOReturn, uint64_t> CompareSwapOctletSync(DICETransaction& tx,
                                                    uint32_t offset,
                                                    uint64_t expected,
                                                    uint64_t desired) {
    auto result = WaitForValue<uint64_t>([&tx, offset, expected, desired](auto callback) {
        tx.CompareSwapOctlet(offset, expected, desired, std::move(callback));
    });
    if (BringupTraceEnabled()) {
        ASFW_LOG(DICE,
                 "TRACE lock64 off=0x%08x expect=0x%016llx desire=0x%016llx status=0x%x prev=0x%016llx",
                 offset,
                 expected,
                 desired,
                 result.first,
                 result.second);
    }
    return result;
}

std::pair<IOReturn, std::vector<uint8_t>> ReadBlockSync(DICETransaction& tx,
                                                        uint32_t offset,
                                                        size_t size) {
    auto result = WaitForValue<std::vector<uint8_t>>(
        [&tx, offset, size](auto callback) {
            tx.ReadBlock(offset,
                         size,
                         [callback = std::move(callback), size](IOReturn status,
                                                                const uint8_t* data,
                                                                size_t actualSize) mutable {
                             std::vector<uint8_t> bytes;
                             if (status == kIOReturnSuccess && data != nullptr) {
                                 const size_t copyBytes = (actualSize < size) ? actualSize : size;
                                 bytes.assign(data, data + copyBytes);
                             }
                             callback(status, std::move(bytes));
                         });
        });
    if (BringupTraceEnabled()) {
        ASFW_LOG(DICE,
                 "TRACE rdblk off=0x%08x req=%zu status=0x%x got=%zu preview=%{public}s",
                 offset,
                 size,
                 result.first,
                 result.second.size(),
                 HexPreview(result.second.data(), result.second.size()).c_str());
    }
    return result;
}

IOReturn WriteBlockSync(DICETransaction& tx,
                        uint32_t offset,
                        const uint8_t* data,
                        size_t size) {
    const IOReturn status = WaitForVoid([&tx, offset, data, size](auto callback) {
        tx.WriteBlock(offset, data, size, std::move(callback));
    });
    if (BringupTraceEnabled()) {
        ASFW_LOG(DICE,
                 "TRACE wrblk off=0x%08x size=%zu status=0x%x preview=%{public}s",
                 offset,
                 size,
                 status,
                 HexPreview(data, size).c_str());
    }
    return status;
}

Routing::RouterEntry ParseRouterEntry(const uint8_t* data) {
    const uint32_t value = DICETransaction::QuadletFromWire(data);
    Routing::RouterEntry entry;
    entry.dst.blockId = static_cast<uint8_t>((value >> 4) & 0x0FU);
    entry.dst.channel = static_cast<uint8_t>(value & 0x0FU);
    entry.src.blockId = static_cast<uint8_t>((value >> 12) & 0x0FU);
    entry.src.channel = static_cast<uint8_t>((value >> 8) & 0x0FU);
    entry.peak = static_cast<uint16_t>((value >> 16) & 0xFFFFU);
    return entry;
}

void SerializeRouterEntry(const Routing::RouterEntry& entry, uint8_t* data) {
    const uint32_t value =
        (static_cast<uint32_t>(entry.peak) << 16) |
        (static_cast<uint32_t>((entry.src.blockId << 4) | (entry.src.channel & 0x0F)) << 8) |
        static_cast<uint32_t>((entry.dst.blockId << 4) | (entry.dst.channel & 0x0F));
    DICETransaction::QuadletToWire(value, data);
}

void LogIns0PlaybackMap() {
    ASFW_LOG(DICE, "  Ins0[0/1] = %{public}s", Routing::DescribeIns0Destination(0).data());
    ASFW_LOG(DICE, "  Ins0[2/3] = %{public}s", Routing::DescribeIns0Destination(2).data());
    ASFW_LOG(DICE, "  Ins0[4/5] = %{public}s", Routing::DescribeIns0Destination(4).data());
}

void LogPlaybackMirrorState(const char* prefix, const std::vector<Routing::RouterEntry>& entries) {
    ASFW_LOG(DICE,
             "%{public}s monitor12=%{public}s hp1=%{public}s hp2=%{public}s",
             prefix,
             Routing::HasStereoPlaybackMirror(entries, Routing::kMonitor12Mirror) ? "yes" : "no",
             Routing::HasStereoPlaybackMirror(entries, Routing::kHeadphone1Mirror) ? "yes" : "no",
             Routing::HasStereoPlaybackMirror(entries, Routing::kHeadphone2Mirror) ? "yes" : "no");
}

void LogRouterEntriesDetailed(const char* prefix,
                              const std::vector<Routing::RouterEntry>& entries) {
    ASFW_LOG(DICE, "%{public}s count=%zu", prefix, entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        ASFW_LOG(DICE,
                 "  [%zu] dst=%u:%u src=%u:%u peak=0x%04x",
                 i,
                 entry.dst.blockId,
                 entry.dst.channel,
                 entry.src.blockId,
                 entry.src.channel,
                 entry.peak);
    }
}

void LogOutputGroupState(const char* prefix, const OutputGroupState& group) {
    ASFW_LOG(DICE,
             "%{public}s mute=%{public}s dim=%{public}s knob=%d",
             prefix,
             group.muteEnabled ? "on" : "off",
             group.dimEnabled ? "on" : "off",
             group.hwKnobValue);
    for (size_t base = 0; base < group.volumes.size(); base += 2) {
        ASFW_LOG(DICE,
                 "  outs %zu/%zu vol=%d/%d mute=%{public}s/%{public}s hwVol=%{public}s/%{public}s hwMute=%{public}s/%{public}s hwDim=%{public}s/%{public}s",
                 base + 1,
                 base + 2,
                 group.volumes[base],
                 group.volumes[base + 1],
                 group.volMutes[base] ? "on" : "off",
                 group.volMutes[base + 1] ? "on" : "off",
                 group.volHwCtls[base] ? "yes" : "no",
                 group.volHwCtls[base + 1] ? "yes" : "no",
                 group.muteHwCtls[base] ? "yes" : "no",
                 group.muteHwCtls[base + 1] ? "yes" : "no",
                 group.dimHwCtls[base] ? "yes" : "no",
                 group.dimHwCtls[base + 1] ? "yes" : "no");
    }
}

void LogGlobalStatusSummary(const char* prefix,
                            uint32_t notification,
                            uint32_t status,
                            uint32_t extStatus) {
    ASFW_LOG(DICE,
             "%{public}s notify=0x%08x status=0x%08x ext=0x%08x sourceLocked=%{public}s nominalRate=%u arx1Locked=%{public}s arx1Slip=%{public}s",
             prefix,
             notification,
             status,
             extStatus,
             IsSourceLocked(status) ? "yes" : "no",
             NominalRateHz(status),
             IsArx1Locked(extStatus) ? "yes" : "no",
             HasArx1Slip(extStatus) ? "yes" : "no");
}

uint64_t BuildOwnerValue(FW::NodeId localNode) noexcept {
    const uint64_t localNodeId = 0xFFC0ULL | static_cast<uint64_t>(localNode.value & 0x3FU);
    return (localNodeId << kOwnerNodeShift) | NotificationMailbox::kHandlerOffset;
}

} // anonymous namespace

// ============================================================================
// CompressorState
// ============================================================================

CompressorState CompressorState::FromWire(const uint8_t* data) {
    CompressorState s;

    // Per Linux reference (spro24dsp.rs), quad 0 (offset 0x00) is reserved
    // (always 0x3f800000 = 1.0f). Actual coefficients start at offset 0x04.
    for (size_t ch = 0; ch < 2; ++ch) {
        const uint8_t* block = data + ch * kCoefBlockSize;
        s.output[ch]    = FloatFromWire(block + 0x04);
        s.threshold[ch] = FloatFromWire(block + 0x08);
        s.ratio[ch]     = FloatFromWire(block + 0x0C);
        s.attack[ch]    = FloatFromWire(block + 0x10);
        s.release[ch]   = FloatFromWire(block + 0x14);
    }

    return s;
}

void CompressorState::ToWire(uint8_t* data) const {
    // Per Linux reference (spro24dsp.rs), quad 0 (offset 0x00) is reserved.
    // Write 1.0f to the reserved field, then actual coefficients at 0x04+.
    for (size_t ch = 0; ch < 2; ++ch) {
        uint8_t* block = data + ch * kCoefBlockSize;
        FloatToWire(1.0f,          block + 0x00);  // reserved (always 1.0)
        FloatToWire(output[ch],    block + 0x04);
        FloatToWire(threshold[ch], block + 0x08);
        FloatToWire(ratio[ch],     block + 0x0C);
        FloatToWire(attack[ch],    block + 0x10);
        FloatToWire(release[ch],   block + 0x14);
    }
}

// ============================================================================
// ReverbState
// ============================================================================

ReverbState ReverbState::FromWire(const uint8_t* data) {
    ReverbState s;
    s.size = FloatFromWire(data + 0x70);
    s.air = FloatFromWire(data + 0x74);
    
    float on = FloatFromWire(data + 0x78);
    s.enabled = on > 0.5f;
    
    float mag = FloatFromWire(data + 0x80);
    float sign = FloatFromWire(data + 0x84);
    s.preFilter = (sign >= 0.5f) ? mag : -mag;
    
    return s;
}

void ReverbState::ToWire(uint8_t* data) const {
    FloatToWire(size, data + 0x70);
    FloatToWire(air, data + 0x74);
    FloatToWire(enabled ? 1.0f : 0.0f, data + 0x78);
    FloatToWire(enabled ? 0.0f : 1.0f, data + 0x7C);
    FloatToWire((preFilter < 0.0f) ? -preFilter : preFilter, data + 0x80);
    FloatToWire((preFilter >= 0.0f) ? 1.0f : 0.0f, data + 0x84);
}

// ============================================================================
// EffectGeneralParams
// ============================================================================

EffectGeneralParams EffectGeneralParams::FromWire(const uint8_t* data) {
    EffectGeneralParams p;
    uint32_t flags = DICETransaction::QuadletFromWire(data);

    // Two-half-word layout per Linux reference (spro24dsp.rs):
    //   Ch0 in bits  0-2:  bit0 = EQ enable, bit1 = Comp enable, bit2 = EQ after comp
    //   Ch1 in bits 16-18: bit16= EQ enable, bit17= Comp enable, bit18= EQ after comp
    for (size_t ch = 0; ch < 2; ++ch) {
        uint16_t chFlags = static_cast<uint16_t>(flags >> (ch * 16));
        p.eqEnable[ch]    = (chFlags & 0x0001) != 0;
        p.compEnable[ch]  = (chFlags & 0x0002) != 0;
        p.eqAfterComp[ch] = (chFlags & 0x0004) != 0;
    }

    return p;
}

void EffectGeneralParams::ToWire(uint8_t* data) const {
    uint32_t flags = 0;

    // Two-half-word layout per Linux reference (spro24dsp.rs):
    //   Ch0 in bits  0-2:  bit0 = EQ enable, bit1 = Comp enable, bit2 = EQ after comp
    //   Ch1 in bits 16-18: bit16= EQ enable, bit17= Comp enable, bit18= EQ after comp
    for (size_t ch = 0; ch < 2; ++ch) {
        uint16_t chFlags = 0;
        if (eqEnable[ch])    chFlags |= 0x0001;
        if (compEnable[ch])  chFlags |= 0x0002;
        if (eqAfterComp[ch]) chFlags |= 0x0004;
        flags |= static_cast<uint32_t>(chFlags) << (ch * 16);
    }

    DICETransaction::QuadletToWire(flags, data);
}

// ============================================================================
// SPro24DspProtocol Implementation
// ============================================================================

SPro24DspProtocol::SPro24DspProtocol(Protocols::Ports::FireWireBusOps& busOps,
                                     Protocols::Ports::FireWireBusInfo& busInfo,
                                     uint16_t nodeId)
    : busInfo_(busInfo)
    , tx_(busOps, busInfo, nodeId)
{
    ASFW_LOG(DICE, "SPro24DspProtocol created for node 0x%04x", nodeId);
}

bool SPro24DspProtocol::GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
    if (!runtimeCapsValid_.load(std::memory_order_acquire)) {
        return false;
    }

    outCaps.sampleRateHz = runtimeSampleRateHz_.load(std::memory_order_relaxed);
    outCaps.hostInputPcmChannels = hostInputPcmChannels_.load(std::memory_order_relaxed);
    outCaps.hostOutputPcmChannels = hostOutputPcmChannels_.load(std::memory_order_relaxed);
    outCaps.deviceToHostAm824Slots = deviceToHostAm824Slots_.load(std::memory_order_relaxed);
    outCaps.hostToDeviceAm824Slots = hostToDeviceAm824Slots_.load(std::memory_order_relaxed);
    return true;
}

IOReturn SPro24DspProtocol::Initialize() {
    // Start async initialization - this will trigger capability discovery
    InitializeAsync([](IOReturn status) {
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "SPro24DspProtocol async initialization failed: 0x%x", status);
        }
    });
    return kIOReturnSuccess;
}

void SPro24DspProtocol::InitializeAsync(InitCallback callback) {
    ASFW_LOG(DICE, "SPro24DspProtocol::InitializeAsync starting capability discovery");

    tx_.ReadCapabilities([this, callback = std::move(callback)](IOReturn status, DICECapabilities caps) mutable {
        HandleCapabilityDiscovery(status, std::move(caps), std::move(callback));
    });
}

void SPro24DspProtocol::HandleCapabilityDiscovery(IOReturn status,
                                                  DICECapabilities caps,
                                                  InitCallback callback) {
    if (status != kIOReturnSuccess) {
        ASFW_LOG(DICE, "Failed to read DICE capabilities: 0x%x", status);
        callback(status);
        return;
    }

    tx_.ReadGeneralSections(
        [this, callback = std::move(callback), caps = std::move(caps)](IOReturn sectionsStatus,
                                                                       GeneralSections sections) mutable {
            HandleGeneralSectionsRead(sectionsStatus, std::move(caps), sections, std::move(callback));
        });
}

void SPro24DspProtocol::HandleGeneralSectionsRead(IOReturn status,
                                                  DICECapabilities caps,
                                                  GeneralSections sections,
                                                  InitCallback callback) {
    if (status != kIOReturnSuccess) {
        ASFW_LOG(DICE, "Failed to read general sections: 0x%x", status);
        callback(status);
        return;
    }

    sections_ = sections;
    tx_.ReadExtensionSections(
        [this, callback = std::move(callback), caps = std::move(caps)](IOReturn extStatus,
                                                                       ExtensionSections extensionSections) mutable {
            HandleExtensionSectionsRead(extStatus, std::move(caps), extensionSections, std::move(callback));
        });
}

void SPro24DspProtocol::HandleExtensionSectionsRead(IOReturn status,
                                                    DICECapabilities caps,
                                                    ExtensionSections sections,
                                                    InitCallback callback) {
    if (status != kIOReturnSuccess) {
        ASFW_LOG(DICE, "Failed to read extension sections: 0x%x", status);
        callback(status);
        return;
    }

    extensionSections_ = sections;
    appSectionBase_ = ExtensionAbsoluteOffset(extensionSections_.application);
    commandSectionBase_ = ExtensionAbsoluteOffset(extensionSections_.command);
    routerSectionBase_ = ExtensionAbsoluteOffset(extensionSections_.router);
    currentConfigBase_ = ExtensionAbsoluteOffset(extensionSections_.currentConfig);

    if (appSectionBase_ == kDICEExtensionOffset ||
        commandSectionBase_ == kDICEExtensionOffset ||
        routerSectionBase_ == kDICEExtensionOffset ||
        currentConfigBase_ == kDICEExtensionOffset) {
        ASFW_LOG(DICE, "SPro24DspProtocol: missing required TCAT extension section(s)");
        callback(kIOReturnNotFound);
        return;
    }

    initialized_ = true;
    CacheRuntimeCaps(caps);
    LogInitializationSummary(caps);

    // Keep protocol initialization side-effect free.
    ASFW_LOG(DICE, "SPro24DspProtocol: extension sections cached; stream start remains backend-driven");
    callback(kIOReturnSuccess);
}

void SPro24DspProtocol::CacheRuntimeCaps(const DICECapabilities& caps) noexcept {
    if (caps.txStreams.numStreams > 0) {
        const auto& tx0 = caps.txStreams.streams[0];
        hostInputPcmChannels_.store(tx0.pcmChannels, std::memory_order_relaxed);
        deviceToHostAm824Slots_.store(tx0.Am824Slots(), std::memory_order_relaxed);
    } else {
        hostInputPcmChannels_.store(0, std::memory_order_relaxed);
        deviceToHostAm824Slots_.store(0, std::memory_order_relaxed);
    }

    if (caps.rxStreams.numStreams > 0) {
        const auto& rx0 = caps.rxStreams.streams[0];
        hostOutputPcmChannels_.store(rx0.pcmChannels, std::memory_order_relaxed);
        hostToDeviceAm824Slots_.store(rx0.Am824Slots(), std::memory_order_relaxed);
    } else {
        hostOutputPcmChannels_.store(0, std::memory_order_relaxed);
        hostToDeviceAm824Slots_.store(0, std::memory_order_relaxed);
    }

    runtimeSampleRateHz_.store(caps.global.sampleRate, std::memory_order_relaxed);
    runtimeCapsValid_.store(true, std::memory_order_release);
}

void SPro24DspProtocol::LogInitializationSummary(const DICECapabilities& caps) const {
    ASFW_LOG(DICE, "═══════════════════════════════════════════════════════");
    ASFW_LOG(DICE, "SPro24DspProtocol Initialized Successfully");
    ASFW_LOG(DICE, "  Current Rate: %u Hz", caps.global.sampleRate);
    ASFW_LOG(DICE, "  TX Streams:   %u (pcm=%u midi=%u slots=%u)",
             caps.txStreams.numStreams,
             caps.txStreams.TotalPcmChannels(),
             caps.txStreams.TotalMidiPorts(),
             caps.txStreams.TotalAm824Slots());
    ASFW_LOG(DICE, "  RX Streams:   %u (pcm=%u midi=%u slots=%u)",
             caps.rxStreams.numStreams,
             caps.rxStreams.TotalPcmChannels(),
             caps.rxStreams.TotalMidiPorts(),
             caps.rxStreams.TotalAm824Slots());

    for (uint32_t i = 0; i < caps.txStreams.numStreams && i < 4; ++i) {
        LogDiceStreamEntryDetail("TX", i, caps.txStreams.streams[i]);
    }
    for (uint32_t i = 0; i < caps.rxStreams.numStreams && i < 4; ++i) {
        LogDiceStreamEntryDetail("RX", i, caps.rxStreams.streams[i]);
    }

    if (caps.rxStreams.numStreams > 0) {
        const auto& rx0 = caps.rxStreams.streams[0];
        ASFW_LOG(DICE,
                 "  Host->HW (DICE RX stream 0): pcm=%u midi=%u am824Slots=%u",
                 rx0.pcmChannels,
                 rx0.midiPorts,
                 Am824SlotsFor(rx0));
    }
    if (caps.txStreams.numStreams > 0) {
        const auto& tx0 = caps.txStreams.streams[0];
        ASFW_LOG(DICE,
                 "  HW->Host (DICE TX stream 0): pcm=%u midi=%u am824Slots=%u",
                 tx0.pcmChannels,
                 tx0.midiPorts,
                 Am824SlotsFor(tx0));
    }

    ASFW_LOG(DICE, "  Nickname:     '%{public}s'", caps.global.nickname);
    ASFW_LOG(DICE, "  App Section:  0x%08x", appSectionBase_);
    ASFW_LOG(DICE, "  Cmd Section:  0x%08x", commandSectionBase_);
    ASFW_LOG(DICE, "  Router Sect.: 0x%08x", routerSectionBase_);
    ASFW_LOG(DICE, "  Current Cfg:  0x%08x", currentConfigBase_);
    ASFW_LOG(DICE, "═══════════════════════════════════════════════════════");
}

IOReturn SPro24DspProtocol::Shutdown() {
    ASFW_LOG(DICE, "SPro24DspProtocol::Shutdown");
    if (initialized_ && (duplexPrepared_ || duplexRunning_)) {
        const IOReturn stopStatus = StopDuplex();
        if (stopStatus != kIOReturnSuccess && stopStatus != kIOReturnUnsupported) {
            ASFW_LOG(DICE, "SPro24DspProtocol::Shutdown duplex stop failed: 0x%x", stopStatus);
        }
    }
    if (ownerClaimed_ && initialized_) {
        const uint64_t ownerValue = BuildOwnerValue(busInfo_.GetLocalNodeID());
        const auto [status, previous] =
            CompareSwapOctletSync(tx_, sections_.global.offset + GlobalOffset::kOwnerHi, ownerValue, kOwnerNoOwner);
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "SPro24DspProtocol::Shutdown owner release failed: 0x%x", status);
        } else if (previous != ownerValue && previous != kOwnerNoOwner) {
            ASFW_LOG(DICE,
                     "SPro24DspProtocol::Shutdown owner changed before release: prev=0x%016llx",
                     previous);
        }
    }
    runtimeCapsValid_.store(false, std::memory_order_release);
    runtimeSampleRateHz_.store(0, std::memory_order_relaxed);
    hostInputPcmChannels_.store(0, std::memory_order_relaxed);
    hostOutputPcmChannels_.store(0, std::memory_order_relaxed);
    deviceToHostAm824Slots_.store(0, std::memory_order_relaxed);
    hostToDeviceAm824Slots_.store(0, std::memory_order_relaxed);
    duplexPrepared_ = false;
    duplexArmed_ = false;
    duplexRunning_ = false;
    ownerClaimed_ = false;
    initialized_ = false;
    return kIOReturnSuccess;
}

IOReturn SPro24DspProtocol::StartDuplex48k(const AudioDuplexChannels& channels) {
    ScopedBringupTrace trace("SPro24DspProtocol::StartDuplex48k");
    if (!initialized_) {
        ASFW_LOG(DICE, "SPro24DspProtocol::StartDuplex48k rejected (not initialized)");
        return kIOReturnNotReady;
    }
    if (channels.deviceToHostIsoChannel > 63 || channels.hostToDeviceIsoChannel > 63) {
        return kIOReturnBadArgument;
    }

    const uint32_t clockSelect = (ClockRateIndex::k48000 << 8) |
                                 static_cast<uint32_t>(ClockSource::Internal);
    const uint64_t ownerValue = BuildOwnerValue(busInfo_.GetLocalNodeID());
    const auto rollbackPreparedState = [this]() {
        (void)WriteQuadletSync(tx_, sections_.global.offset + GlobalOffset::kEnable, 0);
        (void)WriteQuadletSync(tx_,
                               sections_.txStreamFormat.offset + TxOffset::kIsochronous,
                               kDisabledIsoChannel);
        (void)WriteQuadletSync(tx_,
                               sections_.rxStreamFormat.offset + RxOffset::kIsochronous,
                               kDisabledIsoChannel);
        (void)WriteQuadletSync(tx_,
                               sections_.rxStreamFormat.offset + RxOffset::kSeqStart,
                               kRxSeqStartDefault);
        duplexPrepared_ = false;
        duplexArmed_ = false;
        duplexRunning_ = false;
    };

    ASFW_LOG(DICE, "═══════════════════════════════════════════════════════");
    ASFW_LOG(DICE, "SPro24DspProtocol::StartDuplex48k begin");
    ASFW_LOG(DICE,
             "  DICE TX iso=%u (device->host)  DICE RX iso=%u (host->device)",
             channels.deviceToHostIsoChannel,
             channels.hostToDeviceIsoChannel);
    ASFW_LOG(DICE, "  Owner value: 0x%016llx", ownerValue);
    LogIns0PlaybackMap();

    if (duplexPrepared_ || duplexRunning_) {
        const IOReturn stopStatus = StopDuplex();
        if (stopStatus != kIOReturnSuccess && stopStatus != kIOReturnUnsupported) {
            ASFW_LOG(DICE, "StartDuplex48k: existing duplex stop failed: 0x%x", stopStatus);
            return stopStatus;
        }
    }
    duplexPrepared_ = false;
    duplexRunning_ = false;

    NotificationMailbox::Reset();
    const auto [ownerStatus, previousOwner] =
        CompareSwapOctletSync(tx_,
                              sections_.global.offset + GlobalOffset::kOwnerHi,
                              kOwnerNoOwner,
                              ownerValue);
    if (ownerStatus != kIOReturnSuccess) {
        ASFW_LOG(DICE, "StartDuplex48k: owner claim failed: 0x%x", ownerStatus);
        return ownerStatus;
    }
    if (previousOwner != kOwnerNoOwner && previousOwner != ownerValue) {
        ASFW_LOG(DICE,
                 "StartDuplex48k: device already owned by 0x%016llx",
                 previousOwner);
        return kIOReturnExclusiveAccess;
    }
    ownerClaimed_ = true;

    IOReturn status =
        WriteQuadletSync(tx_, sections_.global.offset + GlobalOffset::kClockSelect, clockSelect);
    if (status != kIOReturnSuccess) {
        ASFW_LOG(DICE, "StartDuplex48k: clock select write failed: 0x%x", status);
        rollbackPreparedState();
        return status;
    }

    bool clockAccepted = false;
    for (uint32_t waited = 0; waited < kAsyncTimeoutMs; waited += kPollIntervalMs) {
        const uint32_t mailboxBits = NotificationMailbox::Consume();
        if ((mailboxBits & ::ASFW::Audio::DICE::Notify::kClockAccepted) != 0) {
            clockAccepted = true;
            break;
        }

        const auto [notifyStatus, notifyBits] =
            ReadQuadletSync(tx_, sections_.global.offset + GlobalOffset::kNotification);
        if (notifyStatus == kIOReturnSuccess &&
            (notifyBits & ::ASFW::Audio::DICE::Notify::kClockAccepted) != 0) {
            clockAccepted = true;
            break;
        }

        IOSleep(kPollIntervalMs);
    }
    if (!clockAccepted) {
        ASFW_LOG(DICE, "StartDuplex48k: timed out waiting for CLOCK_ACCEPTED");
        rollbackPreparedState();
        return kIOReturnTimeout;
    }

    status = WriteQuadletSync(tx_,
                              sections_.rxStreamFormat.offset + RxOffset::kIsochronous,
                              channels.hostToDeviceIsoChannel);
    if (status != kIOReturnSuccess) {
        rollbackPreparedState();
        return status;
    }

    status = WriteQuadletSync(tx_,
                              sections_.rxStreamFormat.offset + RxOffset::kSeqStart,
                              kRxSeqStartDefault);
    if (status != kIOReturnSuccess) {
        rollbackPreparedState();
        return status;
    }

    status = WriteQuadletSync(tx_,
                              sections_.txStreamFormat.offset + TxOffset::kIsochronous,
                              channels.deviceToHostIsoChannel);
    if (status != kIOReturnSuccess) {
        rollbackPreparedState();
        return status;
    }

    status = WriteQuadletSync(tx_,
                              sections_.txStreamFormat.offset + TxOffset::kSpeed,
                              kTxSpeedS400);
    if (status != kIOReturnSuccess) {
        rollbackPreparedState();
        return status;
    }

    // Prefer current_config because it reflects the live low-rate route table after the
    // device applies commands; fall back to the writable router shadow when current_config
    // is empty or unavailable.
    const auto loadRouterEntries =
        [this](uint32_t base,
               uint32_t sectionSize,
               uint32_t sectionOffset,
               std::vector<Routing::RouterEntry>& entries) {
            if (sectionSize <= sectionOffset + 4) {
                return kIOReturnUnderrun;
            }

            const auto [countStatus, rawCount] = ReadQuadletSync(tx_, base + sectionOffset);
            if (countStatus != kIOReturnSuccess) {
                return countStatus;
            }

            const uint32_t maxBySize = (sectionSize - sectionOffset - 4) / 4;
            const uint32_t entryCount = std::min(std::min(rawCount, maxBySize), kMaxRouterEntries);
            entries.clear();
            if (entryCount == 0) {
                return kIOReturnSuccess;
            }

            const auto [readStatus, rawEntries] =
                ReadBlockSync(tx_, base + sectionOffset + 4, entryCount * 4);
            if (readStatus != kIOReturnSuccess) {
                return readStatus;
            }
            if (rawEntries.size() < entryCount * 4) {
                return kIOReturnUnderrun;
            }

            entries.reserve(entryCount);
            for (uint32_t i = 0; i < entryCount; ++i) {
                entries.push_back(ParseRouterEntry(rawEntries.data() + i * 4));
            }
            return kIOReturnSuccess;
        };

    std::vector<Routing::RouterEntry> routerEntries;
    status = loadRouterEntries(currentConfigBase_,
                               extensionSections_.currentConfig.size,
                               CurrentConfigOffset::kLowRouter,
                               routerEntries);
    if (status != kIOReturnSuccess || routerEntries.empty()) {
        status = loadRouterEntries(routerSectionBase_, extensionSections_.router.size, 0, routerEntries);
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "StartDuplex48k: failed to read router image: 0x%x", status);
            rollbackPreparedState();
            return status;
        }
    }

    const auto routerEntriesEqual =
        [](const std::vector<Routing::RouterEntry>& lhs,
           const std::vector<Routing::RouterEntry>& rhs) {
            if (lhs.size() != rhs.size()) {
                return false;
            }
            for (size_t i = 0; i < lhs.size(); ++i) {
                if (lhs[i].dst.blockId != rhs[i].dst.blockId ||
                    lhs[i].dst.channel != rhs[i].dst.channel ||
                    lhs[i].src.blockId != rhs[i].src.blockId ||
                    lhs[i].src.channel != rhs[i].src.channel ||
                    lhs[i].peak != rhs[i].peak) {
                    return false;
                }
            }
            return true;
        };

    LogPlaybackMirrorState("StartDuplex48k: current playback mirrors", routerEntries);
    LogRouterEntriesDetailed("StartDuplex48k: current router entries", routerEntries);
    if (Routing::HasStereoPlaybackMirror(routerEntries, Routing::kMonitor12Mirror) &&
        !Routing::HasAnyHeadphonePlaybackMirror(routerEntries)) {
        ASFW_LOG(DICE,
                 "StartDuplex48k: warning - only line out 1/2 is fed from host playback; "
                 "headphone mirrors are not targeted");
    }

    const auto currentRouterEntries = routerEntries;

    // This visible TCAT route image is necessary for first-audible playback, but it is not a
    // full reimplementation of the legacy MixControl hidden block-7/8 logic. We log it loudly
    // because visible routing alone has repeatedly looked correct while the unit stayed silent.
    Routing::ApplyStereoPlaybackMirror(routerEntries, Routing::kMonitor12Mirror);
    Routing::ApplyStereoPlaybackMirror(routerEntries, Routing::kHeadphone1Mirror);
    Routing::ApplyStereoPlaybackMirror(routerEntries, Routing::kHeadphone2Mirror);
    std::vector<Routing::RouterEntry> verifiedRoutes;
    const bool routerChanged = !routerEntriesEqual(routerEntries, currentRouterEntries);
    if (routerChanged) {
        ASFW_LOG(DICE,
                 "StartDuplex48k: mirrored host playback 1/2 to %{public}s, %{public}s, and "
                 "%{public}s",
                 Routing::kMonitor12Mirror.label.data(),
                 Routing::kHeadphone1Mirror.label.data(),
                 Routing::kHeadphone2Mirror.label.data());
        LogRouterEntriesDetailed("StartDuplex48k: router entries to be written", routerEntries);

        const size_t routerBytes = 4 + routerEntries.size() * 4;
        if (routerBytes > extensionSections_.router.size) {
            ASFW_LOG(DICE,
                     "StartDuplex48k: router image %zu exceeds router section %u",
                     routerBytes,
                     extensionSections_.router.size);
            rollbackPreparedState();
            return kIOReturnOverrun;
        }

        std::vector<uint8_t> routerRaw(routerBytes, 0);
        DICETransaction::QuadletToWire(static_cast<uint32_t>(routerEntries.size()), routerRaw.data());
        for (size_t i = 0; i < routerEntries.size(); ++i) {
            SerializeRouterEntry(routerEntries[i], routerRaw.data() + 4 + i * 4);
        }

        status = WriteBlockSync(tx_, routerSectionBase_, routerRaw.data(), routerRaw.size());
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "StartDuplex48k: router section write failed: 0x%x", status);
            rollbackPreparedState();
            return status;
        }

        const uint32_t loadRouterOpcode =
            ExtensionCommandOpcode::kExecute |
            ExtensionCommandOpcode::kRateLow |
            ExtensionCommandOpcode::kLoadRouter;
        status = WriteQuadletSync(tx_,
                                  commandSectionBase_ + ExtensionCommandOffset::kOpcode,
                                  loadRouterOpcode);
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "StartDuplex48k: load-router command write failed: 0x%x", status);
            rollbackPreparedState();
            return status;
        }

        bool commandComplete = false;
        for (uint32_t waited = 0; waited < kAsyncTimeoutMs; waited += kPollIntervalMs) {
            const auto [opcodeStatus, opcodeValue] =
                ReadQuadletSync(tx_, commandSectionBase_ + ExtensionCommandOffset::kOpcode);
            if (opcodeStatus != kIOReturnSuccess) {
                rollbackPreparedState();
                return opcodeStatus;
            }
            if ((opcodeValue & ExtensionCommandOpcode::kExecute) == 0) {
                const auto [returnStatus, returnValue] =
                    ReadQuadletSync(tx_, commandSectionBase_ + ExtensionCommandOffset::kReturn);
                if (returnStatus != kIOReturnSuccess) {
                    rollbackPreparedState();
                    return returnStatus;
                }
                if (returnValue != 0) {
                    ASFW_LOG(DICE, "StartDuplex48k: load-router returned 0x%08x", returnValue);
                    rollbackPreparedState();
                    return kIOReturnError;
                }
                commandComplete = true;
                break;
            }
            IOSleep(kPollIntervalMs);
        }
        if (!commandComplete) {
            ASFW_LOG(DICE, "StartDuplex48k: load-router command timed out");
            rollbackPreparedState();
            return kIOReturnTimeout;
        }

        status = loadRouterEntries(currentConfigBase_,
                                   extensionSections_.currentConfig.size,
                                   CurrentConfigOffset::kLowRouter,
                                   verifiedRoutes);
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "StartDuplex48k: current-config readback failed: 0x%x", status);
            rollbackPreparedState();
            return status;
        }
    } else {
        ASFW_LOG(DICE,
                 "StartDuplex48k: live router already mirrors playback to monitor12/hp1/hp2; "
                 "preserving current e020 route state");
        verifiedRoutes = currentRouterEntries;
    }
    LogPlaybackMirrorState("StartDuplex48k: verified playback mirrors", verifiedRoutes);
    LogRouterEntriesDetailed("StartDuplex48k: verified router entries", verifiedRoutes);
    if (!Routing::HasStereoPlaybackMirror(verifiedRoutes, Routing::kMonitor12Mirror) ||
        !Routing::HasStereoPlaybackMirror(verifiedRoutes, Routing::kHeadphone1Mirror) ||
        !Routing::HasStereoPlaybackMirror(verifiedRoutes, Routing::kHeadphone2Mirror)) {
        ASFW_LOG(DICE,
                 "StartDuplex48k: current-config readback is missing required monitor/headphone "
                 "mirrors");
        rollbackPreparedState();
        return kIOReturnError;
    }

    bool dimMuteChanged = false;
    bool outputChanged = false;
    const auto [groupStatus, groupRaw] =
        ReadBlockSync(tx_, appSectionBase_ + kOutputGroupOffset, kOutputGroupStateSize);
    if (groupStatus == kIOReturnSuccess && groupRaw.size() >= kOutputGroupStateSize) {
        OutputGroupState group = OutputGroupState::FromWire(groupRaw.data());
        LogOutputGroupState("StartDuplex48k: current output-group", group);

        if (group.muteEnabled) {
            group.muteEnabled = false;
            dimMuteChanged = true;
        }
        if (group.dimEnabled) {
            group.dimEnabled = false;
            dimMuteChanged = true;
        }
        constexpr std::array<size_t, 6> kFirstAudibleOutputs = {0, 1, 2, 3, 4, 5};
        for (size_t ch : kFirstAudibleOutputs) {
            if (group.volMutes[ch]) {
                group.volMutes[ch] = false;
                outputChanged = true;
            }
            if (group.volumes[ch] != OutputGroupState::kVolMax) {
                group.volumes[ch] = OutputGroupState::kVolMax;
                outputChanged = true;
            }
        }

        if (dimMuteChanged || outputChanged) {
            LogOutputGroupState("StartDuplex48k: output-group to be written", group);
            std::array<uint8_t, kOutputGroupStateSize> raw{};
            group.ToWire(raw.data());
            status = WriteBlockSync(tx_, appSectionBase_ + kOutputGroupOffset, raw.data(), raw.size());
            if (status != kIOReturnSuccess) {
                ASFW_LOG(DICE, "StartDuplex48k: output-group write failed: 0x%x", status);
                rollbackPreparedState();
                return status;
            }

            if (dimMuteChanged) {
                status = WriteQuadletSync(tx_,
                                          appSectionBase_ + kSwNoticeOffset,
                                          static_cast<uint32_t>(SwNotice::DimMute));
                if (status != kIOReturnSuccess) {
                    rollbackPreparedState();
                    return status;
                }
            }
            if (outputChanged) {
                status = WriteQuadletSync(tx_,
                                          appSectionBase_ + kSwNoticeOffset,
                                          static_cast<uint32_t>(SwNotice::OutputSrc));
                if (status != kIOReturnSuccess) {
                    rollbackPreparedState();
                    return status;
                }
            }
        } else {
            ASFW_LOG(DICE,
                     "StartDuplex48k: live output-group already open for line out 1/2, 3/4, and "
                     "5/6; preserving current app state");
        }
    } else {
        ASFW_LOG(DICE,
                 "StartDuplex48k: output-group read skipped or failed (status=0x%x size=%zu)",
                 groupStatus,
                 groupRaw.size());
    }

    {
        constexpr uint32_t kAppDiagOffset = kDspEnableOffset;
        constexpr size_t kAppDiagSize = 0x20;
        const auto [appDiagStatus, appDiagRaw] =
            ReadBlockSync(tx_, appSectionBase_ + kAppDiagOffset, kAppDiagSize);
        if (appDiagStatus == kIOReturnSuccess && appDiagRaw.size() == kAppDiagSize) {
            const uint32_t item28 = DICETransaction::QuadletFromWire(appDiagRaw.data() + 0x00);
            const uint32_t item29 = DICETransaction::QuadletFromWire(appDiagRaw.data() + 0x04);
            const uint32_t item30 = DICETransaction::QuadletFromWire(appDiagRaw.data() + 0x08);
            const uint32_t item31 = DICETransaction::QuadletFromWire(appDiagRaw.data() + 0x0c);
            const auto effectGeneral = EffectGeneralParams::FromWire(appDiagRaw.data() + 0x08);
            ASFW_LOG(DICE,
                     "StartDuplex48k: app DSP window @+0x%04x size=%zu preview=%{public}s",
                     kAppDiagOffset,
                     appDiagRaw.size(),
                     HexPreview(appDiagRaw.data(), appDiagRaw.size(), kAppDiagSize).c_str());
            ASFW_LOG(DICE,
                     "StartDuplex48k: app items[28..31] raw28=0x%08x status=0x%08x "
                     "effectGeneral=0x%08x raw31=0x%08x dspRunning=%s deviceBusy=%s",
                     item28,
                     item29,
                     item30,
                     item31,
                     (item29 & 0x1U) != 0 ? "yes" : "no",
                     (item29 & 0x2U) != 0 ? "yes" : "no");
            ASFW_LOG(DICE,
                     "StartDuplex48k: effectGeneral decode ch0(eq=%s comp=%s eqAfterComp=%s) "
                     "ch1(eq=%s comp=%s eqAfterComp=%s)",
                     effectGeneral.eqEnable[0] ? "on" : "off",
                     effectGeneral.compEnable[0] ? "on" : "off",
                     effectGeneral.eqAfterComp[0] ? "on" : "off",
                     effectGeneral.eqEnable[1] ? "on" : "off",
                     effectGeneral.compEnable[1] ? "on" : "off",
                     effectGeneral.eqAfterComp[1] ? "on" : "off");
        } else {
            ASFW_LOG(DICE,
                     "StartDuplex48k: app DSP window read failed (status=0x%x size=%zu)",
                     appDiagStatus,
                     appDiagRaw.size());
        }
    }

    if (routerChanged || dimMuteChanged || outputChanged) {
        // Legacy Pro24DSP routing changes are followed by an additional app-space message that
        // appears to refresh the hidden headphone/VRM routing layer above the visible TCAT matrix.
        status = WriteQuadletSync(tx_,
                                  appSectionBase_ + kSwNoticeOffset,
                                  static_cast<uint32_t>(SwNotice::RoutingRefresh));
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "StartDuplex48k: routing refresh notice failed: 0x%x", status);
            rollbackPreparedState();
            return status;
        }
        IOSleep(kRoutingRefreshDelayMs);
    } else {
        ASFW_LOG(DICE,
                 "StartDuplex48k: no visible router/output-group changes needed; skipping "
                 "routing refresh notice");
    }

    const auto [txIsoStatus, txIsoReadback] =
        ReadQuadletSync(tx_, sections_.txStreamFormat.offset + TxOffset::kIsochronous);
    const auto [txSpeedStatus, txSpeedReadback] =
        ReadQuadletSync(tx_, sections_.txStreamFormat.offset + TxOffset::kSpeed);
    const auto [rxIsoStatus, rxIsoReadback] =
        ReadQuadletSync(tx_, sections_.rxStreamFormat.offset + RxOffset::kIsochronous);
    const auto [rxSeqStartStatus, rxSeqStartReadback] =
        ReadQuadletSync(tx_, sections_.rxStreamFormat.offset + RxOffset::kSeqStart);

    if (txIsoStatus != kIOReturnSuccess || txSpeedStatus != kIOReturnSuccess ||
        rxIsoStatus != kIOReturnSuccess || rxSeqStartStatus != kIOReturnSuccess) {
        rollbackPreparedState();
        return kIOReturnError;
    }
    if (txIsoReadback != channels.deviceToHostIsoChannel ||
        txSpeedReadback != kTxSpeedS400 ||
        rxIsoReadback != channels.hostToDeviceIsoChannel ||
        rxSeqStartReadback != kRxSeqStartDefault) {
        ASFW_LOG(DICE,
                 "StartDuplex48k: readback mismatch txIso=%u/%u txSpeed=%u/%u "
                 "rxIso=%u/%u rxSeqStart=%u/%u",
                 txIsoReadback,
                 channels.deviceToHostIsoChannel,
                 txSpeedReadback,
                 kTxSpeedS400,
                 rxIsoReadback,
                 channels.hostToDeviceIsoChannel,
                 rxSeqStartReadback,
                 kRxSeqStartDefault);
        rollbackPreparedState();
        return kIOReturnError;
    }

    runtimeSampleRateHz_.store(48000, std::memory_order_relaxed);
    duplexPrepared_ = true;
    duplexArmed_ = false;
    ASFW_LOG(DICE, "SPro24DspProtocol::StartDuplex48k prepared successfully");
    ASFW_LOG(DICE, "═══════════════════════════════════════════════════════");
    return kIOReturnSuccess;
}

IOReturn SPro24DspProtocol::ArmDuplex48kAfterReceiveStart() {
    ScopedBringupTrace trace("SPro24DspProtocol::ArmDuplex48kAfterReceiveStart");
    if (!initialized_ || !duplexPrepared_) {
        ASFW_LOG(DICE, "ArmDuplex48kAfterReceiveStart rejected (not prepared)");
        return kIOReturnNotReady;
    }
    if (duplexArmed_) {
        ASFW_LOG(DICE, "ArmDuplex48kAfterReceiveStart: already armed");
        return kIOReturnSuccess;
    }

    IOReturn status = WriteQuadletSync(tx_, sections_.global.offset + GlobalOffset::kEnable, 1);
    if (status != kIOReturnSuccess) {
        ASFW_LOG(DICE, "ArmDuplex48kAfterReceiveStart: GLOBAL_ENABLE write failed: 0x%x", status);
        return status;
    }

    duplexArmed_ = true;
    ASFW_LOG(DICE, "ArmDuplex48kAfterReceiveStart: GLOBAL_ENABLE asserted for SYT bootstrap");
    return kIOReturnSuccess;
}

IOReturn SPro24DspProtocol::CompleteDuplex48kStart() {
    ScopedBringupTrace trace("SPro24DspProtocol::CompleteDuplex48kStart");
    if (!initialized_ || !duplexPrepared_ || !duplexArmed_) {
        ASFW_LOG(DICE, "CompleteDuplex48kStart rejected (not armed)");
        return kIOReturnNotReady;
    }

    NotificationMailbox::Reset();

    bool playbackLocked = false;
    uint32_t lastStatus = 0;
    uint32_t lastExtStatus = 0;
    uint32_t lastNotification = 0;

    for (uint32_t waited = 0; waited < kReadyTimeoutMs; waited += kPollIntervalMs) {
        const uint32_t mailboxBits = NotificationMailbox::Consume();
        if (mailboxBits != 0) {
            lastNotification |= mailboxBits;
            if ((mailboxBits & ::ASFW::Audio::DICE::Notify::kLockChange) != 0) {
                ASFW_LOG(DICE, "CompleteDuplex48kStart: observed LOCK_CHANGE");
            }
            if ((mailboxBits & ::ASFW::Audio::DICE::Notify::kExtStatus) != 0) {
                ASFW_LOG(DICE, "CompleteDuplex48kStart: observed EXT_STATUS notification");
            }
        }

        const auto [statusReadStatus, statusValue] =
            ReadQuadletSync(tx_, sections_.global.offset + GlobalOffset::kStatus);
        if (statusReadStatus != kIOReturnSuccess) {
            (void)StopDuplex();
            return statusReadStatus;
        }
        lastStatus = statusValue;

        if (IsSourceLocked(lastStatus)) {
            playbackLocked = true;
            break;
        }

        IOSleep(kPollIntervalMs);
    }

    const auto [notificationStatus, notificationValue] =
        ReadQuadletSync(tx_, sections_.global.offset + GlobalOffset::kNotification);
    if (notificationStatus == kIOReturnSuccess) {
        lastNotification |= notificationValue;
    }

    const auto [extStatusReadStatus, extStatusValue] =
        ReadQuadletSync(tx_, sections_.global.offset + GlobalOffset::kExtStatus);
    if (extStatusReadStatus == kIOReturnSuccess) {
        lastExtStatus = extStatusValue;
    }

    if (!playbackLocked) {
        LogGlobalStatusSummary("CompleteDuplex48kStart: playback lock timeout",
                               lastNotification,
                               lastStatus,
                               lastExtStatus);
        (void)StopDuplex();
        return kIOReturnTimeout;
    }

    duplexRunning_ = true;
    LogGlobalStatusSummary("CompleteDuplex48kStart: playback lock established",
                           lastNotification,
                           lastStatus,
                           lastExtStatus);
    return kIOReturnSuccess;
}

IOReturn SPro24DspProtocol::StopDuplex() {
    ScopedBringupTrace trace("SPro24DspProtocol::StopDuplex");
    if (!initialized_ || !ownerClaimed_) {
        duplexPrepared_ = false;
        duplexArmed_ = false;
        duplexRunning_ = false;
        return kIOReturnSuccess;
    }

    IOReturn firstError = kIOReturnSuccess;
    const auto capture = [&firstError](IOReturn status) {
        if (status != kIOReturnSuccess && firstError == kIOReturnSuccess) {
            firstError = status;
        }
    };

    capture(WriteQuadletSync(tx_, sections_.global.offset + GlobalOffset::kEnable, 0));
    capture(WriteQuadletSync(tx_,
                             sections_.txStreamFormat.offset + TxOffset::kIsochronous,
                             kDisabledIsoChannel));
    capture(WriteQuadletSync(tx_,
                             sections_.rxStreamFormat.offset + RxOffset::kIsochronous,
                             kDisabledIsoChannel));
    capture(WriteQuadletSync(tx_,
                             sections_.rxStreamFormat.offset + RxOffset::kSeqStart,
                             kRxSeqStartDefault));

    duplexPrepared_ = false;
    duplexArmed_ = false;
    duplexRunning_ = false;
    return firstError;
}

uint32_t SPro24DspProtocol::ExtensionAbsoluteOffset(const Section& section, uint32_t offset) const noexcept {
    return kDICEExtensionOffset + section.offset + offset;
}

void SPro24DspProtocol::ReadAppSection(uint32_t offset, size_t size, DICEReadCallback callback) {
    tx_.ReadBlock(appSectionBase_ + offset, size, std::move(callback));
}

void SPro24DspProtocol::WriteAppSection(uint32_t offset,
                                        const uint8_t* data,
                                        size_t size,
                                        DICEWriteCallback callback) {
    tx_.WriteBlock(appSectionBase_ + offset, data, size, std::move(callback));
}

void SPro24DspProtocol::SendSwNotice(SwNotice notice, VoidCallback callback) {
    tx_.WriteQuadlet(appSectionBase_ + kSwNoticeOffset,
                     static_cast<uint32_t>(notice),
                     std::move(callback));
}

void SPro24DspProtocol::EnableDsp(bool enable, VoidCallback callback) {
    uint32_t value = enable ? 1 : 0;
    tx_.WriteQuadlet(appSectionBase_ + kDspEnableOffset,
                     value,
                     [this, callback](IOReturn status) {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }
        SendSwNotice(SwNotice::DspChanged, callback);
    });
}

void SPro24DspProtocol::GetEffectParams(ResultCallback<EffectGeneralParams> callback) {
    tx_.ReadQuadlet(appSectionBase_ + kEffectGeneralOffset,
                    [callback](IOReturn status, uint32_t value) {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        uint8_t data[4];
        DICETransaction::QuadletToWire(value, data);
        callback(kIOReturnSuccess, EffectGeneralParams::FromWire(data));
    });
}

void SPro24DspProtocol::SetEffectParams(const EffectGeneralParams& params, VoidCallback callback) {
    uint8_t data[4];
    params.ToWire(data);
    uint32_t value = DICETransaction::QuadletFromWire(data);
    
    tx_.WriteQuadlet(appSectionBase_ + kEffectGeneralOffset,
                     value,
                     [this, callback](IOReturn status) {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }
        SendSwNotice(SwNotice::EffectChanged, callback);
    });
}

void SPro24DspProtocol::GetCompressorState(ResultCallback<CompressorState> callback) {
    ReadAppSection(kCoefOffset + CoefBlock::kCompressor * kCoefBlockSize, 2 * kCoefBlockSize,
                   [callback](IOReturn status, const uint8_t* data, size_t /*size*/) {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        callback(kIOReturnSuccess, CompressorState::FromWire(data));
    });
}

void SPro24DspProtocol::SetCompressorState(const CompressorState& state, VoidCallback callback) {
    // Note: Need to allocate buffer that outlives async call
    auto buffer = std::make_shared<std::array<uint8_t, 2 * kCoefBlockSize>>();
    state.ToWire(buffer->data());
    
    WriteAppSection(kCoefOffset + CoefBlock::kCompressor * kCoefBlockSize, buffer->data(), buffer->size(),
                    [this, callback, buffer](IOReturn status) {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }
        // Per Linux reference (spro24dsp.rs lines 770-771): send BOTH
        // CompCh0 and CompCh1 SW notices after compressor state write.
        SendSwNotice(SwNotice::CompCh0, [this, callback](IOReturn s1) {
            if (s1 != kIOReturnSuccess) {
                callback(s1);
                return;
            }
            SendSwNotice(SwNotice::CompCh1, callback);
        });
    });
}

void SPro24DspProtocol::GetReverbState(ResultCallback<ReverbState> callback) {
    ReadAppSection(kCoefOffset + CoefBlock::kReverb * kCoefBlockSize, kCoefBlockSize,
                   [callback](IOReturn status, const uint8_t* data, size_t /*size*/) {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        callback(kIOReturnSuccess, ReverbState::FromWire(data));
    });
}

void SPro24DspProtocol::SetReverbState(const ReverbState& state, VoidCallback callback) {
    auto buffer = std::make_shared<std::array<uint8_t, kCoefBlockSize>>();
    state.ToWire(buffer->data());
    
    WriteAppSection(kCoefOffset + CoefBlock::kReverb * kCoefBlockSize, buffer->data(), buffer->size(),
                    [this, callback, buffer](IOReturn status) {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }
        // Per Linux reference: REVERB_SW_NOTICE = 0x1A
        SendSwNotice(SwNotice::Reverb, callback);
    });
}

void SPro24DspProtocol::GetInputParams(ResultCallback<InputParams> callback) {
    ReadAppSection(kInputOffset, 8,
                   [callback](IOReturn status, const uint8_t* data, size_t /*size*/) {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        callback(kIOReturnSuccess, InputParams::FromWire(data));
    });
}

void SPro24DspProtocol::SetInputParams(const InputParams& params, VoidCallback callback) {
    auto buffer = std::make_shared<std::array<uint8_t, 8>>();
    params.ToWire(buffer->data());
    
    WriteAppSection(kInputOffset, buffer->data(), buffer->size(),
                    [this, callback, buffer](IOReturn status) {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }
        SendSwNotice(SwNotice::InputChanged, callback);
    });
}

void SPro24DspProtocol::GetOutputGroupState(ResultCallback<OutputGroupState> callback) {
    ReadAppSection(kOutputGroupOffset, kOutputGroupStateSize,
                   [callback](IOReturn status, const uint8_t* data, size_t size) {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        if (size < kOutputGroupStateSize) {
            callback(kIOReturnUnderrun, {});
            return;
        }
        callback(kIOReturnSuccess, OutputGroupState::FromWire(data));
    });
}

void SPro24DspProtocol::SetOutputGroupState(const OutputGroupState& state, VoidCallback callback) {
    auto buffer = std::make_shared<std::array<uint8_t, kOutputGroupStateSize>>();
    state.ToWire(buffer->data());

    WriteAppSection(kOutputGroupOffset, buffer->data(), buffer->size(),
                    [this, callback, buffer](IOReturn status) {
        if (status != kIOReturnSuccess) {
            callback(status);
            return;
        }
        SendSwNotice(SwNotice::DimMute, [this, callback](IOReturn dimStatus) {
            if (dimStatus != kIOReturnSuccess) {
                callback(dimStatus);
                return;
            }
            SendSwNotice(SwNotice::OutputSrc, callback);
        });
    });
}

// ============================================================================
// TODO: Test only - Stream Control
// ============================================================================

void SPro24DspProtocol::StartStreamTest(VoidCallback callback) {
    const AudioDuplexChannels channels{};
    callback(StartDuplex48k(channels));
}

} // namespace ASFW::Audio::DICE::Focusrite
