// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DescriptorTypes.hpp"
#include "../Commands/DescriptorCommands.hpp"
#include "../Core/IAvcUnit.hpp"
#include "../AVCDefs.hpp"
#include "../../../Common/MoveOnlyCallback.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ASFW::Protocols::AVC {
class DescriptorReadOperation;
/// Serial-queue owner of a generation/route-bound descriptor operation.
class DescriptorAccessor final {
public:
    static constexpr size_t kMaxDescriptorBytes = 4096;
    // Every reply must make progress. Even one-byte replies terminate within
    // the byte budget, rather than an unrelated 50-attempt cutoff.
    static constexpr size_t kMaxChunks = kMaxDescriptorBytes;
    struct ReadDescriptorResult {
        bool success{false};
        std::vector<uint8_t> data;
        AVCResult avcResult{AVCResult::kRejected};
        std::optional<ASFW::AVC::AvcError> primaryError;
        std::optional<ASFW::AVC::AvcError> cleanupError;
        bool cancelled{false};
    };
    /// Runs exactly once; move-only so it can never be duplicated.
    using ReadCompletion = Common::MoveOnlyCallback<void(const ReadDescriptorResult&)>;
    DescriptorAccessor(ASFW::AVC::IAvcUnit& unit, uint8_t address = 0xFF);
    DescriptorAccessor(ASFW::AVC::IAvcUnit& unit, ASFW::AVC::SubunitAddress address);
    ~DescriptorAccessor();
    DescriptorAccessor(const DescriptorAccessor&) = delete("an accessor owns its in-flight OPEN/READ/CLOSE operation");
    DescriptorAccessor& operator=(const DescriptorAccessor&) = delete("an accessor owns its in-flight OPEN/READ/CLOSE operation");

    void readUnitIdentifier(ReadCompletion completion);
    void readStatusDescriptor(uint8_t type, ReadCompletion completion);
    void readWithOpenCloseSequence(const DescriptorSpecifier& specifier, ReadCompletion completion);
    void readWithOpenCloseSequence(const ASFW::AVC::Cmd::DescriptorSpecifier& specifier, ReadCompletion completion);
    /// No naked READ API: this compatibility entry point also acquires OPEN.
    void readComplete(const DescriptorSpecifier& specifier, ReadCompletion completion);
    void Cancel();
    /// Route loss/shutdown terminates an operation without another submission.
    void Abort();
private:
    Common::LiveRef<ASFW::AVC::IAvcUnit> unit_;
    ASFW::AVC::SubunitAddress address_;
    std::shared_ptr<DescriptorReadOperation> operation_;
};
} // namespace ASFW::Protocols::AVC
