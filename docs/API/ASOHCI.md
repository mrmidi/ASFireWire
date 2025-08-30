# ASOHCI Class

The `ASOHCI` class is the main driver class for the ASFireWire OHCI controller. It is a subclass of `IOService` and is responsible for managing the FireWire interface, handling interrupts, and managing DMA contexts.

## Class Definition

```cpp
class ASOHCI : public IOService
```

## Public Methods

### `init()`

Initializes the driver.

**Returns:** `true` if initialization is successful, `false` otherwise.

### `free()`

Frees any resources allocated by the driver.

### `Start()`

Starts the driver.

**Parameters:**

*   `provider`: A pointer to the `IOService` provider.

**Returns:** `kern_return_t`

### `Stop()`

Stops the driver.

**Parameters:**

*   `provider`: A pointer to the `IOService` provider.

**Returns:** `kern_return_t`

### `InterruptOccurred()`

Handles interrupts from the FireWire controller.

**Parameters:**

*   `action`: A pointer to the `OSAction` object.
*   `count`: The interrupt count.
*   `time`: The time of the interrupt.

### `CopyBridgeLogs()`

Copies the bridge logs to an `OSData` object.

**Parameters:**

*   `outData`: A pointer to a pointer to an `OSData` object.

**Returns:** `kern_return_t`

## Private Methods

### `ArmSelfIDReceive()`

Arms the Self-ID receive buffer and enables the necessary bits.

**Parameters:**

*   `clearCount`: A boolean indicating whether to clear the count.

## Instance Variables (`ivars`)

The `ivars` struct contains the per-instance state of the driver.

*   `pciDevice`: A pointer to the `IOPCIDevice` object.
*   `bar0Map`: A pointer to the `IOMemoryMap` object for BAR0.
*   `barIndex`: The BAR index.
*   `intSource`: A pointer to the `IOInterruptDispatchSource` object.
*   `defaultQ`: A pointer to the `IODispatchQueue` object.
*   `interruptCount`: The interrupt count.
*   `selfIDBuffer`: A pointer to the `IOBufferMemoryDescriptor` for the Self-ID buffer.
*   `selfIDDMA`: A pointer to the `IODMACommand` for the Self-ID buffer.
*   `selfIDSeg`: The `IOAddressSegment` for the Self-ID buffer.
*   `selfIDMap`: A pointer to the `IOMemoryMap` for the Self-ID buffer.
*   `configROMBuffer`: A pointer to the `IOBufferMemoryDescriptor` for the Config ROM buffer.
*   `configROMMap`: A pointer to the `IOMemoryMap` for the Config ROM buffer.
*   `configROMDMA`: A pointer to the `IODMACommand` for the Config ROM buffer.
*   `configROMSeg`: The `IOAddressSegment` for the Config ROM buffer.
*   `configROMHeaderQuad`: The computed BIB header quadlet for the Config ROM.
*   `configROMBusOptions`: A mirror of `ROM[2]`.
*   `configROMHeaderNeedsCommit`: A boolean indicating whether the Config ROM header needs to be committed.
*   `cycleTimerArmed`: A boolean indicating whether the cycle timer is armed.
*   `selfIDInProgress`: A boolean indicating whether Self-ID is in progress.
*   `selfIDArmed`: A boolean indicating whether Self-ID is armed.
*   `collapsedBusResets`: The number of collapsed bus resets.
*   `lastLoggedNodeID`: The last logged node ID.
*   `lastLoggedValid`: A boolean indicating whether the last logged node ID is valid.
*   `lastLoggedRoot`: A boolean indicating whether the last logged node ID is the root.
*   `didInitialPhyScan`: A boolean indicating whether the initial PHY scan has been performed.
*   `cycleInconsistentCount`: The number of cycle inconsistent events.
*   `lastCycleInconsistentTime`: The time of the last cycle inconsistent event.
*   `phyAccess`: A pointer to the `ASOHCIPHYAccess` object.
*   `arRequestContext`: A pointer to the `ASOHCIARContext` for asynchronous receive requests.
*   `arResponseContext`: A pointer to the `ASOHCIARContext` for asynchronous receive responses.
*   `atRequestContext`: A pointer to the `ASOHCIATContext` for asynchronous transmit requests.
*   `atResponseContext`: A pointer to the `ASOHCIATContext` for asynchronous transmit responses.
*   `arManager`: A pointer to the `ASOHCIARManager` object.
*   `atManager`: A pointer to the `ASOHCIATManager` object.
*   `irManager`: A pointer to the `ASOHCIIRManager` object.
*   `itManager`: A pointer to the `ASOHCIITManager` object.
*   `selfIDManager`: A pointer to the `SelfIDManager` object.
*   `configROMManager`: A pointer to the `ConfigROMManager` object.
*   `topology`: A pointer to the `Topology` object.
