# OHCIListOps Namespace

The `OHCIListOps` namespace provides small helper functions for packing and unpacking CommandPtr, Branch, and Z values used by the Asynchronous Transmit (AT) and Asynchronous Receive (AR) program builders and descriptor rings.

## Functions

### `EncodeCommandPtr()`

Encodes a DMA address and Z nibble into a CommandPtr.

**Parameters:**

*   `dmaAddress`: The DMA address (must be 16-byte aligned).
*   `zNibble`: The Z nibble.
*   `outCmdPtr`: A pointer to a `uint32_t` to store the resulting CommandPtr.

**Returns:** `true` if the address is 16-byte aligned and the Z nibble is valid, `false` otherwise.

### `ZFromCommandPtr()`

Extracts the Z nibble from a CommandPtr.

**Parameters:**

*   `cmdPtr`: The CommandPtr.

**Returns:** The Z nibble.

### `AddressFromCommandPtr()`

Extracts the address from a CommandPtr.

**Parameters:**

*   `cmdPtr`: The CommandPtr.

**Returns:** The address.

### `PackBranchAndZ()`

- Packs a branch address and Z nibble into a quadlet.

**Parameters:**

*   `branchAddress`: The branch address.
*   `zNibble`: The Z nibble.
*   `outQuadlet`: A pointer to a `uint32_t` to store the resulting quadlet.

**Returns:** `true` if the operation is successful, `false` otherwise.

### `UnpackBranchAndZ()`

Unpacks a branch address and Z nibble from a quadlet.

**Parameters:**

*   `quadlet`: The quadlet.
*   `branchAddress`: A pointer to a `uint32_t` to store the branch address.
*   `zNibble`: A pointer to a `uint8_t` to store the Z nibble.

### `SplitStatusTimestamp()`

Splits a status/timestamp quadlet into its components.

**Parameters:**

*   `word`: The quadlet.
*   `xferStatus`: A pointer to a `uint16_t` to store the transfer status.
*   `timeStamp`: A pointer to a `uint16_t` to store the timestamp.
