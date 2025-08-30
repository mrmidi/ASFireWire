# SelfIDManager Class

The `SelfIDManager` class is responsible for managing the Self-ID process. It owns the Self-ID DMA buffer, handles OHCI programming, and decodes the Self-ID data.

## Class Definition

```cpp
class SelfIDManager
```

## Public Methods

### `Initialize()`

Initializes the `SelfIDManager` and its DMA resources.

**Parameters:**

*   `pci`: A pointer to the `IOPCIDevice` object.
*   `barIndex`: The BAR index.
*   `bufferBytes`: The size of the Self-ID buffer in bytes.

**Returns:** `kern_return_t`

### `Teardown()`

Frees mappings and DMA resources, and scrubs controller programming.

### `Arm()`

Arms the Self-ID reception.

**Parameters:**

*   `clearCount`: If `true`, zeros the hardware counter before arming.

**Returns:** `kern_return_t`

### `OnSelfIDComplete()`

Handles the "Self-ID Complete" interrupt.

**Parameters:**

*   `selfIDCountRegValue`: The value of the `SelfIDCount` register.

### `SetCallbacks()`

Sets the decode and stable callbacks.

**Parameters:**

*   `onDecode`: A `DecodeCallback` function.
*   `onStable`: A `StableCallback` function.

### `IsArmed()`

Checks if the Self-ID reception is armed.

**Returns:** `true` if armed, `false` otherwise.

### `InProgress()`

Checks if the Self-ID process is in progress.

**Returns:** `true` if in progress, `false` otherwise.

### `LastGeneration()`

Returns the last Self-ID generation number.

**Returns:** `uint32_t`

### `BufferIOVA()`

Returns the IOVA of the Self-ID buffer.

**Returns:** `uint64_t`

### `BufferBytes()`

Returns the size of the Self-ID buffer in bytes.

**Returns:** `uint32_t`

## Callbacks

### `DecodeCallback`

A callback function that is called for every Self-ID decode.

```cpp
using DecodeCallback = std::function<void(const SelfID::Result &)>;
```

### `StableCallback`

A callback function that is called when the Self-ID generation is stable.

```cpp
using StableCallback = std::function<void(const SelfID::Result &)>;
```
