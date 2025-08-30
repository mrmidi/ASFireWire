# Topology Class

The `Topology` class provides an in-memory model of the IEEE-1394 bus topology. It is responsible for accumulating Self-ID records, building the parent/child relationships between nodes, finding the root node, and performing basic consistency checks.

## Class Definition

```cpp
class Topology
```

## Structs

### `Node`

Represents a node on the FireWire bus.

**Fields:**

*   `nodeId`: The node ID.
*   `phy`: The PHY ID.
*   `isRoot`: `true` if the node is the root.
*   `linkActive`: `true` if the link is active.
*   `gapCount`: The gap count.
*   `speed`: The link speed.
*   `contender`: `true` if the node is a contender.
*   `powerClass`: The power class.
*   `initiated`: `true` if the node has been initiated.
*   `ports`: An array of `PortState` enums.
*   `parents`: A vector of parent node IDs.
*   `children`: A vector of child node IDs.
*   `rom`: A pointer to the `ConfigROMProperties` for the node.

### `BuildInfo`

Contains information about the topology build process.

**Fields:**

*   `generation`: The Self-ID generation for this snapshot.
*   `rootPhy`: The discovered root PHY ID.
*   `integrityOk`: `true` if the topology is integral.
*   `warnings`: A vector of warning messages.

## Public Methods

### `BeginCycle()`

Starts a new cycle of topology accumulation.

**Parameters:**

*   `generation`: The Self-ID generation.

### `AddOrUpdateNode()`

Adds or updates a node in the topology.

**Parameters:**

*   `rec`: A `SelfID::AlphaRecord`.

### `Finalize()`

Finishes building the topology by assigning node IDs, deriving edges and roles, finding the root, and performing sanity checks.

### `Clear()`

Clears the topology.

### `Info()`

Returns a const reference to the `BuildInfo` struct.

**Returns:** `const BuildInfo &`

### `NodeCount()`

Returns the number of nodes in the topology.

**Returns:** `size_t`

### `Root()`

Returns a pointer to the root node.

**Returns:** `const Node *`

### `FindByPhy()`

Finds a node by its PHY ID.

**Parameters:**

*   `phy`: The PHY ID to find.

**Returns:** `const Node *`

### `FindByNodeId()`

Finds a node by its node ID.

**Parameters:**

*   `id`: The node ID to find.

**Returns:** `const Node *`

### `ForEachNode()`

Iterates over the nodes in the topology.

**Parameters:**

*   `fn`: A function to call for each node.

### `HasCycles()`

Checks if the topology has any cycles.

**Returns:** `true` if the topology has cycles, `false` otherwise.

### `MaxHopsFromRoot()`

Returns the maximum number of hops from the root node.

**Returns:** `uint8_t`

### `IsConsistent()`

Checks if the topology is consistent.

**Returns:** `true` if the topology is consistent, `false` otherwise.

### `AttachROM()`

Attaches Config ROM data to a known node.

**Parameters:**

*   `phy`: The PHY ID of the node.
*   `props`: A pointer to the `ConfigROMProperties`.

**Returns:** `true` if the ROM was attached successfully, `false` otherwise.

### `Log()`

Logs the current topology in a readable format.
