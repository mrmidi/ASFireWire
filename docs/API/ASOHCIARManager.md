# ASOHCIARManager Class

The `ASOHCIARManager` class is responsible for managing the Asynchronous Receive (AR) contexts and rings. It handles the reception of asynchronous packets from the FireWire bus.

## Class Definition

```cpp
class ASOHCIARManager
```

## Public Methods

### `Initialize()`

Initializes the `ASOHCIARManager`.

**Parameters:**

*   `pci`: A pointer to the `IOPCIDevice` object.
*   `barIndex`: The BAR index.
*   `bufferCount`: The number of buffers to allocate.
*   `bufferBytes`: The size of each buffer in bytes.
*   `fillMode`: The buffer fill mode.
*   `filterOpts`: The filter options.

**Returns:** `kern_return_t`

### `Start()`

Starts the `ASOHCIARManager`.

**Returns:** `kern_return_t`

### `Stop()`

Stops the `ASOHCIARManager`.

**Returns:** `kern_return_t`

### `SetPacketCallback()`

Sets the packet callback function.

**Parameters:**

*   `cb`: A pointer to the callback function.
*   `refcon`: A reference constant to be passed to the callback function.

### `OnRequestPacketIRQ()`

Handles a request packet interrupt.

### `OnResponsePacketIRQ()`

Handles a response packet interrupt.

### `OnRequestBufferIRQ()`

Handles a request buffer interrupt.

### `OnResponseBufferIRQ()`

Handles a response buffer interrupt.

### `DequeueRequest()`

Dequeues a request packet.

**Parameters:**

*   `outView`: A pointer to an `ARPacketView` to store the packet.
*   `outIndex`: A pointer to a `uint32_t` to store the index of the packet.

**Returns:** `true` if a packet was dequeued, `false` otherwise.

### `DequeueResponse()`

Dequeues a response packet.

**Parameters:**

*   `outView`: A pointer to an `ARPacketView` to store the packet.
*   `outIndex`: A pointer to a `uint32_t` to store the index of the packet.

**Returns:** `true` if a packet was dequeued, `false` otherwise.

### `RecycleRequest()`

Recycles a request packet.

**Parameters:**

*   `index`: The index of the packet to recycle.

**Returns:** `kern_return_t`

### `RecycleResponse()`

Recycles a response packet.

**Parameters:**

*   `index`: The index of the packet to recycle.

**Returns:** `kern_return_t`

## Structs and Typedefs

### `ARParsedPacket`

This struct represents a parsed asynchronous receive packet.

### `PacketCallback`

This is a function pointer type for the packet callback function.

```cpp
using PacketCallback = void (*)(void *refcon, const ARParsedPacket &pkt);
```
