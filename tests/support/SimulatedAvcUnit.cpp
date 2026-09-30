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

    // 6. Dynamic Descriptor Serving (OPEN: 0x08, READ: 0x09)
    if (!descriptors_.empty() && command.size() >= 4 && command[0] == 0x00) {
        const uint8_t subunit = command[1];
        const uint8_t opcode = command[2];

        for (const auto& desc : descriptors_) {
            if (desc.subunit != subunit) continue;
            const size_t specLen = desc.specifier.size();
            if (command.size() < 3 + specLen) continue;
            if (!std::equal(desc.specifier.begin(), desc.specifier.end(), command.begin() + 3)) {
                continue;
            }

            if (opcode == 0x08 && command.size() >= 3 + specLen + 2) { // OPEN DESCRIPTOR
                const uint8_t subfunc = command[3 + specLen];
                dynamicResponseStorage_.clear();
                dynamicResponseStorage_.push_back(0x09); // Accepted
                dynamicResponseStorage_.push_back(subunit);
                dynamicResponseStorage_.push_back(0x08);
                dynamicResponseStorage_.insert(dynamicResponseStorage_.end(),
                                               desc.specifier.begin(), desc.specifier.end());
                dynamicResponseStorage_.push_back(subfunc);
                dynamicResponseStorage_.push_back(0x00); // Success status
                return std::span<const uint8_t>{dynamicResponseStorage_.data(),
                                                dynamicResponseStorage_.size()};
            }

            if (opcode == 0x09 && command.size() >= 3 + specLen + 6) { // READ DESCRIPTOR
                const size_t pOffset = 3 + specLen;
                const uint16_t reqLen = (static_cast<uint16_t>(command[pOffset + 2]) << 8) | command[pOffset + 3];
                const uint16_t reqOff = (static_cast<uint16_t>(command[pOffset + 4]) << 8) | command[pOffset + 5];

                size_t chunkLen = 0;
                uint8_t readStatus = 0x10; // Complete
                if (reqOff < desc.rawBytes.size()) {
                    const size_t avail = desc.rawBytes.size() - reqOff;
                    chunkLen = std::min(static_cast<size_t>(reqLen), avail);
                    readStatus = (reqOff + chunkLen < desc.rawBytes.size()) ? 0x11 : 0x10;
                }

                dynamicResponseStorage_.clear();
                dynamicResponseStorage_.push_back(0x09); // Accepted
                dynamicResponseStorage_.push_back(subunit);
                dynamicResponseStorage_.push_back(0x09);
                dynamicResponseStorage_.insert(dynamicResponseStorage_.end(),
                                               desc.specifier.begin(), desc.specifier.end());
                dynamicResponseStorage_.push_back(readStatus);
                dynamicResponseStorage_.push_back(0x00); // reserved
                dynamicResponseStorage_.push_back(static_cast<uint8_t>(chunkLen >> 8));
                dynamicResponseStorage_.push_back(static_cast<uint8_t>(chunkLen & 0xFF));
                dynamicResponseStorage_.push_back(static_cast<uint8_t>(reqOff >> 8));
                dynamicResponseStorage_.push_back(static_cast<uint8_t>(reqOff & 0xFF));
                if (chunkLen > 0) {
                    dynamicResponseStorage_.insert(
                        dynamicResponseStorage_.end(),
                        desc.rawBytes.begin() + reqOff,
                        desc.rawBytes.begin() + reqOff + chunkLen);
                }
                return std::span<const uint8_t>{dynamicResponseStorage_.data(),
                                                dynamicResponseStorage_.size()};
            }
        }
    }

    return std::nullopt;
}

} // namespace ASFW::AVC::Testing
