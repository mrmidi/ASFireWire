# ASOHCIPHYAccess Class

The `ASOHCIPHYAccess` class provides a mechanism for serialized access to the OHCI PhyControl register. It uses an `IORecursiveLock` to ensure that only one thread can access the register at a time.

## Class Definition

```cpp
class ASOHCIPHYAccess
```

## Public Methods

### `init()`

Initializes the `ASOHCIPHYAccess` object.

**Parameters:**

*   `owner`: A pointer to the `ASOHCI` object.
*   `pci`: A pointer to the `IOPCIDevice` object.
*   `bar0`: The BAR0 index.

**Returns:** `true` if initialization is successful, `false` otherwise.

### `acquire()`

Acquires the recursive lock.

### `release()`

Releases the recursive lock.

### `readPhyRegister()`

Reads a value from a PHY register.

**Parameters:**

*   `reg`: The register to read from (0-31).
*   `value`: A pointer to a `uint8_t` to store the value.

**Returns:** `kern_return_t`

### `writePhyRegister()`

Writes a value to a PHY register.

**Parameters:**

*   `reg`: The register to write to.
*   `value`: The value to write.

**Returns:** `kern_return_t`

### `updatePhyRegisterWithMask()`

Updates a PHY register with a mask.

**Parameters:**

*   `reg`: The register to update.
*   `value`: The value to write.
*   `mask`: The mask to apply.

**Returns:** `kern_return_t`

## Private Methods

### `waitForWriteComplete()`

Waits for a write operation to complete.

**Parameters:**

*   `timeoutIterations`: The number of iterations to wait before timing out.

**Returns:** `true` if the write completes successfully, `false` otherwise.

### `waitForReadComplete()`

Waits for a read operation to complete.

**Parameters:**

*   `timeoutIterations`: The number of iterations to wait before timing out.

**Returns:** `true` if the read completes successfully, `false` otherwise.
