#!/bin/sh
#
# driverkit-sdk-compat.sh - build-time shims for DriverKit SDK header defects.
#
# Run as an ASFWDriver pre-build phase (see project.yml). Writes patched copies of
# SDK headers into ${DERIVED_FILE_DIR}/SDKCompat, which HEADER_SEARCH_PATHS lists
# first, so `#include <DriverKit/...>` finds the copy before the SDK framework.
# Nothing from the SDK is committed; on an SDK without the defect no copy is made
# and any stale copy is removed.
#
# safe_allocation.h (DriverKit 25.1 SDK, Xcode 26.1.x):
#   It includes <new> only when <__xnu_libcxx_sentinel.h> exists, which the
#   DriverKit SDK does not ship. Otherwise it forward-declares a plain
#   `void* operator new(size_t, void*) noexcept;`, which then clashes with
#   libc++'s inline, abi-tagged, C++26-constexpr placement new:
#     error: cannot add 'abi_tag' attribute in a redeclaration
#     error: constexpr declaration of 'operator new' follows non-constexpr declaration
#   The copy drops only the sentinel test, so the header takes its own
#   `#include <new>` branch. (The sentinel itself is not provided: arm/limits.h
#   also keys off it and would switch to a different <limits.h> path.)

set -eu

: "${SDKROOT:?SDKROOT is not set}"
: "${DERIVED_FILE_DIR:?DERIVED_FILE_DIR is not set}"

src="${SDKROOT}/System/DriverKit/System/Library/Frameworks/DriverKit.framework/Headers/safe_allocation.h"
out_dir="${DERIVED_FILE_DIR}/SDKCompat/DriverKit"
out="${out_dir}/safe_allocation.h"
defect='__has_include(<__xnu_libcxx_sentinel.h>) && __has_include(<new>)'

mkdir -p "${out_dir}"

if [ -f "${src}" ] && grep -qF "${defect}" "${src}"; then
  sed 's|__has_include(<__xnu_libcxx_sentinel.h>) && __has_include(<new>)|__has_include(<new>) /* ASFW SDKCompat: libc++ sentinel test dropped */|' \
    "${src}" > "${out}.tmp"
  if grep -qF '__xnu_libcxx_sentinel' "${out}.tmp"; then
    rm -f "${out}.tmp"
    echo "error: driverkit-sdk-compat: failed to patch ${src}" >&2
    exit 1
  fi
  mv -f "${out}.tmp" "${out}"
  echo "note: driverkit-sdk-compat: using patched safe_allocation.h (SDK forward-declares placement new)"
else
  rm -f "${out}"
fi
