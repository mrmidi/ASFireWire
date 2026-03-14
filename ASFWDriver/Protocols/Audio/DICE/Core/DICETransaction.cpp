// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// DICETransaction.cpp - DICE async transaction implementation

#include "DICETransaction.hpp"
#include "../../../../Common/CallbackUtils.hpp"
#include "../../../../Logging/Logging.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace ASFW::Audio::DICE {

namespace {

constexpr size_t kCapabilityHexPreviewBytes = 64;

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

} // anonymous namespace

DICETransaction::DICETransaction(Protocols::Ports::FireWireBusOps& busOps,
                                 Protocols::Ports::FireWireBusInfo& busInfo,
                                 uint16_t nodeId)
    : busOps_(busOps),
      busInfo_(busInfo),
      nodeId_(FW::NodeId{static_cast<uint8_t>(nodeId & 0x3Fu)}) {}

Async::FWAddress DICETransaction::MakeAddress(uint32_t offset) const {
    const uint64_t addr = kDICEBaseAddress + offset;
    return Async::FWAddress{Async::FWAddress::AddressParts{
        .addressHi = static_cast<uint16_t>((addr >> 32U) & 0xFFFFU),
        .addressLo = static_cast<uint32_t>(addr & 0xFFFFFFFFU),
    }};
}

void DICETransaction::ReadQuadlet(uint32_t offset,
                                  std::function<void(IOReturn, uint32_t)> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    const auto gen = busInfo_.GetGeneration();
    const auto addr = MakeAddress(offset);

    busOps_.ReadBlock(gen,
                      nodeId_,
                      addr,
                      4,
                      FW::FwSpeed::S100,
                      [callbackState](Async::AsyncStatus status,
                                                       std::span<const uint8_t> payload) {
        if (status != Async::AsyncStatus::kSuccess || payload.size() < 4) {
            IOReturn ret = (status == Async::AsyncStatus::kSuccess) ? kIOReturnUnderrun : kIOReturnError;
            Common::InvokeSharedCallback(callbackState, ret, 0u);
            return;
        }
        
        uint32_t value = QuadletFromWire(payload.data());
        Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, value);
    });
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void DICETransaction::WriteQuadlet(uint32_t offset,
                                   uint32_t value,
                                   DICEWriteCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    // Use instance buffer instead of thread_local (not supported in DriverKit)
    QuadletToWire(value, writeQuadletBuffer_);

    const auto gen = busInfo_.GetGeneration();
    const auto addr = MakeAddress(offset);
    const std::span<const uint8_t> data{writeQuadletBuffer_, sizeof(writeQuadletBuffer_)};

    busOps_.WriteBlock(gen,
                       nodeId_,
                       addr,
                       data,
                       FW::FwSpeed::S100,
                       [callbackState](Async::AsyncStatus status,
                                                        std::span<const uint8_t>) {
        IOReturn ret = (status == Async::AsyncStatus::kSuccess) ? kIOReturnSuccess : kIOReturnError;
        Common::InvokeSharedCallback(callbackState, ret);
    });
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void DICETransaction::ReadBlock(uint32_t offset, size_t byteCount, DICEReadCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    // Validate alignment
    if (byteCount % 4 != 0) {
        ASFW_LOG(DICE, "ReadBlock: byteCount %zu not quadlet-aligned", byteCount);
        Common::InvokeSharedCallback(callbackState, kIOReturnBadArgument, nullptr, static_cast<size_t>(0));
        return;
    }
    
    // For large reads, we'd need to chunk - for now assume single read
    if (byteCount > kMaxFrameSize) {
        ASFW_LOG(DICE, "ReadBlock: byteCount %zu exceeds max frame size, chunking not yet implemented", byteCount);
        Common::InvokeSharedCallback(callbackState, kIOReturnOverrun, nullptr, static_cast<size_t>(0));
        return;
    }
    
    const auto gen = busInfo_.GetGeneration();
    const auto addr = MakeAddress(offset);
    const uint32_t length = static_cast<uint32_t>(byteCount);

    busOps_.ReadBlock(gen,
                      nodeId_,
                      addr,
                      length,
                      FW::FwSpeed::S100,
                      [callbackState, byteCount](Async::AsyncStatus status,
                                                                  std::span<const uint8_t> payload) {
        if (status != Async::AsyncStatus::kSuccess) {
            Common::InvokeSharedCallback(callbackState, kIOReturnError, nullptr, static_cast<size_t>(0));
            return;
        }
        
        if (payload.size() < byteCount) {
            ASFW_LOG(DICE, "ReadBlock: short read %zu < %zu", payload.size(), byteCount);
            Common::InvokeSharedCallback(callbackState, kIOReturnUnderrun, payload.data(), payload.size());
            return;
        }
        
        Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, payload.data(), payload.size());
    });
}

void DICETransaction::WriteBlock(uint32_t offset,
                                 const uint8_t* buffer,
                                 size_t byteCount,
                                 DICEWriteCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    // Validate alignment
    if (byteCount % 4 != 0) {
        ASFW_LOG(DICE, "WriteBlock: byteCount %zu not quadlet-aligned", byteCount);
        Common::InvokeSharedCallback(callbackState, kIOReturnBadArgument);
        return;
    }
    
    if (byteCount > kMaxFrameSize) {
        ASFW_LOG(DICE, "WriteBlock: byteCount %zu exceeds max frame size, chunking not yet implemented", byteCount);
        Common::InvokeSharedCallback(callbackState, kIOReturnOverrun);
        return;
    }
    
    const auto gen = busInfo_.GetGeneration();
    const auto addr = MakeAddress(offset);
    const std::span<const uint8_t> data{buffer, byteCount};

    busOps_.WriteBlock(gen,
                       nodeId_,
                       addr,
                       data,
                       FW::FwSpeed::S100,
                       [callbackState](Async::AsyncStatus status,
                                                        std::span<const uint8_t>) {
        IOReturn ret = (status == Async::AsyncStatus::kSuccess) ? kIOReturnSuccess : kIOReturnError;
        Common::InvokeSharedCallback(callbackState, ret);
    });
}

void DICETransaction::ReadGeneralSections(std::function<void(IOReturn, GeneralSections)> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ReadBlock(0, GeneralSections::kWireSize,
              [callbackState](IOReturn status, const uint8_t* data, size_t size) {
        if (status != kIOReturnSuccess || size < GeneralSections::kWireSize) {
            Common::InvokeSharedCallback(callbackState, status, GeneralSections{});
            return;
        }
        
        GeneralSections sections = GeneralSections::FromWire(data);
        LogSectionPreview("ReadGeneralSections", data, size);
        
        ASFW_LOG(DICE, "ReadGeneralSections: global=%u/%u tx=%u/%u rx=%u/%u",
                 sections.global.offset, sections.global.size,
                 sections.txStreamFormat.offset, sections.txStreamFormat.size,
                 sections.rxStreamFormat.offset, sections.rxStreamFormat.size);
        
        Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, sections);
    });
}

void DICETransaction::ReadExtensionSections(std::function<void(IOReturn, ExtensionSections)> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ReadBlock(kDICEExtensionOffset,
              ExtensionSections::kWireSize,
              [callbackState](IOReturn status, const uint8_t* data, size_t size) {
                  if (status != kIOReturnSuccess || size < ExtensionSections::kWireSize) {
                      Common::InvokeSharedCallback(callbackState, status, ExtensionSections{});
                      return;
                  }

                  ExtensionSections sections = ExtensionSections::FromWire(data);
                  LogSectionPreview("ReadExtensionSections", data, size);
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

void DICETransaction::ReadGlobalState(const GeneralSections& sections,
                                      std::function<void(IOReturn, GlobalState)> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    // Read enough of global section for capabilities (0x68 bytes minimum)
    const size_t readSize = (sections.global.size >= 0x68) ? 0x68 : sections.global.size;
    
    ReadBlock(sections.global.offset, readSize,
              [callbackState](IOReturn status, const uint8_t* data, size_t size) {
        if (status != kIOReturnSuccess) {
            Common::InvokeSharedCallback(callbackState, status, GlobalState{});
            return;
        }
        
        GlobalState state;
        LogSectionPreview("ReadGlobalState", data, size);
        
        if (size >= 8) {
            state.owner = (static_cast<uint64_t>(QuadletFromWire(data)) << 32) |
                          QuadletFromWire(data + 4);
        }
        if (size >= 12) {
            state.notification = QuadletFromWire(data + GlobalOffset::kNotification);
        }
        if (size >= 0x4C) {
            // Extract nickname (64 bytes = 16 quadlets starting at offset 0x0C)
            // DICE stores strings as big-endian quadlets, so we need to read each
            // 4-byte group as a quadlet and extract chars in big-endian order
            size_t nickIdx = 0;
            for (size_t q = 0; q < 16 && nickIdx < 63; ++q) {
                size_t qOffset = 0x0C + q * 4;
                if (qOffset + 4 > size) break;
                
                uint32_t quadlet = QuadletFromWire(data + qOffset);
                
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
            state.clockSelect = QuadletFromWire(data + GlobalOffset::kClockSelect);
        }
        if (size >= 0x54) {
            state.enabled = (QuadletFromWire(data + GlobalOffset::kEnable) != 0);
        }
        if (size >= 0x58) {
            state.status = QuadletFromWire(data + GlobalOffset::kStatus);
        }
        if (size >= 0x5C) {
            state.extStatus = QuadletFromWire(data + GlobalOffset::kExtStatus);
        }
        if (size >= 0x60) {
            state.sampleRate = QuadletFromWire(data + GlobalOffset::kSampleRate);
        }
        if (size >= 0x64) {
            state.version = QuadletFromWire(data + GlobalOffset::kVersion);
        }
        if (size >= 0x68) {
            state.clockCaps = QuadletFromWire(data + GlobalOffset::kClockCaps);
        }
        
        ASFW_LOG(DICE, "Global: rate=%uHz caps=0x%08x version=0x%08x nickname='%{public}s'",
                 state.sampleRate, state.clockCaps, state.version, state.nickname);
        
        Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, state);
    });
}

namespace {
constexpr size_t kStreamSectionHeaderBytes = 8;
constexpr size_t kStreamLabelsOffset = 16;
constexpr size_t kStreamLabelsBytes = 256;
constexpr size_t kStreamEntryMinCoreBytes = 16;
constexpr size_t kStreamEntryMinWithLabelsBytes = kStreamLabelsOffset + kStreamLabelsBytes;

int32_t ReadSignedQuadlet(const uint8_t* data) noexcept {
    return static_cast<int32_t>(DICETransaction::QuadletFromWire(data));
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
        const uint32_t q = DICETransaction::QuadletFromWire(src + out);
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

    const uint32_t reportedStreams = DICETransaction::QuadletFromWire(data);
    const uint32_t entryQuadlets = DICETransaction::QuadletFromWire(data + 4);
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
            entry.seqStart = DICETransaction::QuadletFromWire(data + entryBase + 0x04);
            entry.pcmChannels = DICETransaction::QuadletFromWire(data + entryBase + 0x08);
            entry.midiPorts = DICETransaction::QuadletFromWire(data + entryBase + 0x0C);
            entry.speed = 0;
        } else {
            entry.hasSeqStart = false;
            entry.hasSpeed = true;
            entry.seqStart = 0;
            entry.pcmChannels = DICETransaction::QuadletFromWire(data + entryBase + 0x04);
            entry.midiPorts = DICETransaction::QuadletFromWire(data + entryBase + 0x08);
            entry.speed = DICETransaction::QuadletFromWire(data + entryBase + 0x0C);
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
    ASFW_LOG(DICE, "%{public}s Streams: count=%u entrySize=%uB pcm=%u midi=%u am824Slots=%u",
             prefix,
             config.numStreams,
             config.entrySizeBytes,
             config.TotalPcmChannels(),
             config.TotalMidiPorts(),
             config.TotalAm824Slots());

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
    
    ReadBlock(sections.rxStreamFormat.offset, readSize,
              [callbackState](IOReturn status, const uint8_t* data, size_t size) {
        if (status != kIOReturnSuccess) {
            Common::InvokeSharedCallback(callbackState, status, StreamConfig{});
            return;
        }
        
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

    ReadBlock(sections.txStreamFormat.offset,
              readSize,
              [callbackState](IOReturn status, const uint8_t* data, size_t size) {
                  if (status != kIOReturnSuccess) {
                      Common::InvokeSharedCallback(callbackState, status, StreamConfig{});
                      return;
                  }

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

void DICETransaction::CompareSwapOctlet(uint32_t offset,
                                        uint64_t expected,
                                        uint64_t desired,
                                        DICEOctletCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    const auto gen = busInfo_.GetGeneration();
    const auto addr = MakeAddress(offset);

    std::array<uint8_t, 16> operand{};
    OctletToWire(expected, operand.data());
    OctletToWire(desired, operand.data() + 8);

    busOps_.Lock(gen,
                 nodeId_,
                 addr,
                 FW::LockOp::kCompareSwap,
                 std::span<const uint8_t>{operand},
                 8,
                 FW::FwSpeed::S100,
                 [callbackState](Async::AsyncStatus status, std::span<const uint8_t> payload) {
                     if (status != Async::AsyncStatus::kSuccess || payload.size() < 8) {
                         const IOReturn ret =
                             (status == Async::AsyncStatus::kSuccess) ? kIOReturnUnderrun : kIOReturnError;
                         Common::InvokeSharedCallback(callbackState, ret, uint64_t{0});
                         return;
                     }

                     Common::InvokeSharedCallback(callbackState,
                                                  kIOReturnSuccess,
                                                  OctletFromWire(payload.data()));
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
