// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FcpExchangeRecorder.hpp - What one AV/C unit was asked, and what it answered.
//
// Every bring-up path (generic discovery, BeBoB plug probes, vendor commands)
// sends through FCPTransport, so recording there captures the complete
// conversation without each path reporting its own results. The AV/C Report
// exports the log; tools/avc/avc_discover.py --replay rebuilds the graph from it.
//
// Not thread-safe: the owning transport serialises access under its lock.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ASFW::Protocols::AVC {

/// How an exchange ended. A response's own code (ACCEPTED, NOT IMPLEMENTED…)
/// is in its first byte; this says whether there was a response at all.
enum class FcpExchangeOutcome : uint8_t {
    kResponse = 0,
    kTimeout,
    kBusReset,
    kTransportError,
    kResponseMismatch,
    kRefusedByFilter,  ///< Never sent: not in the device's permitted command set.
    kBusy,             ///< Never sent: another command was pending.
    kInvalid,          ///< Never sent: malformed frame.
};

struct FcpExchangeRecord {
    uint32_t sequence{0};
    uint32_t generation{0};
    FcpExchangeOutcome outcome{FcpExchangeOutcome::kResponse};
    bool interim{false};
    uint8_t retries{0};
    /// From the first write to the outcome, replays included; 0 if never sent.
    uint64_t elapsedNs{0};
    std::vector<uint8_t> command;
    std::vector<uint8_t> response;
};

struct FcpExchangeLog {
    /// Increments each time a session starts (attach, manual refresh).
    uint32_t session{0};
    /// Exchanges not kept because the session's budget was spent.
    uint32_t dropped{0};
    std::vector<FcpExchangeRecord> records;
};

class FcpExchangeRecorder {
public:
    // A Phase 88 discovery with descriptors is ~160 exchanges of <=512 bytes.
    static constexpr size_t kDefaultMaxRecords = 1024;
    static constexpr size_t kDefaultMaxBytes = size_t{256} * 1024;

    explicit FcpExchangeRecorder(size_t maxRecords = kDefaultMaxRecords,
                                 size_t maxBytes = kDefaultMaxBytes) noexcept
        : maxRecords_(maxRecords), maxBytes_(maxBytes) {}

    /// Forget the previous session and keep what follows. The first
    /// exchanges of a session are the discovery, so a full session drops
    /// new exchanges rather than overwriting old ones.
    void BeginSession() {
        log_.records.clear();
        log_.dropped = 0;
        ++log_.session;
        bytes_ = 0;
    }

    void Record(uint32_t generation, FcpExchangeOutcome outcome, bool interim, uint8_t retries,
                std::span<const uint8_t> command, std::span<const uint8_t> response,
                uint64_t elapsedNs = 0) {
        const size_t size = command.size() + response.size();
        ++sequence_;
        if (log_.records.size() >= maxRecords_ || bytes_ + size > maxBytes_) {
            ++log_.dropped;
            return;
        }
        bytes_ += size;
        log_.records.push_back(FcpExchangeRecord{
            .sequence = sequence_,
            .generation = generation,
            .outcome = outcome,
            .interim = interim,
            .retries = retries,
            .elapsedNs = elapsedNs,
            .command = {command.begin(), command.end()},
            .response = {response.begin(), response.end()},
        });
    }

    [[nodiscard]] const FcpExchangeLog& Log() const noexcept { return log_; }

private:
    size_t maxRecords_;
    size_t maxBytes_;
    size_t bytes_{0};
    uint32_t sequence_{0};
    FcpExchangeLog log_{};
};

} // namespace ASFW::Protocols::AVC
