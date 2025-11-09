// Error.hpp - Modern C++23 error handling with std::expected
//
// Provides rich error context with source location tracking for improved debugging.
// Replaces raw kern_return_t with type-safe Result<T, Error> pattern.
//
// Key features:
// - Automatic source location capture (file, line, function)
// - Error severity levels (Recoverable, Fatal, Warning)
// - Zero-cost abstractions (compile-time validation)
// - Propagation helpers for clean error handling
//
// Usage:
//   Result<Transaction*> CreateTransaction(uint32_t txid) {
//       if (txid == 0) {
//           return ASFW_ERROR_INVALID("Transaction ID cannot be zero");
//       }
//       auto* txn = new Transaction(txid);
//       if (!txn) {
//           return ASFW_ERROR_FATAL(kIOReturnNoMemory, "Failed to allocate Transaction");
//       }
//       return txn;
//   }
//
//   // Caller
//   auto result = CreateTransaction(42);
//   if (!result) {
//       result.error().Log();  // Logs with file:line:function context
//       return result.error().kr;
//   }
//   Transaction* txn = result.value();

#pragma once

#include <expected>
#include <string_view>
#include <DriverKit/IOReturn.h>
#include "../../Logging/Logging.hpp"

namespace ASFW::Async {

// ============================================================================
// Source Location (C++20 std::source_location alternative)
// ============================================================================

/// Compile-time source location tracking
/// Uses compiler builtins for zero-overhead location capture
struct SourceLocation {
    const char* file;
    const char* function;
    int line;

    /// Construct with automatic location capture via compiler builtins
    /// Default parameters capture call site when no arguments provided
    constexpr SourceLocation(
        const char* f = __builtin_FILE(),
        const char* fn = __builtin_FUNCTION(),
        int l = __builtin_LINE()) noexcept
        : file(f), function(fn), line(l) {}

    /// Extract filename from full path (strips directory)
    [[nodiscard]] constexpr std::string_view FileName() const noexcept {
        std::string_view path(file);
        auto pos = path.find_last_of('/');
        return (pos != std::string_view::npos) ? path.substr(pos + 1) : path;
    }

    /// Format as "file:line" for compact logging
    [[nodiscard]] constexpr const char* FileAndLine() const noexcept {
        return file;  // Caller should format as needed
    }
};

// ============================================================================
// Error Severity
// ============================================================================

/// Error severity levels for compile-time categorization
enum class ErrorSeverity : uint8_t {
    /// Recoverable error - can retry or continue with degraded functionality
    Recoverable,

    /// Fatal error - cannot continue, must abort operation
    Fatal,

    /// Warning - non-blocking issue, logged but operation continues
    Warning
};

/// Convert severity to string for logging
[[nodiscard]] constexpr const char* ToString(ErrorSeverity severity) noexcept {
    switch (severity) {
        case ErrorSeverity::Recoverable: return "RECOVERABLE";
        case ErrorSeverity::Fatal:       return "FATAL";
        case ErrorSeverity::Warning:     return "WARNING";
    }
    return "UNKNOWN";
}

// ============================================================================
// Error Type
// ============================================================================

/// Rich error context with source location and severity
/// Designed for zero-overhead abstraction with compile-time validation
struct Error {
    kern_return_t kr;              ///< IOKit error code
    SourceLocation location;       ///< Capture site (file, line, function)
    ErrorSeverity severity;        ///< Error severity level
    const char* message;           ///< Human-readable description

    /// Compile-time error factory with automatic source location capture
    /// Use macros ASFW_ERROR_RECOVERABLE, ASFW_ERROR_FATAL, ASFW_ERROR_WARNING instead
    [[nodiscard]] static constexpr Error Make(
        kern_return_t kr,
        ErrorSeverity sev,
        const char* msg,
        SourceLocation loc = SourceLocation()) noexcept
    {
        return Error{kr, loc, sev, msg};
    }

    /// Check if error is recoverable (can retry)
    [[nodiscard]] constexpr bool IsRecoverable() const noexcept {
        return severity == ErrorSeverity::Recoverable;
    }

    /// Check if error is fatal (must abort)
    [[nodiscard]] constexpr bool IsFatal() const noexcept {
        return severity == ErrorSeverity::Fatal;
    }

    /// Check if error is a warning (non-blocking)
    [[nodiscard]] constexpr bool IsWarning() const noexcept {
        return severity == ErrorSeverity::Warning;
    }

    /// Log error with full context (file, line, function, message)
    void Log() const noexcept {
        ASFW_LOG_ERROR(Async,
                       "[%{public}s] %{public}s:%d in %{public}s() - kr=0x%08x (%{public}s)",
                       ToString(severity),
                       location.FileName().data(),
                       location.line,
                       location.function,
                       kr,
                       message);
    }

    /// Log error as warning (for non-fatal errors)
    void LogAsWarning() const noexcept {
        ASFW_LOG(Async,
                 "[%{public}s] %{public}s:%d in %{public}s() - kr=0x%08x (%{public}s)",
                 ToString(severity),
                 location.FileName().data(),
                 location.line,
                 location.function,
                 kr,
                 message);
    }
};

// Compile-time validation
// Error struct is 48 bytes on 64-bit (kern_return_t + SourceLocation + severity + message pointer)
static_assert(sizeof(Error) <= 64, "Error must be cache-line friendly (≤64 bytes)");

// ============================================================================
// Result Type (std::expected alias)
// ============================================================================

/// Result type for operations that can fail
/// Wraps std::expected<T, Error> for type-safe error handling
///
/// Usage:
///   Result<int> Divide(int a, int b) {
///       if (b == 0) {
///           return ASFW_ERROR_INVALID("Division by zero");
///       }
///       return a / b;
///   }
///
///   auto result = Divide(10, 2);
///   if (result) {
///       int value = *result;  // or result.value()
///   } else {
///       result.error().Log();
///   }
template<typename T>
using Result = std::expected<T, Error>;

/// Specialization for void return (operation that can fail but has no value)
/// Use Result<void> for functions that return kern_return_t today
///
/// Note: std::expected<void, E> is valid in C++23
/// For operations that only signal success/failure without a return value

// ============================================================================
// Error Creation Macros (with automatic source location)
// ============================================================================

/// Create recoverable error (can retry)
#define ASFW_ERROR_RECOVERABLE(kr, msg) \
    std::unexpected(Error::Make((kr), ErrorSeverity::Recoverable, (msg)))

/// Create fatal error (must abort)
#define ASFW_ERROR_FATAL(kr, msg) \
    std::unexpected(Error::Make((kr), ErrorSeverity::Fatal, (msg)))

/// Create warning (non-blocking)
#define ASFW_ERROR_WARNING(kr, msg) \
    std::unexpected(Error::Make((kr), ErrorSeverity::Warning, (msg)))

/// Create invalid argument error (common case)
#define ASFW_ERROR_INVALID(msg) \
    ASFW_ERROR_FATAL(kIOReturnBadArgument, (msg))

/// Create not ready error (common case)
#define ASFW_ERROR_NOT_READY(msg) \
    ASFW_ERROR_RECOVERABLE(kIOReturnNotReady, (msg))

/// Create timeout error (common case)
#define ASFW_ERROR_TIMEOUT(msg) \
    ASFW_ERROR_RECOVERABLE(kIOReturnTimeout, (msg))

/// Create no memory error (common case)
#define ASFW_ERROR_NO_MEMORY(msg) \
    ASFW_ERROR_FATAL(kIOReturnNoMemory, (msg))

/// Create no space error (ring full, common case)
#define ASFW_ERROR_NO_SPACE(msg) \
    ASFW_ERROR_RECOVERABLE(kIOReturnNoSpace, (msg))

// ============================================================================
// Error Propagation Helpers
// ============================================================================

/// Try macro - propagate error or extract value
/// Similar to Rust's ? operator
///
/// Usage:
///   Result<Foo*> CreateFoo() {
///       auto bar = TRY(CreateBar());  // Propagates error if CreateBar() fails
///       return new Foo(bar);
///   }
#define TRY(expr) \
    ({ \
        auto&& _result = (expr); \
        if (!_result) { \
            return std::unexpected(_result.error()); \
        } \
        std::move(_result).value(); \
    })

/// Try and log - propagate error with logging
/// Logs error before propagating (useful for debugging)
#define TRY_LOG(expr) \
    ({ \
        auto&& _result = (expr); \
        if (!_result) { \
            _result.error().Log(); \
            return std::unexpected(_result.error()); \
        } \
        std::move(_result).value(); \
    })

/// Convert kern_return_t to Result<void>
/// For gradual migration from kern_return_t to Result<T>
[[nodiscard]] inline Result<void> ToResult(kern_return_t kr, const char* msg,
                                            SourceLocation loc = SourceLocation()) noexcept {
    if (kr == kIOReturnSuccess) {
        return {};  // Success
    }
    return std::unexpected(Error::Make(kr, ErrorSeverity::Fatal, msg, loc));
}

/// Convert Result<T> to kern_return_t (for compatibility with legacy code)
/// Logs error if present, returns kr
template<typename T>
[[nodiscard]] kern_return_t ToKernReturn(const Result<T>& result) noexcept {
    if (result) {
        return kIOReturnSuccess;
    }
    result.error().Log();
    return result.error().kr;
}

// ============================================================================
// Error Context Builder (for complex error messages)
// ============================================================================

/// Error context builder for formatting complex error messages
/// Avoids dynamic allocation by using compile-time string composition
///
/// Usage:
///   return ASFW_ERROR_FATAL(kIOReturnNoSpace,
///                           "Ring full: head=%zu tail=%zu capacity=%zu",
///                           head, tail, capacity);
///
/// Note: This is a compile-time helper, actual formatting happens at log time
/// For now, use simple string literals in error messages
/// TODO: Add constexpr string formatting when needed

} // namespace ASFW::Async
