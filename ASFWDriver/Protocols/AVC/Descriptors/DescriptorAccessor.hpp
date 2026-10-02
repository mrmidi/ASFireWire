// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../Commands/DescriptorCommands.hpp"
#include "../Core/IAvcUnit.hpp"
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
    /// READ DESCRIPTOR data_length per request (bytes). Unchanged from the
    /// hardware-proven reader; Apple MusicSubunitController reads in chunks too.
    static constexpr uint16_t kChunkBytes = 128;
    // Every reply must make progress. Even one-byte replies terminate within
    // the byte budget, rather than an unrelated 50-attempt cutoff.
    static constexpr size_t kMaxChunks = kMaxDescriptorBytes;
    struct ReadDescriptorResult {
        bool success{false};
        std::vector<uint8_t> data;
        std::optional<ASFW::AVC::AvcError> primaryError;
        std::optional<ASFW::AVC::AvcError> cleanupError;
        bool cancelled{false};
    };
    /// Runs exactly once; move-only so it can never be duplicated.
    using ReadCompletion = Common::MoveOnlyCallback<void(const ReadDescriptorResult&)>;
    explicit DescriptorAccessor(ASFW::AVC::IAvcUnit& unit,
                                ASFW::AVC::SubunitAddress address = ASFW::AVC::SubunitAddress::Unit());
    ~DescriptorAccessor();
    DescriptorAccessor(const DescriptorAccessor&) = delete("an accessor owns its in-flight OPEN/READ/CLOSE operation");
    DescriptorAccessor& operator=(const DescriptorAccessor&) = delete("an accessor owns its in-flight OPEN/READ/CLOSE operation");

    /// OPEN for read, READ until the declared length, CLOSE. There is no naked
    /// READ: every read holds the descriptor open (TA 2002013).
    void Read(const ASFW::AVC::Cmd::DescriptorSpecifier& specifier, ReadCompletion completion);
    void Cancel();
    /// Route loss/shutdown terminates an operation without another submission.
    void Abort();
private:
    Common::LiveRef<ASFW::AVC::IAvcUnit> unit_;
    ASFW::AVC::SubunitAddress address_;
    std::shared_ptr<DescriptorReadOperation> operation_;
};
} // namespace ASFW::Protocols::AVC
