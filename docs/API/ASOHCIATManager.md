# ASOHCIATManager Class

The `ASOHCIATManager` class is the top-level orchestrator for Asynchronous Transmit (AT) operations. It owns the descriptor pool, program builder, and both the request and response contexts. It provides a simple API to queue packets and handles reset windows.

## Class Definition

```cpp
class ASOHCIATManager
```

## Public Methods

### `Initialize()`

Initializes the `ASOHCIATManager`.

**Parameters:**

*   `pci`: A pointer to the `IOPCIDevice` object.
*   `barIndex`: The BAR index.
*   `retry`: The retry policy.
*   `fair`: The fairness policy.
*   `pipe`: The pipeline policy.

**Returns:** `kern_return_t`

### `Start()`

Starts both the request and response contexts.

**Returns:** `kern_return_t`

### `Stop()`

Stops both the request and response contexts.

**Returns:** `kern_return_t`

### `QueueRequest()`

Queues an asynchronous transmit request.

**Parameters:**

*   `header`: A pointer to the header data.
*   `headerBytes`: The size of the header in bytes.
*   `payloadPAs`: A pointer to the payload physical addresses.
*   `payloadSizes`: A pointer to the payload sizes.
*   `fragments`: The number of fragments.
*   `opts`: The queue options.

**Returns:** `kern_return_t`

### `QueueResponse()`

Queues an asynchronous transmit response.

**Parameters:**

*   `header`: A pointer to the header data.
*   `headerBytes`: The size of the header in bytes.
*   `payloadPAs`: A pointer to the payload physical addresses.
*   `payloadSizes`: A pointer to the payload sizes.
*   `fragments`: The number of fragments.
*   `opts`: The queue options.

**Returns:** `kern_return_t`

### `OnInterrupt_ReqTxComplete()`

Handles the request transmit complete interrupt.

### `OnInterrupt_RspTxComplete()`

Handles the response transmit complete interrupt.

### `OnBusResetBegin()`

Handles the beginning of a bus reset.

### `OnBusResetEnd()`

Handles the end of a bus reset.

### `OutstandingRequests()`

Returns the number of outstanding requests.

**Returns:** `uint32_t`

### `OutstandingResponses()`

Returns the number of outstanding responses.

**Returns:** `uint32_t`
