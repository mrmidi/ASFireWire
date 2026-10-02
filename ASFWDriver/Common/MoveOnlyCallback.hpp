// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MoveOnlyCallback.hpp - A move-only, type-erased callable.
//
// std::move_only_function is absent from this libc++ (checked 2026-10-02), and
// std::function requires copyable targets, which would let a completion that
// must run exactly once be duplicated. This holds any move-only callable.

#pragma once

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace ASFW::Common {

template <class Signature> class MoveOnlyCallback;

template <class R, class... Args>
class MoveOnlyCallback<R(Args...)> final {
public:
    MoveOnlyCallback() noexcept = default;
    MoveOnlyCallback(std::nullptr_t) noexcept {}
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, MoveOnlyCallback> &&
                 std::is_invocable_r_v<R, std::decay_t<F>&, Args...>)
    MoveOnlyCallback(F&& callable) // NOLINT(google-explicit-constructor): a callback converts.
        : target_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(callable))) {}

    MoveOnlyCallback(MoveOnlyCallback&&) noexcept = default;
    MoveOnlyCallback& operator=(MoveOnlyCallback&&) noexcept = default;
    MoveOnlyCallback(const MoveOnlyCallback&) = delete("a move-only callback has exactly one owner");
    MoveOnlyCallback& operator=(const MoveOnlyCallback&) = delete("a move-only callback has exactly one owner");

    [[nodiscard]] explicit operator bool() const noexcept { return target_ != nullptr; }
    R operator()(Args... args) { return target_->Invoke(std::forward<Args>(args)...); }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual R Invoke(Args... args) = 0;
    };
    template <class F>
    struct Model final : Concept {
        explicit Model(F f) : callable(std::move(f)) {}
        R Invoke(Args... args) override { return std::invoke(callable, std::forward<Args>(args)...); }
        F callable;
    };
    std::unique_ptr<Concept> target_;
};

} // namespace ASFW::Common
