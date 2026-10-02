// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Lifetime.hpp - Non-owning references that know when their target is gone.
//
// An asynchronous continuation often reaches an object by raw pointer, and the
// object may be destroyed before the continuation runs. LifetimeAnchor is a
// member the object owns (it dies with the object); LiveRef carries the pointer
// together with a weak view of that anchor, and yields nullptr once the object
// is gone. For plain C++ objects only (not OSObjects), on one serial queue:
// checking and destruction must not run concurrently.

#pragma once

#include <memory>

namespace ASFW::Common {

class LifetimeAnchor {
public:
    LifetimeAnchor() = default;
    // A copy is a different object with its own lifetime.
    LifetimeAnchor(const LifetimeAnchor&) noexcept : LifetimeAnchor() {}
    LifetimeAnchor& operator=(const LifetimeAnchor&) noexcept { return *this; }

    [[nodiscard]] std::weak_ptr<const void> Token() const noexcept { return token_; }

private:
    std::shared_ptr<const void> token_{std::make_shared<const bool>(true)};
};

/// A non-owning reference to a T exposing `LifetimeToken()`.
template <typename T>
class LiveRef {
public:
    LiveRef() = default;
    explicit LiveRef(T& target) noexcept : target_(&target), token_(target.LifetimeToken()) {}

    /// The target, or nullptr once it has been destroyed.
    [[nodiscard]] T* Get() const noexcept { return token_.expired() ? nullptr : target_; }
    [[nodiscard]] explicit operator bool() const noexcept { return Get() != nullptr; }

private:
    T* target_{nullptr};
    std::weak_ptr<const void> token_;
};

} // namespace ASFW::Common
