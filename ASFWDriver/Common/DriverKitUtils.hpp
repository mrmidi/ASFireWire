#pragma once

#include <DriverKit/OSSharedPtr.h>
#include <new>
#include <type_traits>

namespace ASFW::Common {

/// Factory helper for DriverKit OSObject-derived types.
///
/// The OSObject allocation model requires `new (std::nothrow)` + null-check + wrap in
/// OSSharedPtr. This helper encapsulates that unavoidable pattern so callsites are
/// raw-pointer-free and the single NOSONAR suppression is centralised here.
///
/// Requirements on T:
///   - Must inherit from OSObject (enforced at compile time)
///   - Must be constructible with the supplied arguments
///   - operator new must call IOMallocZero or equivalent
template <typename T, typename... Args>
[[nodiscard]] OSSharedPtr<T> MakeOSObject(Args&&... args) noexcept
{
    static_assert(std::is_base_of_v<OSObject, T>,
                  "MakeOSObject<T>() requires T to derive from OSObject");
    // NOSONAR(cpp:S5025): DriverKit OSObject uses intrusive ref-counting.
    // Allocation must be `new (std::nothrow)`; result is immediately transferred
    // into OSSharedPtr with OSNoRetain — no raw pointer escapes this function.
    auto* raw = new (std::nothrow) T(std::forward<Args>(args)...); // NOSONAR(cpp:S5025)
    if (!raw) return {};
    return OSSharedPtr<T>(raw, OSNoRetain);
}

} // namespace ASFW::Common
