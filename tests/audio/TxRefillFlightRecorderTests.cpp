// SPDX-License-Identifier: Apache-2.0
#include "Isoch/Transmit/TxRefillFlightRecorder.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <vector>
using namespace ASFW::Isoch::Tx;
TEST(TxRefillFlightRecorder, HistoryIsUnreadableUntilFrozenAndIncludesTrigger) {
    TxRefillFlightRecorder recorder;
    for (uint64_t i=0; i<100; ++i) {
        recorder.Record(TxRefillRecord{.epoch=1, .eventTicks=i}, false);
        EXPECT_FALSE(recorder.ExportOnce([](auto,auto,const auto&){ ADD_FAILURE(); }));
    }
    recorder.Record(TxRefillRecord{.epoch=1, .eventTicks=100, .failure=7}, true);
    std::vector<uint64_t> values;
    ASSERT_TRUE(recorder.ExportOnce([&](auto i,auto n,const auto& r){
        EXPECT_EQ(n,64U); values.push_back(r.eventTicks);
        if(i==63) EXPECT_EQ(r.failure,7U);
    }));
    ASSERT_EQ(values.size(),64U);EXPECT_EQ(values.front(),37U);EXPECT_EQ(values.back(),100U);
    EXPECT_FALSE(recorder.ExportOnce([](auto,auto,const auto&){ ADD_FAILURE(); }));
}
TEST(TxRefillFlightRecorder, RestartAndSecondFaultCannotOverwriteFirstFault) {
    TxRefillFlightRecorder recorder;
    recorder.Record(TxRefillRecord{.epoch=1,.eventTicks=74,.failure=7},true);
    for(unsigned i=0;i<100;++i) recorder.Record(TxRefillRecord{.epoch=2,.eventTicks=i},i==99);
    ASSERT_TRUE(recorder.ExportOnce([](auto i,auto n,const auto& r){
        EXPECT_EQ(i,0U);EXPECT_EQ(n,1U);EXPECT_EQ(r.epoch,1U);EXPECT_EQ(r.eventTicks,74U);EXPECT_EQ(r.failure,7U);
    }));
}
TEST(TxRefillFlightRecorder, AcquireHandoffPublishesOneCoherentImmutableHistory) {
    for (unsigned attempt=0;attempt<100;++attempt) {
        TxRefillFlightRecorder recorder;
        std::thread producer([&]{
            for(uint64_t i=1;i<=1000;++i)
                recorder.Record(TxRefillRecord{.epoch=i,.eventTicks=i,.mappedBefore=i},i==1000);
        });
        bool exported=false;
        while(!exported) {
            exported=recorder.ExportOnce([](auto i,auto n,const auto& r){
                EXPECT_EQ(n,64U);EXPECT_EQ(r.epoch,937U+i);
                EXPECT_EQ(r.eventTicks,r.epoch);EXPECT_EQ(r.mappedBefore,r.epoch);
            });
            if(!exported) std::this_thread::yield();
        }
        producer.join();
    }
}
