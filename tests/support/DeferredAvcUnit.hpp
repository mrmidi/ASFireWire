// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DeferredAvcUnit.hpp - An AV/C unit whose replies outlive it.
//
// Submitted completions go to a test-owned queue, so a test can destroy the
// unit and then deliver a reply, as the transaction engine can (its bus
// callbacks and timers keep it alive past its unit). Allocate the unit on the
// heap: AddressSanitizer then reports any continuation that touches it.

#pragma once

#include "ASFWDriver/Protocols/AVC/Core/IAvcUnit.hpp"

#include <memory>
#include <optional>

namespace ASFW::AVC::Testing {

class DeferredAvcUnit final : public IAvcUnit {
public:
    /// One outstanding reply, as the engine allows. (A std::vector of these
    /// callbacks trips a libc++ recursive-constraint error in TUs that use
    /// `using namespace ASFW::AVC`.)
    struct Pending {
        std::optional<ResponseCallback> callback;
        size_t submitted{0};
    };

    explicit DeferredAvcUnit(std::shared_ptr<Pending> pending) : pending_(std::move(pending)) {}

    void Submit(const CommandFrame&, FW::Generation, ResponseCallback completion) override {
        ++pending_->submitted;
        pending_->callback = std::move(completion);
    }
    [[nodiscard]] FW::NodeId NodeId() const noexcept override { return FW::NodeId{0}; }
    [[nodiscard]] FW::Generation CurrentGeneration() const noexcept override { return FW::Generation{1}; }
    [[nodiscard]] uint64_t Guid() const noexcept override { return 0x000aac0300b1d1f7ULL; }

private:
    std::shared_ptr<Pending> pending_;
};

} // namespace ASFW::AVC::Testing
