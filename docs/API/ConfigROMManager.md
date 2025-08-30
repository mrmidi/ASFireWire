# ConfigROMManager Class

The `ConfigROMManager` class centralizes the local node's Configuration ROM build, mapping, and commit process.

## Class Definition

```cpp
class ConfigROMManager
```

## Public Methods

### `Initialize()`

Initializes the `ConfigROMManager`, allocates and maps the ROM buffer, builds the ROM image, DMA-maps it, and programs the ROM map register.

**Parameters:**

*   `pci`: A pointer to the `IOPCIDevice` object.
*   `barIndex`: The BAR index.
*   `busOptions`: The bus options.
*   `guidHi`: The high 32 bits of the GUID.
*   `guidLo`: The low 32 bits of the GUID.
*   `romBytes`: The size of the ROM in bytes (usually 1024).

**Returns:** `kern_return_t`

### `Teardown()`

Frees the map, DMA, and buffer, and scrubs the ROM map register.

### `CommitOnBusReset()`

Commits the staged BusOptions and Header on a bus reset.

### `Dump()`

Dumps the ROM content to the log for diagnostic purposes.

**Parameters:**

*   `label`: A label to print with the dump.

### `EUI64()`

Returns the EUI-64 of the node.

**Returns:** `uint64_t`

### `VendorId()`

Returns the vendor ID of the node.

**Returns:** `uint32_t`

### `BusOptions()`

Returns the bus options.

**Returns:** `uint32_t`

### `HeaderStaged()`

Checks if the header is staged for commit.

**Returns:** `true` if the header is staged, `false` otherwise.

### `HeaderQuad()`

Returns the header quadlet.

**Returns:** `uint32_t`

### `ROMIOVA()`

Returns the IOVA of the ROM.

**Returns:** `uint64_t`
