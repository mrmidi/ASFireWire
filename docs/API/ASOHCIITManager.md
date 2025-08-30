# ASOHCIITManager Class

The `ASOHCIITManager` class owns multiple Isochronous Transmit (IT) contexts, shares a descriptor pool, and handles interrupt fan-out.

## Class Definition

```cpp
class ASOHCIITManager
```

## Public Methods

### `Initialize()`

Discovers available IT contexts, initializes the shared pool, and applies default policies.

**Parameters:**

*   `pci`: A pointer to the `IOPCIDevice` object.
*   `barIndex`: The BAR index.
*   `defaultPolicy`: The default IT policy.

**Returns:** `kern_return_t`

### `StartAll()`

Starts all IT contexts.

**Returns:** `kern_return_t`

### `StopAll()`

Stops all IT contexts.

**Returns:** `kern_return_t`

### `Queue()`

Queues a packet into a specific IT context.

**Parameters:**

*   `ctxId`: The ID of the context.
*   `spd`: The speed.
*   `tag`: The tag.
*   `channel`: The channel.
*   `sy`: The synchronization bits.
*   `payloadPAs`: An array of payload physical addresses.
*   `payloadSizes`: An array of payload sizes.
*   `fragments`: The number of fragments.
*   `opts`: The queue options.

**Returns:** `kern_return_t`

### `OnInterrupt_TxEventMask()`

Handles the transmit event interrupt.

**Parameters:**

*   `mask`: The interrupt event mask.

### `OnInterrupt_CycleInconsistent()`

Handles the cycle inconsistent interrupt.

### `NumContexts()`

Returns the number of available IT contexts.

**Returns:** `uint32_t`
