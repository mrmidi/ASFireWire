// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// DICETransaction.cpp - DICE async transaction implementation

#include "DICETransaction.hpp"
#include "../../../../Logging/Logging.hpp"
#include "../../../../Async/AsyncSubsystem.hpp"
#include <cstring>

namespace ASFW::Audio::DICE {

DICETransaction::DICETransaction(uint16_t nodeId)
    : nodeId_(nodeId)
{
}

Async::ReadParams DICETransaction::MakeReadParams(uint32_t offset, uint32_t length) const {
    // DICE base address: 0xFFFFE0000000
    // Split into addressHigh (upper 16 bits) and addressLow (lower 32 bits)
    uint64_t addr = kDICEBaseAddress + offset;
    
    Async::ReadParams params;
    params.destinationID = nodeId_;
    params.addressHigh = static_cast<uint32_t>((addr >> 32) & 0xFFFF);
    params.addressLow = static_cast<uint32_t>(addr & 0xFFFFFFFF);
    params.length = length;
    params.speedCode = 0xFF;  // Auto speed
    return params;
}

Async::WriteParams DICETransaction::MakeWriteParams(uint32_t offset, const void* data, uint32_t length) const {
    uint64_t addr = kDICEBaseAddress + offset;
    
    Async::WriteParams params;
    params.destinationID = nodeId_;
    params.addressHigh = static_cast<uint32_t>((addr >> 32) & 0xFFFF);
    params.addressLow = static_cast<uint32_t>(addr & 0xFFFFFFFF);
    params.payload = data;
    params.length = length;
    params.speedCode = 0xFF;  // Auto speed
    return params;
}

void DICETransaction::ReadQuadlet(Async::AsyncSubsystem& subsystem, uint32_t offset,
                                   std::function<void(IOReturn, uint32_t)> callback) {
    auto params = MakeReadParams(offset, 4);
    
    subsystem.Read(params, [callback](Async::AsyncHandle /*handle*/,
                                       Async::AsyncStatus status,
                                       uint8_t,
                                       std::span<const uint8_t> payload) {
        if (status != Async::AsyncStatus::kSuccess || payload.size() < 4) {
            IOReturn ret = (status == Async::AsyncStatus::kSuccess) ? kIOReturnUnderrun : kIOReturnError;
            callback(ret, 0);
            return;
        }
        
        uint32_t value = QuadletFromWire(payload.data());
        callback(kIOReturnSuccess, value);
    });
}

void DICETransaction::WriteQuadlet(Async::AsyncSubsystem& subsystem, uint32_t offset,
                                    uint32_t value, DICEWriteCallback callback) {
    // Use instance buffer instead of thread_local (not supported in DriverKit)
    QuadletToWire(value, writeQuadletBuffer_);
    
    auto params = MakeWriteParams(offset, writeQuadletBuffer_, 4);
    
    subsystem.Write(params, [callback](Async::AsyncHandle /*handle*/,
                                        Async::AsyncStatus status,
                                        uint8_t,
                                        std::span<const uint8_t> /*payload*/) {
        IOReturn ret = (status == Async::AsyncStatus::kSuccess) ? kIOReturnSuccess : kIOReturnError;
        callback(ret);
    });
}

void DICETransaction::ReadBlock(Async::AsyncSubsystem& subsystem, uint32_t offset,
                                 size_t byteCount, DICEReadCallback callback) {
    // Validate alignment
    if (byteCount % 4 != 0) {
        ASFW_LOG(DICE, "ReadBlock: byteCount %zu not quadlet-aligned", byteCount);
        callback(kIOReturnBadArgument, nullptr, 0);
        return;
    }
    
    // For large reads, we'd need to chunk - for now assume single read
    if (byteCount > kMaxFrameSize) {
        ASFW_LOG(DICE, "ReadBlock: byteCount %zu exceeds max frame size, chunking not yet implemented", byteCount);
        callback(kIOReturnOverrun, nullptr, 0);
        return;
    }
    
    auto params = MakeReadParams(offset, static_cast<uint32_t>(byteCount));
    
    subsystem.Read(params, [callback, byteCount](Async::AsyncHandle /*handle*/,
                                                  Async::AsyncStatus status,
                                                  uint8_t,
                                                  std::span<const uint8_t> payload) {
        if (status != Async::AsyncStatus::kSuccess) {
            callback(kIOReturnError, nullptr, 0);
            return;
        }
        
        if (payload.size() < byteCount) {
            ASFW_LOG(DICE, "ReadBlock: short read %zu < %zu", payload.size(), byteCount);
            callback(kIOReturnUnderrun, payload.data(), payload.size());
            return;
        }
        
        callback(kIOReturnSuccess, payload.data(), payload.size());
    });
}

void DICETransaction::WriteBlock(Async::AsyncSubsystem& subsystem, uint32_t offset,
                                  const uint8_t* buffer, size_t byteCount, DICEWriteCallback callback) {
    // Validate alignment
    if (byteCount % 4 != 0) {
        ASFW_LOG(DICE, "WriteBlock: byteCount %zu not quadlet-aligned", byteCount);
        callback(kIOReturnBadArgument);
        return;
    }
    
    if (byteCount > kMaxFrameSize) {
        ASFW_LOG(DICE, "WriteBlock: byteCount %zu exceeds max frame size, chunking not yet implemented", byteCount);
        callback(kIOReturnOverrun);
        return;
    }
    
    auto params = MakeWriteParams(offset, buffer, static_cast<uint32_t>(byteCount));
    
    subsystem.Write(params, [callback](Async::AsyncHandle /*handle*/,
                                        Async::AsyncStatus status,
                                        uint8_t,
                                        std::span<const uint8_t> /*payload*/) {
        IOReturn ret = (status == Async::AsyncStatus::kSuccess) ? kIOReturnSuccess : kIOReturnError;
        callback(ret);
    });
}

void DICETransaction::ReadGeneralSections(Async::AsyncSubsystem& subsystem,
                                           std::function<void(IOReturn, GeneralSections)> callback) {
    ReadBlock(subsystem, 0, GeneralSections::kWireSize, 
              [callback](IOReturn status, const uint8_t* data, size_t size) {
        if (status != kIOReturnSuccess || size < GeneralSections::kWireSize) {
            callback(status, {});
            return;
        }
        
        GeneralSections sections = GeneralSections::FromWire(data);
        
        ASFW_LOG(DICE, "ReadGeneralSections: global=%u/%u tx=%u/%u rx=%u/%u",
                 sections.global.offset, sections.global.size,
                 sections.txStreamFormat.offset, sections.txStreamFormat.size,
                 sections.rxStreamFormat.offset, sections.rxStreamFormat.size);
        
        callback(kIOReturnSuccess, sections);
    });
}

// ============================================================================
// Capability Discovery
// ============================================================================

void DICETransaction::ReadGlobalState(Async::AsyncSubsystem& subsystem, const GeneralSections& sections,
                                       std::function<void(IOReturn, GlobalState)> callback) {
    // Read enough of global section for capabilities (0x68 bytes minimum)
    const size_t readSize = (sections.global.size >= 0x68) ? 0x68 : sections.global.size;
    
    ReadBlock(subsystem, sections.global.offset, readSize,
              [callback](IOReturn status, const uint8_t* data, size_t size) {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        
        GlobalState state;
        
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
        
        callback(kIOReturnSuccess, state);
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

void DICETransaction::ReadTxStreamConfig(Async::AsyncSubsystem& subsystem, const GeneralSections& sections,
                                          std::function<void(IOReturn, StreamConfig)> callback) {
    const size_t readSize = (sections.txStreamFormat.size > 512) ? 512 : sections.txStreamFormat.size;
    if (sections.txStreamFormat.size > 512) {
        ASFW_LOG(DICE, "TX stream format section (%u bytes) exceeds read limit %zu; diagnostics may be partial",
                 sections.txStreamFormat.size, readSize);
    }
    
    ReadBlock(subsystem, sections.txStreamFormat.offset, readSize,
              [callback](IOReturn status, const uint8_t* data, size_t size) {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        
        StreamConfig config = ParseStreamConfig(data, size, false);
        LogStreamConfigDetails("TX", config);
        
        callback(kIOReturnSuccess, config);
    });
}

void DICETransaction::ReadRxStreamConfig(Async::AsyncSubsystem& subsystem, const GeneralSections& sections,
                                          std::function<void(IOReturn, StreamConfig)> callback) {
    const size_t readSize = (sections.rxStreamFormat.size > 512) ? 512 : sections.rxStreamFormat.size;
    if (sections.rxStreamFormat.size > 512) {
        ASFW_LOG(DICE, "RX stream format section (%u bytes) exceeds read limit %zu; diagnostics may be partial",
                 sections.rxStreamFormat.size, readSize);
    }
    
    ReadBlock(subsystem, sections.rxStreamFormat.offset, readSize,
              [callback](IOReturn status, const uint8_t* data, size_t size) {
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        
        StreamConfig config = ParseStreamConfig(data, size, true);
        LogStreamConfigDetails("RX", config);
        
        callback(kIOReturnSuccess, config);
    });
}

void DICETransaction::ReadCapabilities(Async::AsyncSubsystem& subsystem,
                                        std::function<void(IOReturn, DICECapabilities)> callback) {
    // Use shared_ptr to manage state across async callbacks
    auto caps = std::make_shared<DICECapabilities>();
    auto sections = std::make_shared<GeneralSections>();
    
    // Step 1: Read sections
    ReadGeneralSections(subsystem, [this, &subsystem, caps, sections, callback](IOReturn status, GeneralSections secs) {
        if (status != kIOReturnSuccess) {
            ASFW_LOG(DICE, "ReadCapabilities: failed to read sections");
            callback(status, {});
            return;
        }
        
        *sections = secs;
        
        // Step 2: Read global state
        ReadGlobalState(subsystem, secs, [this, &subsystem, caps, sections, callback](IOReturn status, GlobalState global) {
            if (status != kIOReturnSuccess) {
                ASFW_LOG(DICE, "ReadCapabilities: failed to read global state");
                callback(status, {});
                return;
            }
            
            caps->global = global;
            
            // Step 3: Read TX streams
            ReadTxStreamConfig(subsystem, *sections, [this, &subsystem, caps, sections, callback](IOReturn status, StreamConfig txConfig) {
                if (status != kIOReturnSuccess) {
                    ASFW_LOG(DICE, "ReadCapabilities: failed to read TX streams");
                    callback(status, {});
                    return;
                }
                
                caps->txStreams = txConfig;
                
                // Step 4: Read RX streams
                ReadRxStreamConfig(subsystem, *sections, [caps, callback](IOReturn status, StreamConfig rxConfig) {
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
    char* p = desc;
    *p = '\0';
    
    if (clockCaps & RateCaps::k32000)  p += std::snprintf(p, 16, "32k ");
    if (clockCaps & RateCaps::k44100)  p += std::snprintf(p, 16, "44.1k ");
    if (clockCaps & RateCaps::k48000)  p += std::snprintf(p, 16, "48k ");
    if (clockCaps & RateCaps::k88200)  p += std::snprintf(p, 16, "88.2k ");
    if (clockCaps & RateCaps::k96000)  p += std::snprintf(p, 16, "96k ");
    if (clockCaps & RateCaps::k176400) p += std::snprintf(p, 16, "176.4k ");
    if (clockCaps & RateCaps::k192000) p += std::snprintf(p, 16, "192k ");
    
    return desc;
}

} // namespace ASFW::Audio::DICE
