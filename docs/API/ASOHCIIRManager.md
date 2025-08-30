# ASOHCIIRManager Class

The `ASOHCIIRManager` class owns multiple Isochronous Receive (IR) contexts, shares a descriptor pool, and handles interrupt fan-out.

## Class Definition

```cpp
class ASOHCIIRManager
```

## Public Methods

### `Initialize()`

Discovers available IR contexts, initializes the shared pool, and applies default policies.

**Parameters:**

*   `pci`: A pointer to the `IOPCIDevice` object.
*   `barIndex`: The BAR index.
*   `defaultPolicy`: The default IR policy.

**Returns:** `kern_return_t`

### `StartAll()`

Starts all IR contexts.

**Returns:** `kern_return_t`

### `StopAll()`

Stops all IR contexts.

**Returns:** `kern_return_t`

### `StartReception()`

Configures and starts reception on a specific IR context.

**Parameters:**

*   `ctxId`: The ID of the context to start.
*   `channelFilter`: The channel filter to apply.
*   `queueOpts`: The queue options.
*   `completionCallback`: A pointer to the completion callback function.
*   `callbackContext`: A context pointer to be passed to the callback function.

**Returns:** `kern_return_t`

### `StopReception()`

Stops reception on a specific context.

**Parameters:**

*   `ctxId`: The ID of the context to stop.

**Returns:** `kern_return_t`

### `EnqueueReceiveBuffers()`

Enqueues receive buffers for standard modes (buffer-fill, packet-per-buffer).

**Parameters:**

*   `ctxId`: The ID of the context.
*   `bufferVAs`: An array of buffer virtual addresses.
*   `bufferPAs`: An array of buffer physical addresses.
*   `bufferSizes`: An array of buffer sizes.
*   `bufferCount`: The number of buffers.
*   `opts`: The queue options.

**Returns:** `kern_return_t`

### `EnqueueDualBufferReceive()`

Enqueues a dual-buffer receive operation.

**Parameters:**

*   `ctxId`: The ID of the context.
*   `dualBufferInfo`: The dual-buffer information.
*   `opts`: The queue options.

**Returns:** `kern_return_t`

### `OnInterrupt_RxEventMask()`

Handles the receive event interrupt.

**Parameters:**

*   `mask`: The interrupt event mask.

### `OnInterrupt_BusReset()`

Handles a bus reset.

### `RefillContext()`

Refills a context with buffers.

**Parameters:**

*   `ctxId`: The ID of the context to refill.

**Returns:** `kern_return_t`

### `ContextNeedsRefill()`

Checks if a context needs to be refilled.

**Parameters:**

*   `ctxId`: The ID of the context.

**Returns:** `true` if the context needs to be refilled, `false` otherwise.

### `GetContextStats()`

Gets the statistics for a context.

**Parameters:**

*   `ctxId`: The ID of the context.

**Returns:** A const reference to the `IRStats` struct.

### `ResetContextStats()`

Resets the statistics for a context.

**Parameters:**

*   `ctxId`: The ID of the context.

### `NumContexts()`

Returns the number of available IR contexts.

**Returns:** `uint32_t`

### `IsContextValid()`

Checks if a context ID is valid.

**Parameters:**

*   `ctxId`: The ID of the context.

**Returns:** `true` if the context ID is valid, `false` otherwise.
