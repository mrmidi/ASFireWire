/**
 * CallbackInvocationTest.cpp - Unit test for callback invocation mechanism
 *
 * PURPOSE: Verify that callbacks are properly stored and invoked through the
 *          Transaction -> InvokeResponseHandler -> Wrapper Lambda flow.
 *
 * This test isolates the callback mechanism from DriverKit dependencies.
 */

#include <functional>
#include <cstdint>
#include <iostream>
#include <cassert>

// Simple span replacement for C++17
template<typename T>
struct span {
    T* data_;
    size_t size_;

    span(T* d, size_t s) : data_(d), size_(s) {}

    T* data() const { return data_; }
    size_t size() const { return size_; }
};

// Minimal type definitions (avoiding DriverKit)
using kern_return_t = int;
constexpr kern_return_t kIOReturnSuccess = 0;

// Mock AsyncHandle and AsyncStatus
struct AsyncHandle {
    uint32_t value;
};

enum class AsyncStatus : uint8_t {
    kSuccess = 0,
    kTimeout = 1,
    kHardwareError = 2
};

// CompletionCallback type (matches AsyncTypes.hpp)
using CompletionCallback = std::function<void(AsyncHandle handle,
                                              AsyncStatus status,
                                              span<const uint8_t> responsePayload)>;

/**
 * Simplified Transaction class for testing callback invocation
 */
class MockTransaction {
public:
    MockTransaction(uint8_t label) : label_(label) {}

    void SetResponseHandler(std::function<void(kern_return_t, span<const uint8_t>)> handler) {
        responseHandler_ = std::move(handler);
    }

    void InvokeResponseHandler(kern_return_t kr, span<const uint8_t> data) {
        std::cout << "🔍 [InvokeResponseHandler] label=" << (int)label_
                  << " responseHandler_=" << (responseHandler_ ? "VALID" : "NULL")
                  << " kr=0x" << std::hex << kr << std::dec << std::endl;

        if (responseHandler_) {
            std::cout << "🔍 [InvokeResponseHandler] Invoking responseHandler_" << std::endl;
            responseHandler_(kr, data);
            std::cout << "🔍 [InvokeResponseHandler] responseHandler_ returned" << std::endl;
        } else {
            std::cout << "⚠️ [InvokeResponseHandler] responseHandler_ is NULL!" << std::endl;
        }
    }

    uint8_t label() const { return label_; }

private:
    uint8_t label_;
    std::function<void(kern_return_t, span<const uint8_t>)> responseHandler_;
};

/**
 * Test 1: Direct callback invocation (baseline)
 */
void Test1_DirectCallback() {
    std::cout << "\n=== Test 1: Direct Callback ===" << std::endl;

    bool callbackInvoked = false;
    uint32_t receivedHandle = 0;
    AsyncStatus receivedStatus = AsyncStatus::kTimeout;

    // Create a callback that sets flags
    CompletionCallback userCallback = [&](AsyncHandle h, AsyncStatus s, span<const uint8_t> data) {
        std::cout << "📥 [User Callback] INVOKED: handle=" << h.value
                  << " status=" << (int)s << " dataLen=" << data.size() << std::endl;
        callbackInvoked = true;
        receivedHandle = h.value;
        receivedStatus = s;
    };

    // Invoke directly
    uint8_t testData[] = {0x04, 0x20, 0x8F, 0xE2};
    userCallback(AsyncHandle{1}, AsyncStatus::kSuccess, span<const uint8_t>(testData, 4));

    // Verify
    assert(callbackInvoked && "Callback should have been invoked");
    assert(receivedHandle == 1 && "Handle should be 1");
    assert(receivedStatus == AsyncStatus::kSuccess && "Status should be success");

    std::cout << "✅ Test 1 PASSED" << std::endl;
}

/**
 * Test 2: Callback through wrapper lambda (simulates Tracking::RegisterTx)
 */
void Test2_WrapperLambda() {
    std::cout << "\n=== Test 2: Wrapper Lambda ===" << std::endl;

    bool callbackInvoked = false;
    uint32_t receivedHandle = 0;

    // User's completion callback
    CompletionCallback userCallback = [&](AsyncHandle h, AsyncStatus s, span<const uint8_t> data) {
        std::cout << "📥 [User Callback] INVOKED: handle=" << h.value << std::endl;
        callbackInvoked = true;
        receivedHandle = h.value;
    };

    // Simulate TxMetadata
    struct {
        CompletionCallback callback;
    } meta;
    meta.callback = userCallback;

    std::cout << "🔍 [Test2] meta.callback valid=" << (meta.callback ? "YES" : "NO") << std::endl;

    // Simulate Transaction setup (Tracking::RegisterTx wrapper lambda)
    MockTransaction txn(0);
    uint8_t label = 0;

    // This is the EXACT pattern from Tracking.hpp lines 138-156
    txn.SetResponseHandler([callback = meta.callback, label]
                           (kern_return_t kr, span<const uint8_t> data) {
        std::cout << "🔍 [Wrapper Lambda] ENTRY: label=" << (int)label
                  << " callback=" << (callback ? "VALID" : "NULL")
                  << " kr=0x" << std::hex << kr << std::dec << std::endl;

        if (callback) {
            AsyncStatus status = (kr == kIOReturnSuccess) ? AsyncStatus::kSuccess : AsyncStatus::kHardwareError;
            std::cout << "🔍 [Wrapper Lambda] About to invoke callback: handle=" << (label + 1) << std::endl;
            callback(AsyncHandle{static_cast<uint32_t>(label) + 1}, status, data);
            std::cout << "🔍 [Wrapper Lambda] Callback returned" << std::endl;
        } else {
            std::cout << "⚠️ [Wrapper Lambda] callback is NULL!" << std::endl;
        }
    });

    // Simulate transaction completion
    uint8_t testData[] = {0x04, 0x20, 0x8F, 0xE2};
    txn.InvokeResponseHandler(kIOReturnSuccess, span<const uint8_t>(testData, 4));

    // Verify
    assert(callbackInvoked && "Callback should have been invoked through wrapper");
    assert(receivedHandle == 1 && "Handle should be 1 (label 0 + 1)");

    std::cout << "✅ Test 2 PASSED" << std::endl;
}

/**
 * Test 3: Copy semantics - does std::function copy correctly?
 */
void Test3_CopySemantics() {
    std::cout << "\n=== Test 3: Copy Semantics ===" << std::endl;

    bool callbackInvoked = false;

    // Original callback
    CompletionCallback original = [&](AsyncHandle h, AsyncStatus s, span<const uint8_t> data) {
        std::cout << "📥 [User Callback] INVOKED via copy" << std::endl;
        callbackInvoked = true;
    };

    // Copy via assignment (simulates meta.callback = callback_)
    CompletionCallback copy;
    copy = original;

    std::cout << "🔍 [Test3] original valid=" << (original ? "YES" : "NO") << std::endl;
    std::cout << "🔍 [Test3] copy valid=" << (copy ? "YES" : "NO") << std::endl;

    // Invoke copy
    uint8_t testData[] = {0x04};
    copy(AsyncHandle{1}, AsyncStatus::kSuccess, span<const uint8_t>(testData, 1));

    assert(callbackInvoked && "Copy should work");
    std::cout << "✅ Test 3 PASSED" << std::endl;
}

/**
 * Test 4: Move semantics - does std::function move correctly?
 */
void Test4_MoveSemantics() {
    std::cout << "\n=== Test 4: Move Semantics ===" << std::endl;

    bool callbackInvoked = false;

    // Original callback
    CompletionCallback original = [&](AsyncHandle h, AsyncStatus s, span<const uint8_t> data) {
        std::cout << "📥 [User Callback] INVOKED via move" << std::endl;
        callbackInvoked = true;
    };

    std::cout << "🔍 [Test4] original valid (before move)=" << (original ? "YES" : "NO") << std::endl;

    // Move via assignment (simulates AsyncCommand(std::move(callback)))
    CompletionCallback moved = std::move(original);

    std::cout << "🔍 [Test4] original valid (after move)=" << (original ? "YES" : "NO") << std::endl;
    std::cout << "🔍 [Test4] moved valid=" << (moved ? "YES" : "NO") << std::endl;

    // Invoke moved
    uint8_t testData[] = {0x04};
    moved(AsyncHandle{1}, AsyncStatus::kSuccess, span<const uint8_t>(testData, 1));

    assert(callbackInvoked && "Moved callback should work");
    std::cout << "✅ Test 4 PASSED" << std::endl;
}

/**
 * Test 5: Lambda capture by value - does it preserve the callback?
 */
void Test5_LambdaCapture() {
    std::cout << "\n=== Test 5: Lambda Capture ===" << std::endl;

    bool callbackInvoked = false;

    CompletionCallback userCallback = [&](AsyncHandle h, AsyncStatus s, span<const uint8_t> data) {
        std::cout << "📥 [User Callback] INVOKED via lambda capture" << std::endl;
        callbackInvoked = true;
    };

    // Simulate the lambda capture from Tracking.hpp
    // CRITICAL: Does [callback = userCallback] properly copy the std::function?
    auto wrapperLambda = [callback = userCallback]() {
        std::cout << "🔍 [Lambda] callback valid=" << (callback ? "YES" : "NO") << std::endl;
        if (callback) {
            uint8_t testData[] = {0x04};
            callback(AsyncHandle{1}, AsyncStatus::kSuccess, span<const uint8_t>(testData, 1));
        }
    };

    wrapperLambda();

    assert(callbackInvoked && "Lambda capture should preserve callback");
    std::cout << "✅ Test 5 PASSED" << std::endl;
}

/**
 * Test 6: Full flow simulation (AsyncCommand → Tracking → Transaction → Callback)
 */
void Test6_FullFlow() {
    std::cout << "\n=== Test 6: Full Flow Simulation ===" << std::endl;

    bool callbackInvoked = false;

    // 1. User creates callback (ROMReader lambda)
    CompletionCallback userCallback = [&](AsyncHandle h, AsyncStatus s, span<const uint8_t> data) {
        std::cout << "📥 [ROMReader Callback] INVOKED: handle=" << h.value << std::endl;
        callbackInvoked = true;
    };

    // 2. AsyncCommand stores callback
    struct {
        CompletionCallback callback_;
    } asyncCommand;
    asyncCommand.callback_ = userCallback;  // Line 29 of AsyncCommandImpl.hpp

    std::cout << "🔍 [AsyncCommand] callback_ valid=" << (asyncCommand.callback_ ? "YES" : "NO") << std::endl;

    // 3. TxMetadata receives callback
    struct {
        CompletionCallback callback;
    } meta;
    meta.callback = asyncCommand.callback_;  // Line 29 of AsyncCommandImpl.hpp

    std::cout << "🔍 [TxMetadata] callback valid=" << (meta.callback ? "YES" : "NO") << std::endl;

    // 4. Tracking::RegisterTx wraps callback in lambda
    MockTransaction txn(0);
    uint8_t label = 0;

    txn.SetResponseHandler([callback = meta.callback, label]
                           (kern_return_t kr, span<const uint8_t> data) {
        std::cout << "🔍 [Wrapper Lambda] callback valid=" << (callback ? "YES" : "NO") << std::endl;
        if (callback) {
            callback(AsyncHandle{label + 1}, AsyncStatus::kSuccess, data);
        }
    });

    // 5. TransactionCompletionHandler::CompleteTransaction_ invokes callback
    uint8_t testData[] = {0x04, 0x20, 0x8F, 0xE2};
    txn.InvokeResponseHandler(kIOReturnSuccess, span<const uint8_t>(testData, 4));

    assert(callbackInvoked && "Full flow should invoke callback");
    std::cout << "✅ Test 6 PASSED" << std::endl;
}

int main() {
    std::cout << "================================" << std::endl;
    std::cout << "Callback Invocation Test Suite" << std::endl;
    std::cout << "================================" << std::endl;

    try {
        Test1_DirectCallback();
        Test2_WrapperLambda();
        Test3_CopySemantics();
        Test4_MoveSemantics();
        Test5_LambdaCapture();
        Test6_FullFlow();

        std::cout << "\n🎉 ALL TESTS PASSED! 🎉" << std::endl;
        std::cout << "\nConclusion: Callback mechanism is WORKING correctly." << std::endl;
        std::cout << "The issue must be elsewhere in the code path." << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    }
}
