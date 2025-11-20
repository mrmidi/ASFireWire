# IEEE-1394 Tree Identification State Machine  
(Modernized Technical Documentation)

This document presents a clear and developer-oriented rewrite of the IEEE‑1394‑1995 §16.4.6 Tree Identification state machine.  
It improves readability while keeping full fidelity to the original technical behavior.

---

## 1. Overview

After the bus reset process completes (R1 → T0), every FireWire node enters the **Tree Identification** procedure.  
The goal is to determine:

- which node becomes the **root**,  
- which ports are **child ports**,  
- which ports are **parent ports**,  
- and the final loop‑free spanning tree used in Self‑ID.

The state machine consists of four core states:

- **T0 — Tree ID Start**  
- **T1 — Child Handshake**  
- **T2 — Parent Handshake**  
- **T3 — Root Contention**  

The successful exit point is **S0 — Self-ID Start**.

---

## 2. Mermaid.js State Diagram

```mermaid
stateDiagram-v2

    [*] --> T0: Tree ID Start
(tree_ID_startActions)

    T0 --> T1: (!forceRoot || arbTimer >= FORCE_ROOT_TIMEOUT) &&
(children == NPORT - 1) || (children == NPORT)
    T0 --> T0: !T0_timeout && (arbTimer == configTimeout)
T0_timeout = true
    T0 --> S0: Parent_Handshake_Complete

    T1 --> T2: childHandshakeComplete()

    T2 --> S0: PARENT_HANDSHAKE received
    T2 --> T3: !root && portRArb[parentPort] == ROOT_CONTENTION
    T2 --> T2: root || portRArb[parentPort] == PARENT_HANDSHAKE

    T3 --> T2: arbTimer > contendTime && portRArb[parentPort] == IDLE
    T3 --> T1: arbTimer > contendTime && portRArb[parentPort] == PARENT_NOTIFY
```

---

## 3. State Definitions and Transitions

---

## T0: Tree ID Start

The node has just exited the Reset Wait state.  
It waits up to **CONFIG_TIMEOUT** to receive **PARENT_NOTIFY** on all but at most one of its active ports.

Rules:

- Any port sending **PARENT_NOTIFY** is marked as a **child**.  
- If a loop exists in the topology, a **configuration timeout** triggers.

### Transition T0 → T0 (Timeout)

If the configuration period expires:

- `T0_timeout = TRUE`
- Any Beta‑mode ports return to **P11: Untested**
- May allow a bus reset to restart or continue a loop‑free build process

### Transition T0 → T1 (Handshake Start)

If the node receives **PARENT_NOTIFY** on:

- all ports, or  
- all but one port  

then:

- Node knows it is either the **root** or a **branch node**
- Node begins the **handshake process** with its children

Leaf nodes (one connected port) immediately take this transition.  
If `forceRoot` is active, this transition may be intentionally delayed until **FORCE_ROOT_TIMEOUT**.

---

## T1: Child Handshake

All ports labeled as child ports transmit the **CHILD_NOTIFY** signal.

State purpose:

- Notify children that the parent is ready  
- Children receiving CHILD_NOTIFY exit T2 → S0 immediately  
- If all ports are child ports, the node knows it is **root**

### Transition T1 → T2

Occurs when:

- All children stop sending **PARENT_NOTIFY**, and
- Node receives **CHILD_HANDSHAKE** on all child ports

Node then moves to handshake with its own parent.

---

## T2: Parent Handshake

Node waits for **PARENT_HANDSHAKE** from its parent.

Cases:

- A root node receives **PARENT_NOTIFY** on all ports  
  → it bypasses this state  
- If receiving **ROOT_CONTENTION**, exit to T3

### Transition T2 → S0

Triggered when:

- Node receives **PARENT_HANDSHAKE** from its parent  
- Node sends **IDLE** and enters the Self‑ID Start state

A root node also takes this transition immediately (no parent exists).

### Transition T2 → T3

Triggered when:

- A node receives **PARENT_NOTIFY** on the same port where it is sending **PARENT_NOTIFY**  
- Merged signals interpreted as **ROOT_CONTENTION**

### Transition T2 → T2 (Stay)

Occurs when:

- Node is root, or  
- Parent port already shows **PARENT_HANDSHAKE**

---

## T3: Root Contention

Occurs when two nodes simultaneously attempt to become root.

Mechanism:

1. Both nodes back off by sending **IDLE**
2. Both start a random timer
3. If the random bit is 1, the node waits longer; if 0, it waits shorter
4. On timer expiration, node samples the contention port

### Transition T3 → T2

If:

- `arbTimer > contendTime` and  
- parent port receives **IDLE**

Node returns to parent‑handshake and proceeds.

### Transition T3 → T1

If:

- `arbTimer > contendTime` and  
- parent port receives **PARENT_NOTIFY**

Node enters T1 and becomes the root.

---

## S0: Self-ID Start

Final exit from tree identification.

Triggered by:

- Receiving **PARENT_HANDSHAKE**, or  
- Root node with no parent, or  
- Leaf nodes completing CHILD_HANDSHAKE

Node begins transmitting **Self‑ID packets**.

---

# End of Document
