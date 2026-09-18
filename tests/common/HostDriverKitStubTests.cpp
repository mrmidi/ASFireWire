// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// HostDriverKitStubTests.cpp - the host stubs' own ownership semantics.
//
// Every other test in this suite depends on these, and they are the thing that
// decides whether a retain/release imbalance in driver code shows up or passes
// silently. They were no-ops until 2026-09-18, which is why eleven tests could
// leak a gmock object and still report green. Pinned here so that cannot regress
// unnoticed.

#include <gtest/gtest.h>

#include "ASFWDriver/Testing/HostDriverKitStubs.hpp"

namespace {

// Reports its own destruction so a test can tell "released" from "destroyed".
class TrackedObject final : public OSObject {
public:
    explicit TrackedObject(bool* destroyedFlag) : destroyed_(destroyedFlag) {}
    ~TrackedObject() override {
        if (destroyed_ != nullptr) {
            *destroyed_ = true;
        }
    }

private:
    bool* destroyed_{nullptr};
};

TEST(HostDriverKitStubs, NewObjectStartsWithOneReference) {
    bool destroyed = false;
    auto* object = new TrackedObject(&destroyed);
    EXPECT_EQ(object->GetRetainCount(), 1);
    object->release();
    EXPECT_TRUE(destroyed) << "the creating reference must be the last one";
}

TEST(HostDriverKitStubs, ReleaseDestroysOnlyAtZero) {
    bool destroyed = false;
    auto* object = new TrackedObject(&destroyed);
    object->retain();
    EXPECT_EQ(object->GetRetainCount(), 2);

    object->release();
    EXPECT_FALSE(destroyed) << "destroyed while a reference was still held";
    EXPECT_EQ(object->GetRetainCount(), 1);

    object->release();
    EXPECT_TRUE(destroyed);
}

TEST(HostDriverKitStubs, SharedPtrAdoptsWithoutRetaining) {
    bool destroyed = false;
    auto* raw = new TrackedObject(&destroyed);
    {
        OSSharedPtr<TrackedObject> owner(raw, OSNoRetain);
        EXPECT_EQ(raw->GetRetainCount(), 1) << "OSNoRetain must not add a reference";
        EXPECT_EQ(owner.get(), raw);
    }
    EXPECT_TRUE(destroyed) << "the adopted reference must be released on scope exit";
}

TEST(HostDriverKitStubs, SharedPtrRetainTagAddsAReference) {
    bool destroyed = false;
    auto* raw = new TrackedObject(&destroyed);
    {
        OSSharedPtr<TrackedObject> owner(raw, OSRetain);
        EXPECT_EQ(raw->GetRetainCount(), 2);
    }
    EXPECT_FALSE(destroyed) << "the creating reference is still outstanding";
    EXPECT_EQ(raw->GetRetainCount(), 1);
    raw->release();
    EXPECT_TRUE(destroyed);
}

TEST(HostDriverKitStubs, SharedPtrCopyAndMoveTrackReferences) {
    bool destroyed = false;
    auto* raw = new TrackedObject(&destroyed);
    {
        OSSharedPtr<TrackedObject> first(raw, OSNoRetain);
        {
            OSSharedPtr<TrackedObject> second = first;  // copy retains
            EXPECT_EQ(raw->GetRetainCount(), 2);

            OSSharedPtr<TrackedObject> third = std::move(second);  // move does not
            EXPECT_EQ(raw->GetRetainCount(), 2);
            EXPECT_EQ(second.get(), nullptr);
        }
        EXPECT_EQ(raw->GetRetainCount(), 1);
        EXPECT_FALSE(destroyed);
    }
    EXPECT_TRUE(destroyed);
}

TEST(HostDriverKitStubs, DetachHandsOverTheReferenceWithoutLeaking) {
    bool destroyed = false;
    auto* raw = new TrackedObject(&destroyed);
    TrackedObject* taken = nullptr;
    {
        OSSharedPtr<TrackedObject> owner(raw, OSNoRetain);
        taken = owner.detach();
        EXPECT_EQ(owner.get(), nullptr);
    }
    // The old stub leaked a strong reference here on purpose, because release()
    // did nothing; detach() now simply transfers the one reference.
    ASSERT_EQ(taken, raw);
    EXPECT_FALSE(destroyed) << "detach() must not release";
    EXPECT_EQ(raw->GetRetainCount(), 1);
    taken->release();
    EXPECT_TRUE(destroyed);
}

TEST(HostDriverKitStubs, ResetReleasesThePreviousObject) {
    bool firstDestroyed = false;
    bool secondDestroyed = false;
    auto* first = new TrackedObject(&firstDestroyed);
    auto* second = new TrackedObject(&secondDestroyed);

    OSSharedPtr<TrackedObject> owner(first, OSNoRetain);
    owner.reset(second, OSNoRetain);
    EXPECT_TRUE(firstDestroyed) << "the replaced object must be released";
    EXPECT_FALSE(secondDestroyed);

    owner.reset();
    EXPECT_TRUE(secondDestroyed);
}

} // namespace
