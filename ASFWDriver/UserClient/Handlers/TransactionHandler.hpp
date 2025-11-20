//
//  TransactionHandler.hpp
//  ASFWDriver
//
//  Handler for async transaction related UserClient methods
//

#ifndef ASFW_USERCLIENT_TRANSACTION_HANDLER_HPP
#define ASFW_USERCLIENT_TRANSACTION_HANDLER_HPP

#include <DriverKit/IOUserClient.h>
#include "../../Async/AsyncTypes.hpp"

// Forward declarations
class ASFWDriver;
class ASFWDriverUserClient;

namespace ASFW::UserClient {

class TransactionStorage;

class TransactionHandler {
public:
    TransactionHandler(ASFWDriver* driver, TransactionStorage* storage);
    ~TransactionHandler() = default;

    // Disable copy/move
    TransactionHandler(const TransactionHandler&) = delete;
    TransactionHandler& operator=(const TransactionHandler&) = delete;

    // Method 8: Initiate async read transaction
    kern_return_t AsyncRead(IOUserClientMethodArguments* args,
                           ASFWDriverUserClient* userClient);

    // Method 9: Initiate async write transaction
    kern_return_t AsyncWrite(IOUserClientMethodArguments* args,
                            ASFWDriverUserClient* userClient);

    // Method 12: Retrieve completed transaction result
    kern_return_t GetTransactionResult(IOUserClientMethodArguments* args);

    // Method 13: Register async callback for transaction completion
    kern_return_t RegisterTransactionListener(IOUserClientMethodArguments* args,
                                              ASFWDriverUserClient* userClient);

    // Method 17: Initiate async compare-and-swap (lock) transaction
    kern_return_t AsyncCompareSwap(IOUserClientMethodArguments* args,
                                   ASFWDriverUserClient* userClient);

private:
    ASFWDriver* driver_;
    TransactionStorage* storage_;

    // Static completion callback for async transactions
    static void AsyncCompletionCallback(
        ASFW::Async::AsyncHandle handle,
        ASFW::Async::AsyncStatus status,
        void* context,
        const void* responsePayload,
        uint32_t responseLength);
};

} // namespace ASFW::UserClient

#endif // ASFW_USERCLIENT_TRANSACTION_HANDLER_HPP
