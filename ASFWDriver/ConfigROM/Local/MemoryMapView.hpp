#pragma once

#include <DriverKit/IOMemoryMap.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace ASFW::Driver {

enum class MemoryMapViewError : uint8_t {
    Unaligned,
    TooSmall,
};

class MemoryMapView {
  public:
    explicit MemoryMapView(IOMemoryMap& map) noexcept : map_(map) {}

    [[nodiscard]] std::span<std::byte> Bytes() const noexcept {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        auto* const base = reinterpret_cast<std::byte*>(static_cast<uintptr_t>(map_.GetAddress()));
        const auto length = static_cast<size_t>(map_.GetLength());
        return {base, length};
    }

    template <typename T>
    [[nodiscard]] std::expected<std::span<T>, MemoryMapViewError>
    Span(size_t elementCount) const noexcept {
        const uintptr_t addr = static_cast<uintptr_t>(map_.GetAddress());
        if ((addr % alignof(T)) != 0) {
            return std::unexpected(MemoryMapViewError::Unaligned);
        }

        const auto lengthBytes = static_cast<size_t>(map_.GetLength());
        const auto neededBytes = elementCount * sizeof(T);
        if (neededBytes > lengthBytes) {
            return std::unexpected(MemoryMapViewError::TooSmall);
        }

        auto* const base = reinterpret_cast<T*>(addr); // NOLINT(performance-no-int-to-ptr)
        return std::span<T>{base, elementCount};
    }

  private:
    IOMemoryMap& map_;
};

} // namespace ASFW::Driver
