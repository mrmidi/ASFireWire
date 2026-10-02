// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BoundedList.hpp - A fixed-capacity list: std::array plus a count.
//
// std::inplace_vector is absent from this libc++ (checked 2026-10-02). Lists
// whose length a device controls get a compile-time capacity: a full list
// refuses the next element instead of growing on device data.

#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <type_traits>

namespace ASFW::Common {

template <class T, std::size_t Capacity>
class BoundedList {
    static_assert(Capacity > 0);
    static_assert(std::is_nothrow_move_constructible_v<T>, "a variant holding this must never become valueless");

public:
    constexpr BoundedList() = default;

    /// False, and nothing stored, when the list is full.
    [[nodiscard]] constexpr bool push_back(const T& value) noexcept(std::is_nothrow_copy_assignable_v<T>) {
        if (count_ == Capacity) return false;
        items_[count_++] = value;
        return true;
    }
    constexpr void clear() noexcept { count_ = 0; }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return count_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] constexpr bool full() const noexcept { return count_ == Capacity; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

    [[nodiscard]] constexpr const T* begin() const noexcept { return items_.data(); }
    [[nodiscard]] constexpr const T* end() const noexcept { return items_.data() + count_; }
    [[nodiscard]] constexpr T* begin() noexcept { return items_.data(); }
    [[nodiscard]] constexpr T* end() noexcept { return items_.data() + count_; }
    [[nodiscard]] constexpr const T& operator[](std::size_t i) const noexcept { return items_[i]; }
    [[nodiscard]] constexpr const T& back() const noexcept { return items_[count_ - 1]; }
    [[nodiscard]] constexpr std::span<const T> view() const noexcept { return {items_.data(), count_}; }

    friend constexpr bool operator==(const BoundedList& a, const BoundedList& b) noexcept {
        if (a.count_ != b.count_) return false;
        for (std::size_t i = 0; i < a.count_; ++i) if (!(a.items_[i] == b.items_[i])) return false;
        return true;
    }

private:
    std::array<T, Capacity> items_{};
    std::size_t count_{0};
};

} // namespace ASFW::Common
