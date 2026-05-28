#include "UserClient/Storage/TransactionStorage.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

TEST(TransactionStorageTests, StoresCompletePayloadBeyondLegacy512ByteLimit) {
    ASFW::UserClient::TransactionStorage storage;
    ASSERT_TRUE(storage.IsValid());

    std::array<std::uint8_t, 640> payload{};
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i & 0xffU);
    }

    EXPECT_TRUE(storage.StoreResult(0x1234, 7, 0x11, payload.data(),
                                    static_cast<std::uint32_t>(payload.size())));

    storage.Lock();
    ASFW::UserClient::TransactionResult* result = storage.FindResult(0x1234);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->status, 7U);
    EXPECT_EQ(result->responseCode, 0x11U);
    ASSERT_EQ(result->dataLength, payload.size());
    ASSERT_NE(result->Data(), nullptr);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        EXPECT_EQ(result->Data()[i], payload[i]) << "byte " << i;
    }
    storage.Unlock();
}

TEST(TransactionStorageTests, PreservesStatusAndEmptyPayload) {
    ASFW::UserClient::TransactionStorage storage;
    ASSERT_TRUE(storage.IsValid());

    EXPECT_TRUE(storage.StoreResult(0x4321, 5, 0x04, nullptr, 0));

    storage.Lock();
    ASFW::UserClient::TransactionResult* result = storage.FindResult(0x4321);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->status, 5U);
    EXPECT_EQ(result->responseCode, 0x04U);
    EXPECT_EQ(result->dataLength, 0U);
    EXPECT_EQ(result->Data(), nullptr);
    storage.Unlock();
}

} // namespace
