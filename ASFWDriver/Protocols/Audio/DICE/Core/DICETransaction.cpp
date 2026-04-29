// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// DICETransaction.cpp - DICE section/capability reader

#include "DICETransaction.hpp"
#include "../../../../Common/CallbackUtils.hpp"
#include "../../../../Logging/Logging.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace ASFW::Audio::DICE {

using ASFW::FW::ReadBE32;
using ASFW::FW::WriteBE32;
using ASFW::FW::ReadBE64;
using ASFW::FW::WriteBE64;

namespace {

constexpr size_t kCapabilityHexPreviewBytes = 64;

[[nodiscard]] IOReturn MapReadStatus(ASFW::Async::AsyncStatus status) noexcept {
    return Protocols::Ports::MapAsyncStatusToIOReturn(status);
}

std::string HexPreview(const uint8_t* data, size_t size, size_t maxBytes = kCapabilityHexPreviewBytes) {
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

void LogSectionPreview(const char* label, const uint8_t* data, size_t size) {
    ASFW_LOG(DICE,
             "%{public}s raw[%zu]=%{public}s",
             label,
             size,
             HexPreview(data, size).c_str());
}

void LogGlobalStateDetails(const GlobalState& state) {
    const uint32_t clockSource = state.clockSelect & ClockSelect::kSourceMask;
    const uint32_t clockRateIndex =
        (state.clockSelect & ClockSelect::kRateMask) >> ClockSelect::kRateShift;
    const uint32_t nominalRateHz = NominalRateHz(state.status);
    const uint32_t clockRateHz = RateHzFromIndex(clockRateIndex);
    const uint32_t arxLocks = state.extStatus &
        (ExtStatusBits::kArx1Locked |
         ExtStatusBits::kArx2Locked |
         ExtStatusBits::kArx3Locked |
         ExtStatusBits::kArx4Locked);
    const uint32_t arxSlips = state.extStatus &
        (ExtStatusBits::kArx1Slip |
         ExtStatusBits::kArx2Slip |
         ExtStatusBits::kArx3Slip |
         ExtStatusBits::kArx4Slip);

    ASFW_LOG(DICE,
             "DICE register snapshot: global owner=0x%016llx notify=0x%08x "
             "clockSelect=0x%08x clockSource=%u clockRateIndex=%u clockRateHz=%u "
             "enable=%u status=0x%08x locked=%u nominalRateHz=%u "
             "extStatus=0x%08x arxLocks=0x%08x arxSlips=0x%08x "
             "sampleRate=%u clockCaps=0x%08x version=0x%08x nickname='%{public}s'",
             static_cast<unsigned long long>(state.owner),
             state.notification,
             state.clockSelect,
             clockSource,
             clockRateIndex,
             clockRateHz,
             state.enabled ? 1U : 0U,
             state.status,
             IsSourceLocked(state.status) ? 1U : 0U,
             nominalRateHz,
             state.extStatus,
             arxLocks,
             arxSlips,
             state.sampleRate,
             state.clockCaps,
             state.version,
             state.nickname);
}

} // anonymous namespace

DICETransaction::DICETransaction(Protocols::Ports::ProtocolRegisterIO& io)
    : io_(io) {}

void DICETransaction::ReadGeneralSections(std::function<void(IOReturn, GeneralSections)> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    io_.ReadBlock(MakeDICEAddress(0),
                  static_cast<uint32_t>(GeneralSections::kWireSize),
                  [callbackState](Async::AsyncStatus status, std::span<const uint8_t> payload) {
        if (status != Async::AsyncStatus::kSuccess || payload.size() < GeneralSections::kWireSize) {
            Common::InvokeSharedCallback(callbackState, MapReadStatus(status), GeneralSections{});
            return;
        }

        GeneralSections sections = GeneralSections::Deserialize(payload.data());
        LogSectionPreview("ReadGeneralSections", payload.data(), payload.size());
        
        ASFW_LOG(DICE, "ReadGeneralSections: global=%u/%u tx=%u/%u rx=%u/%u",
                 sections.global.offset, sections.global.size,
                 sections.txStreamFormat.offset, sections.txStreamFormat.size,
                 sections.rxStreamFormat.offset, sections.rxStreamFormat.size);
        ASFW_LOG(DICE,
                 "DICE register snapshot: sections global=0x%08x+%u "
                 "tx=0x%08x+%u rx=0x%08x+%u extSync=0x%08x+%u reserved=0x%08x+%u",
                 sections.global.offset,
                 sections.global.size,
                 sections.txStreamFormat.offset,
                 sections.txStreamFormat.size,
                 sections.rxStreamFormat.offset,
                 sections.rxStreamFormat.size,
                 sections.extSync.offset,
                 sections.extSync.size,
                 sections.reserved.offset,
                 sections.reserved.size);
        
        Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, sections);
    });
}

void DICETransaction::ReadExtensionSections(std::function<void(IOReturn, ExtensionSections)> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    io_.ReadBlock(MakeDICEAddress(kDICEExtensionOffset),
                  static_cast<uint32_t>(ExtensionSections::kWireSize),
                  [callbackState](Async::AsyncStatus status, std::span<const uint8_t> payload) {
                  if (status != Async::AsyncStatus::kSuccess ||
                      payload.size() < ExtensionSections::kWireSize) {
                      Common::InvokeSharedCallback(callbackState, MapReadStatus(status), ExtensionSections{});
                      return;
                  }

                  ExtensionSections sections = ExtensionSections::Deserialize(payload.data());
                  LogSectionPreview("ReadExtensionSections", payload.data(), payload.size());
                  ASFW_LOG(DICE,
                           "ReadExtensionSections: cmd=%u/%u router=%u/%u current=%u/%u app=%u/%u",
                           sections.command.offset,
                           sections.command.size,
                           sections.router.offset,
                           sections.router.size,
                           sections.currentConfig.offset,
                           sections.currentConfig.size,
                           sections.application.offset,
                           sections.application.size);

                  Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, sections);
              });
}

// ============================================================================
// Capability Discovery
// ============================================================================

void DICETransaction::ReadGlobalStateSized(const GeneralSections& sections,
                                           size_t readSize,
                                           std::function<void(IOReturn, GlobalState)> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    io_.ReadBlock(MakeDICEAddress(sections.global.offset),
                  static_cast<uint32_t>(readSize),
                  [callbackState](Async::AsyncStatus status, std::span<const uint8_t> payload) {
        if (status != Async::AsyncStatus::kSuccess) {
            Common::InvokeSharedCallback(callbackState, MapReadStatus(status), GlobalState{});
            return;
        }

        const uint8_t* data = payload.data();
        const size_t size = payload.size();
        GlobalState state;
        LogSectionPreview("ReadGlobalState", data, size);
        
        if (size >= 8) {
            state.owner = (static_cast<uint64_t>(ReadBE32(data)) << 32) |
                          ReadBE32(data + 4);
        }
        if (size >= 12) {
            state.notification = ReadBE32(data + GlobalOffset::kNotification);
        }
        if (size >= 0x4C) {
            // Extract nickname (64 bytes = 16 quadlets starting at offset 0x0C)
            // DICE stores strings as big-endian quadlets, so we need to read each
            // 4-byte group as a quadlet and extract chars in big-endian order
            size_t nickIdx = 0;
            for (size_t q = 0; q < 16 && nickIdx < 63; ++q) {
                size_t qOffset = 0x0C + q * 4;
                if (qOffset + 4 > size) break;
                
                uint32_t quadlet = ReadBE32(data + qOffset);
                
                // Extract chars from quadlet (MSB first = big-endian string order)
                char c0 = (quadlet >> 24) & 0xFF;
                char c1 = (quadlet >> 16) & 0xFF;
                char c2 = (quadlet >> 8) & 0xFF;
                char c3 = quadlet & 0xFF;
                
                if (c0 == '\0') break;
                state.nickname[nickIdx++] = c0;
                if (c1 == '\0') break;
                state.nickname[nickIdx++] = c1;
                if (c2 == '\0') break;
                state.nickname[nickIdx++] = c2;
                if (c3 == '\0') break;
                state.nickname[nickIdx++] = c3;
            }
            state.nickname[nickIdx] = '\0';
        }
        if (size >= 0x50) {
            state.clockSelect = ReadBE32(data + GlobalOffset::kClockSelect);
        }
        if (size >= 0x54) {
            state.enabled = (ReadBE32(data + GlobalOffset::kEnable) != 0);
        }
        if (size >= 0x58) {
            state.status = ReadBE32(data + GlobalOffset::kStatus);
        }
        if (size >= 0x5C) {
            state.extStatus = ReadBE32(data + GlobalOffset::kExtStatus);
        }
        if (size >= 0x60) {
            state.sampleRate = ReadBE32(data + GlobalOffset::kSampleRate);
        }
        if (size >= 0x64) {
            state.version = ReadBE32(data + GlobalOffset::kVersion);
        }
        if (size >= 0x68) {
            state.clockCaps = ReadBE32(data + GlobalOffset::kClockCaps);
        }
        
        ASFW_LOG(DICE, "Global: rate=%uHz caps=0x%08x version=0x%08x nickname='%{public}s'",
                 state.sampleRate, state.clockCaps, state.version, state.nickname);
        LogGlobalStateDetails(state);
        
        Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, state);
    });
}

void DICETransaction::ReadGlobalState(const GeneralSections& sections,
                                      std::function<void(IOReturn, GlobalState)> callback) {
    const size_t readSize = (sections.global.size >= 0x68) ? 0x68 : sections.global.size;
    ReadGlobalStateSized(sections, readSize, std::move(callback));
}

void DICETransaction::ReadGlobalStateFull(const GeneralSections& sections,
                                          std::function<void(IOReturn, GlobalState)> callback) {
    ReadGlobalStateSized(sections, sections.global.size, std::move(callback));
}

namespace {
constexpr size_t kStreamSectionHeaderBytes = 8;
constexpr size_t kStreamLabelsOffset = 16;
constexpr size_t kStreamLabelsBytes = 256;
constexpr size_t kStreamEntryMinCoreBytes = 16;
constexpr size_t kStreamEntryMinWithLabelsBytes = kStreamLabelsOffset + kStreamLabelsBytes;

int32_t ReadSignedQuadlet(const uint8_t* data) noexcept {
    return static_cast<int32_t>(ReadBE32(data));
}

void CopyLabelBlob(char (&dst)[256], const uint8_t* src, size_t bytesAvailable) noexcept {
    std::memset(dst, 0, sizeof(dst));
    if (!src || bytesAvailable == 0) {
        return;
    }

    const size_t copyBytes = (bytesAvailable < (sizeof(dst) - 1)) ? bytesAvailable : (sizeof(dst) - 1);
    size_t out = 0;

    // DICE stores text fields as big-endian quadlets. Decode quadlet-wise into
    // host-order bytes instead of using memcpy; this avoids alignment-sensitive
    // vectorized memmove paths on DriverKit RX payload buffers.
    while (out + 4 <= copyBytes) {
        const uint32_t q = ReadBE32(src + out);
        dst[out + 0] = static_cast<char>((q >> 24) & 0xFF);
        dst[out + 1] = static_cast<char>((q >> 16) & 0xFF);
        dst[out + 2] = static_cast<char>((q >>  8) & 0xFF);
        dst[out + 3] = static_cast<char>( q        & 0xFF);
        out += 4;
    }

    // Remainder (defensive; labels are normally quadlet-aligned).
    while (out < copyBytes) {
        dst[out] = static_cast<char>(src[out]);
        ++out;
    }

    dst[copyBytes] = '\0';
}

uint32_t ClampStreamCount(uint32_t count) noexcept {
    return (count > 4u) ? 4u : count;
}

StreamConfig ParseStreamConfig(const uint8_t* data, size_t size, bool isRxLayout) {
    StreamConfig config;
    config.isRxLayout = isRxLayout;

    if (!data || size < kStreamSectionHeaderBytes) {
        return config;
    }

    const uint32_t reportedStreams = ReadBE32(data);
    const uint32_t entryQuadlets = ReadBE32(data + 4);
    config.numStreams = ClampStreamCount(reportedStreams);
    config.entrySizeBytes = entryQuadlets * 4u;
    config.parsedEntrySizeBytes = config.entrySizeBytes;

    if (config.entrySizeBytes < kStreamEntryMinCoreBytes) {
        ASFW_LOG(DICE, "DICE %{public}s stream format: invalid entry size %u bytes (reported streams=%u)",
                 isRxLayout ? "RX" : "TX", config.entrySizeBytes, reportedStreams);
        config.numStreams = 0;
        return config;
    }

    uint32_t parsedCount = 0;
    for (uint32_t i = 0; i < config.numStreams; ++i) {
        const size_t entryBase = kStreamSectionHeaderBytes + (size_t(i) * config.parsedEntrySizeBytes);
        if (entryBase + kStreamEntryMinCoreBytes > size) {
            break;
        }

        auto& entry = config.streams[i];
        entry.isoChannel = ReadSignedQuadlet(data + entryBase + 0x00);
        if (isRxLayout) {
            entry.hasSeqStart = true;
            entry.hasSpeed = false;
            entry.seqStart = ReadBE32(data + entryBase + 0x04);
            entry.pcmChannels = ReadBE32(data + entryBase + 0x08);
            entry.midiPorts = ReadBE32(data + entryBase + 0x0C);
            entry.speed = 0;
        } else {
            entry.hasSeqStart = false;
            entry.hasSpeed = true;
            entry.seqStart = 0;
            entry.pcmChannels = ReadBE32(data + entryBase + 0x04);
            entry.midiPorts = ReadBE32(data + entryBase + 0x08);
            entry.speed = ReadBE32(data + entryBase + 0x0C);
        }

        if (config.entrySizeBytes >= kStreamEntryMinWithLabelsBytes &&
            entryBase + kStreamEntryMinWithLabelsBytes <= size) {
            CopyLabelBlob(entry.labels, data + entryBase + kStreamLabelsOffset, kStreamLabelsBytes);
        }

        ++parsedCount;
    }

    if (parsedCount < config.numStreams) {
        ASFW_LOG(DICE,
                 "DICE %{public}s stream format truncated: reported=%u clamped=%u parsed=%u readSize=%zu entrySize=%u",
                 isRxLayout ? "RX" : "TX",
                 reportedStreams,
                 ClampStreamCount(reportedStreams),
                 parsedCount,
                 size,
                 config.entrySizeBytes);
        config.numStreams = parsedCount;
    }

    return config;
}

uint32_t ComputeAm824Slots(uint32_t pcmChannels, uint32_t midiPorts) noexcept {
    return pcmChannels + ((midiPorts + 7u) / 8u);
}

void LogStreamConfigDetails(const char* prefix, const StreamConfig& config) {
    ASFW_LOG(DICE, "%{public}s Streams: count=%u active=%u entrySize=%uB pcm=%u/%u midi=%u/%u am824Slots=%u/%u disabledPcm=%u",
             prefix,
             config.numStreams,
             config.ActiveStreamCount(),
             config.entrySizeBytes,
             config.ActivePcmChannels(),
             config.TotalPcmChannels(),
             config.ActiveMidiPorts(),
             config.TotalMidiPorts(),
             config.ActiveAm824Slots(),
             config.TotalAm824Slots(),
             config.DisabledPcmChannels());
    ASFW_LOG(DICE,
             "DICE register snapshot: %{public}s streams count=%u active=%u entrySize=%uB activePcm=%u totalPcm=%u activeMidi=%u totalMidi=%u activeAm824Slots=%u totalAm824Slots=%u disabledPcm=%u",
             prefix,
             config.numStreams,
             config.ActiveStreamCount(),
             config.entrySizeBytes,
             config.ActivePcmChannels(),
             config.TotalPcmChannels(),
             config.ActiveMidiPorts(),
             config.TotalMidiPorts(),
             config.ActiveAm824Slots(),
             config.TotalAm824Slots(),
             config.DisabledPcmChannels());

    for (uint32_t i = 0; i < config.numStreams && i < 4; ++i) {
        const auto& e = config.streams[i];
        if (config.isRxLayout) { // NOSONAR(cpp:S3923): branches log different diagnostic messages
            ASFW_LOG(DICE,
                     "  %{public}s[%u]: iso=%d start=%u pcm=%u midi=%u am824Slots=%u labels='%{public}s'",
                     prefix,
                     i,
                     e.isoChannel,
                     e.seqStart,
                     e.pcmChannels,
                     e.midiPorts,
                     ComputeAm824Slots(e.pcmChannels, e.midiPorts),
                     e.labels);
            ASFW_LOG(DICE,
                     "DICE register snapshot: %{public}s[%u] iso=%d seqStart=%u pcm=%u midi=%u am824Slots=%u",
                     prefix,
                     i,
                     e.isoChannel,
                     e.seqStart,
                     e.pcmChannels,
                     e.midiPorts,
                     ComputeAm824Slots(e.pcmChannels, e.midiPorts));
        } else {
            ASFW_LOG(DICE,
                     "  %{public}s[%u]: iso=%d speed=%u pcm=%u midi=%u am824Slots=%u labels='%{public}s'",
                     prefix,
                     i,
                     e.isoChannel,
                     e.speed,
                     e.pcmChannels,
                     e.midiPorts,
                     ComputeAm824Slots(e.pcmChannels, e.midiPorts),
                     e.labels);
            ASFW_LOG(DICE,
                     "DICE register snapshot: %{public}s[%u] iso=%d speed=%u pcm=%u midi=%u am824Slots=%u",
                     prefix,
                     i,
                     e.isoChannel,
                     e.speed,
                     e.pcmChannels,
                     e.midiPorts,
                     ComputeAm824Slots(e.pcmChannels, e.midiPorts));
        }
    }
}
} // anonymous namespace

void DICETransaction::ReadRxStreamConfig(const GeneralSections& sections,
                                        std::function<void(IOReturn, StreamConfig)> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    const size_t readSize = (sections.rxStreamFormat.size > 512) ? 512 : sections.rxStreamFormat.size;
    if (sections.rxStreamFormat.size > 512) {
        ASFW_LOG(DICE, "RX stream format section (%u bytes) exceeds read limit %zu; diagnostics may be partial",
                 sections.rxStreamFormat.size, readSize);
    }
    
    io_.ReadBlock(MakeDICEAddress(sections.rxStreamFormat.offset),
                  static_cast<uint32_t>(readSize),
                  [callbackState](Async::AsyncStatus status, std::span<const uint8_t> payload) {
        if (status != Async::AsyncStatus::kSuccess) {
            Common::InvokeSharedCallback(callbackState, MapReadStatus(status), StreamConfig{});
            return;
        }

        const uint8_t* data = payload.data();
        const size_t size = payload.size();
        LogSectionPreview("ReadRxStreamConfig", data, size);
        StreamConfig config = ParseStreamConfig(data, size, true);
        LogStreamConfigDetails("RX", config);
        
        Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, config);
    });
}

void DICETransaction::ReadTxStreamConfig(const GeneralSections& sections,
                                        std::function<void(IOReturn, StreamConfig)> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    const size_t readSize = (sections.txStreamFormat.size > 512) ? 512 : sections.txStreamFormat.size;
    if (sections.txStreamFormat.size > 512) {
        ASFW_LOG(DICE, "TX stream format section (%u bytes) exceeds read limit %zu; diagnostics may be partial",
                 sections.txStreamFormat.size, readSize);
    }

    io_.ReadBlock(MakeDICEAddress(sections.txStreamFormat.offset),
              static_cast<uint32_t>(readSize),
              [callbackState](Async::AsyncStatus status, std::span<const uint8_t> payload) {
                  if (status != Async::AsyncStatus::kSuccess) {
                      Common::InvokeSharedCallback(callbackState, MapReadStatus(status), StreamConfig{});
                      return;
                  }

                  const uint8_t* data = payload.data();
                  const size_t size = payload.size();
                  LogSectionPreview("ReadTxStreamConfig", data, size);
                  StreamConfig config = ParseStreamConfig(data, size, false);
                  LogStreamConfigDetails("TX", config);

                  Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, config);
              });
}

void DICETransaction::ReadCapabilities(std::function<void(IOReturn, DICECapabilities)> callback) {
    // Use shared_ptr to manage state across async callbacks
    auto caps = std::make_shared<DICECapabilities>();
    auto sections = std::make_shared<GeneralSections>();
    
    // Step 1: Read sections
    ReadGeneralSections([this, caps, sections, callback = std::move(callback)](IOReturn status, GeneralSections secs) {
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "ReadCapabilities: failed to read sections");
            callback(status, {});
            return;
        }
        
        *sections = secs;
        
        // Step 2: Read global state
        ReadGlobalState(secs, [this, caps, sections, callback](IOReturn status, GlobalState global) {
            if (status != kIOReturnSuccess) {
                ASFW_LOG(DICE, "ReadCapabilities: failed to read global state");
                callback(status, {});
                return;
            }
            
            caps->global = global;
            
            // Step 3: Read TX streams
            ReadTxStreamConfig(*sections, [this, caps, sections, callback](IOReturn status, StreamConfig txConfig) {
                if (status != kIOReturnSuccess) {
                    ASFW_LOG(DICE, "ReadCapabilities: failed to read TX streams");
                    callback(status, {});
                    return;
                }
                
                caps->txStreams = txConfig;
                
                // Step 4: Read RX streams
                ReadRxStreamConfig(*sections, [caps, callback](IOReturn status, StreamConfig rxConfig) {
                    if (status != kIOReturnSuccess) {
                        ASFW_LOG(DICE, "ReadCapabilities: failed to read RX streams");
                        callback(status, {});
                        return;
                    }
                    
                    caps->rxStreams = rxConfig;
                    caps->valid = true;
                    
                    ASFW_LOG(DICE, "═══════════════════════════════════════════════════════");
                    ASFW_LOG(DICE, "DICE Capabilities Discovered:");
                    ASFW_LOG(DICE, "  Sample Rate: %u Hz", caps->global.sampleRate);
                    ASFW_LOG(DICE, "  Clock Caps:  0x%08x", caps->global.clockCaps);
                    ASFW_LOG(DICE, "  TX PCM/MIDI/Slots: %u/%u/%u",
                             caps->txStreams.TotalPcmChannels(),
                             caps->txStreams.TotalMidiPorts(),
                             caps->txStreams.TotalAm824Slots());
                    ASFW_LOG(DICE, "  RX PCM/MIDI/Slots: %u/%u/%u",
                             caps->rxStreams.TotalPcmChannels(),
                             caps->rxStreams.TotalMidiPorts(),
                             caps->rxStreams.TotalAm824Slots());
                    ASFW_LOG(DICE, "  Nickname:    '%{public}s'", caps->global.nickname);
                    ASFW_LOG(DICE, "═══════════════════════════════════════════════════════");
                    
                    callback(kIOReturnSuccess, *caps);
                });
            });
        });
    });
}

// Helper for GlobalState
const char* GlobalState::SupportedRatesDescription() const {
    // Return a static description based on clockCaps bits
    // Bits 0-6 correspond to 32k, 44.1k, 48k, 88.2k, 96k, 176.4k, 192k
    static char desc[128];
    desc[0] = '\0';

    if (clockCaps & RateCaps::k32000)  strlcat(desc, "32k ", sizeof(desc));
    if (clockCaps & RateCaps::k44100)  strlcat(desc, "44.1k ", sizeof(desc));
    if (clockCaps & RateCaps::k48000)  strlcat(desc, "48k ", sizeof(desc));
    if (clockCaps & RateCaps::k88200)  strlcat(desc, "88.2k ", sizeof(desc));
    if (clockCaps & RateCaps::k96000)  strlcat(desc, "96k ", sizeof(desc));
    if (clockCaps & RateCaps::k176400) strlcat(desc, "176.4k ", sizeof(desc));
    if (clockCaps & RateCaps::k192000) strlcat(desc, "192k ", sizeof(desc));

    return desc;
}

} // namespace ASFW::Audio::DICE
