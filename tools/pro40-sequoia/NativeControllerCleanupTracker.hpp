// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeServiceAttemptResult.hpp"
namespace Pro40Audio {
enum class ControllerSenderObligation { none, required };
enum class ControllerReceiptObligation { none, required };
// Single owner, one attempt. Declare obligations before acquisition. This tracks
// trusted owner observations only: it performs no close/wait, owns no handles and
// cannot prove callers' assertions. Resource owners remain responsible for cleanup.
class NativeControllerCleanupTracker {
    const ControllerSenderObligation sender_;
    const ControllerReceiptObligation receipt_;
    bool receiptClosed_=false,senderReaped_=false,senderEndpointsClosed_=false;
public:
    explicit NativeControllerCleanupTracker(ControllerSenderObligation sender,
        ControllerReceiptObligation receipt=ControllerReceiptObligation::required)noexcept:sender_(sender),receipt_(receipt){}
    NativeControllerCleanupTracker(const NativeControllerCleanupTracker &)=delete;
    NativeControllerCleanupTracker &operator=(const NativeControllerCleanupTracker &)=delete;
    [[nodiscard]] bool confirmReceiptClosed()noexcept{
        if(receipt_!=ControllerReceiptObligation::required||receiptClosed_)return false;
        receiptClosed_=true;return true;
    }
    [[nodiscard]] bool confirmSenderReaped()noexcept{
        if(sender_!=ControllerSenderObligation::required||senderReaped_)return false;
        senderReaped_=true;return true;
    }
    [[nodiscard]] bool confirmSenderEndpointsClosed()noexcept{
        if(sender_!=ControllerSenderObligation::required||senderEndpointsClosed_)return false;
        senderEndpointsClosed_=true;return true;
    }
    ServiceControllerCleanup snapshot()const noexcept{
        const bool noSender=sender_==ControllerSenderObligation::none;
        return {(receipt_==ControllerReceiptObligation::none||receiptClosed_)&&(noSender||senderEndpointsClosed_)?LocalCleanup::closed:LocalCleanup::incomplete,
                noSender?RemoteCleanup::notRequired:senderReaped_?RemoteCleanup::confirmed:RemoteCleanup::unconfirmed};
    }
};
}
