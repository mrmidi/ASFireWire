#include <gtest/gtest.h>
#include <functional>
#include <vector>

// Simplified test of the Apple completion pattern (fNumROMReads-- → finishedBusScan())
// This tests the PATTERN without needing full ROMScanner dependencies

namespace {

using CompletionCallback = std::function<void(int generation)>;

// Minimal scanner simulator that demonstrates the bug and fix
class SimplifiedScanner {
public:
    explicit SimplifiedScanner(CompletionCallback callback = nullptr)
        : onComplete_(callback), activeCount_(0), currentGen_(0) {}

    void Begin(int gen, int nodeCount) {
        currentGen_ = gen;
        activeCount_ = nodeCount;
    }

    // BEFORE FIX: This would NOT call onComplete_
    void OnNodeComplete_BuggyVersion() {
        activeCount_--;
        // BUG: Missing completion check here!
        // In real code, this was in OnRootDirComplete/OnBIBComplete
    }

    // AFTER FIX: This DOES call onComplete_ (Apple pattern)
    void OnNodeComplete_FixedVersion() {
        activeCount_--;
        CheckAndNotifyCompletion();  // Apple pattern: immediate check
    }

    bool IsIdle() const {
        return activeCount_ == 0 && currentGen_ != 0;
    }

private:
    void CheckAndNotifyCompletion() {
        if (activeCount_ == 0 && currentGen_ != 0 && onComplete_) {
            onComplete_(currentGen_);
        }
    }

    CompletionCallback onComplete_;
    int activeCount_;
    int currentGen_;
};

} // anonymous namespace

// ============================================================================
// Test demonstrates the bug that was fixed
// ============================================================================

TEST(CompletionPattern, BuggyVersion_DoesNotInvokeCallback) {
    // Demonstrates the BUG - callback never invoked
    bool called = false;
    SimplifiedScanner scanner([&](int gen) { called = true; });

    scanner.Begin(42, 2);  // Scan 2 nodes
    scanner.OnNodeComplete_BuggyVersion();  // Node 1 done
    scanner.OnNodeComplete_BuggyVersion();  // Node 2 done

    // BUG: Callback was NOT invoked even though scan is complete!
    EXPECT_FALSE(called) << "Buggy version does not invoke callback";
    EXPECT_TRUE(scanner.IsIdle()) << "Scanner is idle but callback was not invoked";
}

TEST(CompletionPattern, FixedVersion_InvokesCallbackImmediately) {
    // Demonstrates the FIX - callback invoked immediately (Apple pattern)
    bool called = false;
    int completedGen = 0;

    SimplifiedScanner scanner([&](int gen) {
        called = true;
        completedGen = gen;
    });

    scanner.Begin(42, 2);  // Scan 2 nodes
    scanner.OnNodeComplete_FixedVersion();  // Node 1 done
    EXPECT_FALSE(called) << "Callback should not fire until all nodes complete";

    scanner.OnNodeComplete_FixedVersion();  // Node 2 done

    // FIX: Callback IS invoked immediately when last node completes
    EXPECT_TRUE(called) << "Fixed version invokes callback immediately (Apple pattern)";
    EXPECT_EQ(completedGen, 42);
    EXPECT_TRUE(scanner.IsIdle());
}

TEST(CompletionPattern, AppleStyle_DecrementAndCheck) {
    // This is exactly what Apple does in readDeviceROM():
    //
    // fNumROMReads--;
    // if(fNumROMReads == 0) {
    //     finishedBusScan();
    // }

    int fNumROMReads = 3;  // 3 nodes to scan
    std::vector<int> callbackInvocations;

    auto finishedBusScan = [&]() {
        callbackInvocations.push_back(fNumROMReads);
    };

    // Read node 1
    fNumROMReads--;
    if (fNumROMReads == 0) finishedBusScan();
    EXPECT_EQ(callbackInvocations.size(), 0);

    // Read node 2
    fNumROMReads--;
    if (fNumROMReads == 0) finishedBusScan();
    EXPECT_EQ(callbackInvocations.size(), 0);

    // Read node 3 - should trigger callback
    fNumROMReads--;
    if (fNumROMReads == 0) finishedBusScan();

    EXPECT_EQ(callbackInvocations.size(), 1) << "Apple pattern: callback after last ROM";
    EXPECT_EQ(callbackInvocations[0], 0);
}

// ============================================================================
// Documentation Test - Explains the fix
// ============================================================================

TEST(CompletionPattern, DocumentationOfFix) {
    // This test documents what was fixed:
    //
    // BEFORE (BUG):
    //   OnRootDirComplete() {
    //       completedROMs_.push_back(...);
    //       AdvanceFSM();
    //       // MISSING: No completion check!
    //   }
    //   Result: Manual reads complete silently, ROMs stuck in completedROMs_
    //
    // AFTER (FIX):
    //   OnRootDirComplete() {
    //       completedROMs_.push_back(...);
    //       AdvanceFSM();
    //       CheckAndNotifyCompletion();  // ← Apple pattern added
    //   }
    //   Result: Callback fires immediately → OnDiscoveryScanComplete() → ConfigROMStore

    SUCCEED() << "Fix adds CheckAndNotifyCompletion() to match Apple's fNumROMReads-- pattern";
}
