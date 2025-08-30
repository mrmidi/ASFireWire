# ASOHCIContextBase Class

The `ASOHCIContextBase` class provides the base functionality for OHCI contexts. It handles the common context register plumbing for starting, stopping, and waking the context, as well as writing to the command pointer.

## Class Definition

```cpp
class ASOHCIContextBase
```

## Public Methods

### `Initialize()`

Initializes the context base.

**Parameters:**

*   `pci`: A pointer to the `IOPCIDevice` object.
*   `barIndex`: The BAR index.
*   `kind`: The context kind.
*   `offsets`: The context offsets.

**Returns:** `kern_return_t`

### `Start()`

Starts the context.

**Returns:** `kern_return_t`

### `Stop()`

Stops the context.

**Returns:** `kern_return_t`

### `Wake()`

Wakes the context.

**Returns:** `kern_return_t`

### `OnBusResetBegin()`

Handles the beginning of a bus reset.

### `OnBusResetEnd()`

Handles the end of a bus reset.

### `IsRunning()`

Checks if the context is running.

**Returns:** `true` if the context is running, `false` otherwise.

### `IsActive()`

Checks if the context is active.

**Returns:** `true` if the context is active, `false` otherwise.

### `ReadContextSet()`

Reads the `ContextControl` register.

**Returns:** `uint32_t`

### `WriteCommandPtr()`

Writes to the `CommandPtr` register.

**Parameters:**

*   `descriptorAddress`: The descriptor address (must be 16-byte aligned).
*   `zNibble`: The Z nibble (0-15).

**Returns:** `kern_return_t`

### `GetKind()`

Returns the context kind.

**Returns:** `ASContextKind`

### `GetOffsets()`

Returns the context offsets.

**Returns:** `const ASContextOffsets &`

### `GetBAR()`

Returns the BAR index.

**Returns:** `uint8_t`

### `GetPCIDevice()`

Returns a pointer to the `IOPCIDevice` object.

**Returns:** `IOPCIDevice *`
