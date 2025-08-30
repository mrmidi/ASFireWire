# ASOHCI API Documentation

This documentation provides an overview of the ASOHCI driver's main classes and their APIs.

## Core Classes

*   [ASOHCI](./ASOHCI.md): The main driver class.
*   [ASOHCIPHYAccess](./ASOHCIPHYAccess.md): Provides serialized access to the OHCI PhyControl register.

## DMA Managers

*   [ASOHCIARManager](./ASOHCIARManager.md): Asynchronous Receive Manager.
*   [ASOHCIATManager](./ASOHCIATManager.md): Asynchronous Transmit Manager.
*   [ASOHCIIRManager](./ASOHCIIRManager.md): Isochronous Receive Manager.
*   [ASOHCIITManager](./ASOHCIITManager.md): Isochronous Transmit Manager.

## Bus Management

*   [SelfIDManager](./SelfIDManager.md): Manages the Self-ID process.
*   [ConfigROMManager](./ConfigROMManager.md): Manages the local node's Configuration ROM.
*   [Topology](./Topology.md): Represents the in-memory model of the IEEE-1394 bus topology.

## Shared Components

*   [ASOHCIContextBase](./ASOHCIContextBase.md): Base class for OHCI contexts.
*   [ASOHCIInterruptRouter](./ASOHCIInterruptRouter.md): Routes interrupts for AT & AR contexts.
*   [OHCIListOps](./OHCIListOps.md): Helper functions for OHCI list operations.