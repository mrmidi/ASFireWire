// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// SPro24DspRouting.hpp - Pure routing helpers for Saffire Pro 24 DSP
//
// Important: these helpers model only the visible TCAT router image exposed by the
// DICE extension sections. The legacy Focusrite stack also maintained hidden block-7/8
// companion routes above this graph, so a "correct" visible route set is not yet proof
// that the full Pro24DSP headphone path matches MixControl behavior.

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace ASFW::Audio::DICE::Focusrite::SPro24DspRouting {

struct RouterEndpoint {
    uint8_t blockId{0};
    uint8_t channel{0};
};

struct RouterEntry {
    RouterEndpoint dst{};
    RouterEndpoint src{};
    uint16_t peak{0};
};

struct StereoPlaybackMirrorTarget {
    uint8_t leftDstChannel{0};
    uint8_t rightDstChannel{0};
    std::string_view label{};
};

constexpr uint8_t kDstBlkIns0 = 0x04;
constexpr uint8_t kSrcBlkAvs0 = 0x0B;

inline constexpr StereoPlaybackMirrorTarget kMonitor12Mirror{
    .leftDstChannel = 0,
    .rightDstChannel = 1,
    .label = "line out 1/2",
};

inline constexpr StereoPlaybackMirrorTarget kHeadphone1Mirror{
    .leftDstChannel = 2,
    .rightDstChannel = 3,
    .label = "line out 3/4 (Headphone 1 mirror)",
};

inline constexpr StereoPlaybackMirrorTarget kHeadphone2Mirror{
    .leftDstChannel = 4,
    .rightDstChannel = 5,
    .label = "line out 5/6 (Headphone 2 mirror)",
};

inline bool HasRoute(const std::vector<RouterEntry>& entries,
                     uint8_t dstBlockId,
                     uint8_t dstChannel,
                     uint8_t srcBlockId,
                     uint8_t srcChannel) {
    for (const auto& entry : entries) {
        if (entry.dst.blockId == dstBlockId &&
            entry.dst.channel == dstChannel &&
            entry.src.blockId == srcBlockId &&
            entry.src.channel == srcChannel) {
            return true;
        }
    }
    return false;
}

inline void UpsertRoute(std::vector<RouterEntry>& entries,
                        uint8_t dstBlockId,
                        uint8_t dstChannel,
                        uint8_t srcBlockId,
                        uint8_t srcChannel) {
    for (auto& entry : entries) {
        if (entry.dst.blockId == dstBlockId && entry.dst.channel == dstChannel) {
            entry.src.blockId = srcBlockId;
            entry.src.channel = srcChannel;
            entry.peak = 0;
            return;
        }
    }

    entries.push_back({
        .dst = {.blockId = dstBlockId, .channel = dstChannel},
        .src = {.blockId = srcBlockId, .channel = srcChannel},
        .peak = 0,
    });
}

inline bool HasStereoPlaybackMirror(const std::vector<RouterEntry>& entries,
                                    const StereoPlaybackMirrorTarget& target) {
    return HasRoute(entries, kDstBlkIns0, target.leftDstChannel, kSrcBlkAvs0, 0) &&
           HasRoute(entries, kDstBlkIns0, target.rightDstChannel, kSrcBlkAvs0, 1);
}

inline bool HasAnyHeadphonePlaybackMirror(const std::vector<RouterEntry>& entries) {
    return HasStereoPlaybackMirror(entries, kHeadphone1Mirror) ||
           HasStereoPlaybackMirror(entries, kHeadphone2Mirror);
}

inline void ApplyStereoPlaybackMirror(std::vector<RouterEntry>& entries,
                                      const StereoPlaybackMirrorTarget& target) {
    UpsertRoute(entries, kDstBlkIns0, target.leftDstChannel, kSrcBlkAvs0, 0);
    UpsertRoute(entries, kDstBlkIns0, target.rightDstChannel, kSrcBlkAvs0, 1);
}

inline std::string_view DescribeIns0Destination(uint8_t channel) {
    switch (channel) {
    case 0:
    case 1:
        return kMonitor12Mirror.label;
    case 2:
    case 3:
        return kHeadphone1Mirror.label;
    case 4:
    case 5:
        return kHeadphone2Mirror.label;
    default:
        return "unknown Ins0 destination";
    }
}

} // namespace ASFW::Audio::DICE::Focusrite::SPro24DspRouting
