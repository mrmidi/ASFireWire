// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SimulatedAvcUnit.cpp - Implementation of SimulatedAvcUnit.

#include "SimulatedAvcUnit.hpp"

#include <algorithm>

namespace ASFW::AVC::Testing {

void SimulatedAvcUnit::Submit(const CommandFrame& frame,
                              FW::Generation generation,
                              ResponseCallback completion) {
    if (generation != generation_) {
        completion(std::unexpected(AvcError::Of(AvcErrorKind::kBusReset)));
        return;
    }

    if (faults_.busResetNext) {
        faults_.busResetNext = false;
        generation_ = FW::Generation{generation_.value + 1};
        completion(std::unexpected(AvcError::Of(AvcErrorKind::kBusReset)));
        return;
    }

    if (faults_.timeoutNext) {
        faults_.timeoutNext = false;
        // Dropped response: simulate timeout without calling completion
        return;
    }

    auto respOpt = FindResponse(frame.Bytes());
    if (!respOpt) {
        respOpt = FindResponse(frame.WireBytes());
    }

    if (respOpt) {
        auto respBytes = *respOpt;

        if (faults_.interimNext) {
            faults_.interimNext = false;
            std::vector<uint8_t> interim(respBytes.begin(), respBytes.end());
            interim[0] = static_cast<uint8_t>(ResponseCode::kInterim);
            auto interimParsed = ParseResponseFor(frame, interim);
            if (interimParsed) {
                completion(*interimParsed);
            }
        }

        if (faults_.rejectNext) {
            faults_.rejectNext = false;
            std::vector<uint8_t> rejected(respBytes.begin(), respBytes.end());
            rejected[0] = static_cast<uint8_t>(ResponseCode::kRejected);
            completion(ParseResponseFor(frame, rejected));
            return;
        }

        if (faults_.notImplementedNext) {
            faults_.notImplementedNext = false;
            std::vector<uint8_t> notImpl(respBytes.begin(), respBytes.end());
            notImpl[0] = static_cast<uint8_t>(ResponseCode::kNotImplemented);
            completion(ParseResponseFor(frame, notImpl));
            return;
        }

        completion(ParseResponseFor(frame, respBytes));
        return;
    }

    // Default response when command is not in captured fixtures: NOT IMPLEMENTED
    const uint8_t notImplBytes[3] = {
        static_cast<uint8_t>(ResponseCode::kNotImplemented),
        frame.Address().Byte(),
        static_cast<uint8_t>(frame.OpcodeValue()),
    };
    completion(ParseResponseFor(frame, notImplBytes));
}

void SimulatedAvcUnit::AttachToBus(
    ::ASFW::Testing::RecordingFireWireBus& bus,
    FcpResponseSink responseSink) {

    bus.SetWriteResponder([this, responseSink = std::move(responseSink)](
                              Async::FWAddress address,
                              std::span<const uint8_t> data) {
        // FCP command register is 0xFFFFF0000B00
        if (address.addressHi == 0xFFFF && address.addressLo == 0xF0000B00) {
            if (faults_.timeoutNext) {
                faults_.timeoutNext = false;
                return;
            }

            if (faults_.busResetNext) {
                faults_.busResetNext = false;
                generation_ = FW::Generation{generation_.value + 1};
                return;
            }

            auto respOpt = FindResponse(data);
            if (!respOpt) {
                const uint8_t notImplBytes[3] = {
                    0x08, // NOT IMPLEMENTED
                    data.size() > 1 ? data[1] : uint8_t{0xFF},
                    data.size() > 2 ? data[2] : uint8_t{0x00},
                };
                responseSink(nodeId_.value, generation_.value, notImplBytes);
                return;
            }

            auto respBytes = *respOpt;

            if (faults_.interimNext) {
                faults_.interimNext = false;
                std::vector<uint8_t> interim(respBytes.begin(), respBytes.end());
                interim[0] = 0x0F; // INTERIM
                responseSink(nodeId_.value, generation_.value, interim);
            }

            if (faults_.rejectNext) {
                faults_.rejectNext = false;
                std::vector<uint8_t> rejected(respBytes.begin(), respBytes.end());
                rejected[0] = 0x0A; // REJECTED
                responseSink(nodeId_.value, generation_.value, rejected);
                return;
            }

            if (faults_.notImplementedNext) {
                faults_.notImplementedNext = false;
                std::vector<uint8_t> notImpl(respBytes.begin(), respBytes.end());
                notImpl[0] = 0x08; // NOT IMPLEMENTED
                responseSink(nodeId_.value, generation_.value, notImpl);
                return;
            }

            responseSink(nodeId_.value, generation_.value, respBytes);
        }
    });
}

std::optional<std::span<const uint8_t>> SimulatedAvcUnit::FindResponse(
    std::span<const uint8_t> command) const {

    // Check user-configured overrides first
    for (const auto& overrideEntry : overrides_) {
        if (command.size() >= overrideEntry.commandPrefix.size() &&
            std::equal(overrideEntry.commandPrefix.begin(),
                       overrideEntry.commandPrefix.end(),
                       command.begin())) {
            return std::span<const uint8_t>{overrideEntry.response.data(),
                                            overrideEntry.response.size()};
        }
    }

    if (command.empty()) {
        return std::nullopt;
    }

    // Match against device image records
    for (const auto& record : image_.records) {
        // 1. Exact byte length and content match
        if (record.command.size() == command.size()) {
            if (std::equal(record.command.begin(), record.command.end(), command.begin())) {
                return record.response;
            }
        }

        // 2. Incoming command is zero-padded to quadlet boundary
        if (command.size() > record.command.size()) {
            if (std::equal(record.command.begin(), record.command.end(), command.begin())) {
                bool paddingAllZeros = true;
                for (size_t i = record.command.size(); i < command.size(); ++i) {
                    if (command[i] != 0) {
                        paddingAllZeros = false;
                        break;
                    }
                }
                if (paddingAllZeros) {
                    return record.response;
                }
            }
        }

        // 3. Special case: UNIT INFO (0x30)
        // Apple & legacy send 0 operands [01, FF, 30] (3 bytes, or 4 padded).
        // Captured Linux fixtures record [01, FF, 30, 07, FF, FF, FF, FF] (8 bytes).
        if (command.size() >= 3 && command[0] == 0x01 && command[1] == 0xFF && command[2] == 0x30) {
            if (record.command.size() >= 3 && record.command[0] == 0x01 &&
                record.command[1] == 0xFF && record.command[2] == 0x30) {
                return record.response;
            }
        }

        // 4. Special case: SUBUNIT INFO (0x31) page query matching
        if (command.size() >= 4 && command[0] == 0x01 && command[1] == 0xFF && command[2] == 0x31) {
            if (record.command.size() >= 4 && record.command[0] == 0x01 &&
                record.command[1] == 0xFF && record.command[2] == 0x31 &&
                record.command[3] == command[3]) {
                return record.response;
            }
        }

        // 5. Special case: PLUG INFO (0x02) unit plug info query (subfunction 0x00)
        // Linux/BeBoB sends 0x00 for the trailing query operands; captured fixtures record 0xFF wildcards.
        if (command.size() >= 4 && command[0] == 0x01 && command[1] == 0xFF &&
            command[2] == 0x02 && command[3] == 0x00) {
            if (record.command.size() >= 4 && record.command[0] == 0x01 &&
                record.command[1] == 0xFF && record.command[2] == 0x02 &&
                record.command[3] == 0x00) {
                return record.response;
            }
        }
    }

    return std::nullopt;
}

} // namespace ASFW::AVC::Testing
