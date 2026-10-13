// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeFeedMailbox.hpp"
namespace Pro40Audio {
// Source-thread endpoint only. Producer and mailbox outlive this object.
// Worker may access ONLY the mailbox; it never calls the producer or its owner.
// Caller keeps ticking during idle and joins worker before mailbox destruction.
// END timeout covers sender completion after the content owner has finished.
template<class Producer> class NativeProducerExchange {
public:
    enum class Result {waiting,ended,failed};
    NativeProducerExchange(Producer &producer,NativeFeedMailbox &mailbox,uint64_t epoch,uint64_t endAge)
        :producer_(producer),mailbox_(mailbox),epoch_(epoch),endAge_(endAge){if(!epoch||!endAge)fail();}
    ~NativeProducerExchange(){if(!ended_&&!failed_)fail();}
    NativeProducerExchange(const NativeProducerExchange &)=delete;
    NativeProducerExchange &operator=(const NativeProducerExchange &)=delete;
    Result tick(uint64_t now) noexcept {
        if(failed_)return Result::failed;
        if(now<lastTime_||mailbox_.cancelled())return fail();lastTime_=now;
        if(ended_)return Result::ended;
        if(endSent_&&now-endSince_>endAge_)return fail();
        NativeFeedMailbox::Message receipt;
        const auto received=mailbox_.receipt(receipt);
        if(received==NativeFeedMailbox::Receipt::failed)return fail();
        if(received==NativeFeedMailbox::Receipt::accepted){
            if(!pending_||receipt.batch.epoch!=epoch_||receipt.end!=endSent_)return fail();
            pending_=false;
            if(endSent_){
                if(producer_.pump(now)!=Producer::Result::end)return fail();
                ended_=true;return Result::ended;
            }
            if(!producer_.acknowledge(receipt.batch.epoch,receipt.batch.firstFrame,receipt.batch.frames,now))return fail();
        }
        const auto result=producer_.pump(now);
        using R=typename Producer::Result;
        if(result==R::failed)return fail();
        if(pending_)return Result::waiting;
        AudioFeedBatch batch;
        if(result==R::batch){
            if(!producer_.copy(now,batch)||batch.epoch!=epoch_||!mailbox_.submit(batch))return fail();
            pending_=true;
        }else if(result==R::end){
            batch.epoch=epoch_;batch.firstFrame=producer_.delivered();
            if(!mailbox_.submit(batch,true))return fail();
            pending_=endSent_=true;endSince_=now;
        }
        return Result::waiting;
    }
    void cancel() noexcept {fail();}
private:
    Result fail() noexcept {
        if(!failed_){failed_=true;mailbox_.cancel();producer_.cancel();}return Result::failed;
    }
    Producer &producer_;NativeFeedMailbox &mailbox_;uint64_t epoch_,endAge_,lastTime_=0,endSince_=0;
    bool pending_=false,endSent_=false,ended_=false,failed_=false;
};
// Sender-thread step; Sender owns its transport and applies bounded send times.
// No loop/wait is imposed here. A send failure revokes the mailbox; source tick
// performs owner cancellation on the source thread. Join is external/mandatory.
template<class Sender> bool sendNativeFeedOnce(NativeFeedMailbox &mailbox,Sender &sender) noexcept {
    NativeFeedMailbox::Message message;
    if(!mailbox.take(message))return !mailbox.cancelled();
    if(mailbox.cancelled())return false;
    const bool accepted=message.end?sender.finish(message.batch.epoch,message.batch.firstFrame):sender.send(message.batch);
    return mailbox.complete(message.sequence,accepted);
}
}
