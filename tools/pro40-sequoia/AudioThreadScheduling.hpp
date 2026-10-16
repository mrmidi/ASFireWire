// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pthread.h>
#include <pthread/qos.h>
namespace Pro40Audio {
// For dedicated audio-owner threads only, before their loop starts. They exit
// after joining/closing their work; never call from a borrowed dispatch thread
// or HAL callback. SDK pthread/qos.h:119-187 specifies request/readback and
// rejection of threads opted out of QoS. This is NOT a realtime reservation or
// a deadline guarantee; retain native lead, expiry and underrun guards.
struct AudioThreadScheduling {
    qos_class_t before=QOS_CLASS_UNSPECIFIED,after=QOS_CLASS_UNSPECIFIED;
    int beforeRelative=0,afterRelative=0,readBefore=-1,set=-1,readAfter=-1;
    [[nodiscard]] bool accepted()const noexcept {
        return !readBefore && !set && !readAfter && after==QOS_CLASS_USER_INTERACTIVE && !afterRelative;
    }
    static AudioThreadScheduling request()noexcept {
        AudioThreadScheduling result;
        result.readBefore=pthread_get_qos_class_np(pthread_self(),&result.before,&result.beforeRelative);
        if(result.readBefore)return result;
        result.set=pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
        if(!result.set)result.readAfter=pthread_get_qos_class_np(pthread_self(),&result.after,&result.afterRelative);
        return result;
    }
};
}
