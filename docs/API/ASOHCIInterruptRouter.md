# ASOHCIInterruptRouter Class

The `ASOHCIInterruptRouter` class is a thin, shared interrupt fan-out for Asynchronous Transmit (AT) and Asynchronous Receive (AR) contexts. It is responsible for dispatching interrupts to the appropriate context.

## Class Definition

```cpp
class ASOHCIInterruptRouter : public OSObject
```

## Public Methods

### `SetATRequest()`

Sets the AT request context.

**Parameters:**

*   `c`: A pointer to the `ASOHCIContextBase` for the AT request context.

### `SetATResponse()`

Sets the AT response context.

**Parameters:**

*   `c`: A pointer to the `ASOHCIContextBase` for the AT response context.

### `SetARRequest()`

Sets the AR request context.

**Parameters:**

*   `c`: A pointer to the `ASOHCIContextBase` for the AR request context.

### `SetARResponse()`

Sets the AR response context.

**Parameters:**

*   `c`: A pointer to the `ASOHCIContextBase` for the AR response context.

### `OnAT_Request_TxComplete()`

Handles the AT request transmit complete interrupt.

### `OnAT_Response_TxComplete()`

Handles the AT response transmit complete interrupt.

### `OnAR_Request_PacketArrived()`

Handles the AR request packet arrived interrupt.

### `OnAR_Response_PacketArrived()`

Handles the AR response packet arrived interrupt.

### `OnAR_Request_BufferComplete()`

Handles the AR request buffer complete interrupt.

### `OnAR_Response_BufferComplete()`

Handles the AR response buffer complete interrupt.
