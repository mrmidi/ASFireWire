#pragma once

#include <cstdint>

namespace ASFW::Driver {

// Canonical OHCI register offsets (subset) expressed as strongly typed enums so
// call sites avoid sprinkling magic numbers. Values are taken from OHCI 1.1
// Table 5-1 and related chapters.
enum class Register32 : uint32_t {
    kVersion = 0x000,
    kGUIDROM = 0x004,
    kATRetries = 0x008,
    kCSRData = 0x00C,
    kCSRCompareData = 0x010,
    kCSRControl = 0x014,
    kConfigROMHeader = 0x018,
    kBusID = 0x01C,
    kBusOptions = 0x020,
    kGUIDHi = 0x024,
    kGUIDLo = 0x028,
    kConfigROMMap = 0x034,
    kPostedWriteAddressLo = 0x038,
    kPostedWriteAddressHi = 0x03C,
    kVendorId = 0x040,
    kHCControlSet = 0x050,  // Write-only: set bits (OHCI §5.3)
    kHCControlClear = 0x054,  // Write-only: clear bits
    kHCControl = 0x050,  // Read view: both 0x050/0x054 return latched value
    kSelfIDBuffer = 0x064,
    kSelfIDCount = 0x068,
    kIRMultiChanMaskHiSet = 0x070,
    kIRMultiChanMaskHiClear = 0x074,
    kIRMultiChanMaskLoSet = 0x078,
    kIRMultiChanMaskLoClear = 0x07C,
    kIntEvent = 0x080,  // Read-only: current interrupt event status
    kIntEventSet = 0x080,
    kIntEventClear = 0x084,
    kIntMaskSet = 0x088,
    kIntMaskClear = 0x08C,
    kIsoXmitEvent = 0x090,  // Read-only: current isochronous transmit interrupt event status
    kIsoXmitIntEventSet = 0x090,
    kIsoXmitIntEventClear = 0x094,
    kIsoXmitIntMaskSet = 0x098,
    kIsoXmitIntMaskClear = 0x09C,
    kIsoRecvEvent = 0x0A0,  // Read-only: current isochronous receive interrupt event status
    kIsoRecvIntEventSet = 0x0A0,
    kIsoRecvIntEventClear = 0x0A4,
    kIsoRecvIntMaskSet = 0x0A8,
    kIsoRecvIntMaskClear = 0x0AC,
    kInitialBandwidthAvailable = 0x0B0,
    kInitialChannelsAvailableHi = 0x0B4,
    kInitialChannelsAvailableLo = 0x0B8,
    kFairnessControl = 0x0DC,
    kLinkControlSet = 0x0E0,  // Write-only: set bits (OHCI §5.14)
    kLinkControlClear = 0x0E4,  // Write-only: clear bits
    kLinkControl = 0x0E0,  // Read view: returns current LinkControl state
    kNodeID = 0x0E8,
    kPhyControl = 0x0EC,
    kCycleTimer = 0x0F0,
    kAsReqFilterHiSet = 0x100,
    kAsReqFilterHiClear = 0x104,
    kAsReqFilterLoSet = 0x108,
    kAsReqFilterLoClear = 0x10C,
    kPhyReqFilterHiSet = 0x110,
    kPhyReqFilterHiClear = 0x114,
    kPhyReqFilterLoSet = 0x118,
    kPhyReqFilterLoClear = 0x11C,
    kPhyUpperBound = 0x120
};

struct HCControlBits {
    static constexpr uint32_t kSoftReset = 1u << 16;
    static constexpr uint32_t kLinkEnable = 1u << 17;
    static constexpr uint32_t kPostedWriteEnable = 1u << 18;
    static constexpr uint32_t kLPS = 1u << 19;
    static constexpr uint32_t kCycleMatchEnable = 1u << 20;
    static constexpr uint32_t kAPhyEnhanceEnable = 1u << 22;  // OHCI §5.7.2: Enable IEEE1394a enhancements in Link
    static constexpr uint32_t kProgramPhyEnable = 1u << 23;
    static constexpr uint32_t kNoByteSwap = 1u << 30;
    static constexpr uint32_t kBibImageValid = 1u << 31;
};

/// \brief LinkControl register bit definitions (OHCI 1.1 §5.10, Table 5-17).
///
/// This register is accessed through two write-only strobes and one read view:
/// - `LinkControlSet`  (0x0E0): writing 1s **sets** the corresponding bits
/// - `LinkControlClear`(0x0E4): writing 1s **clears** the corresponding bits
/// - `LinkControl`     (0x0E0): **reads** return the current latched value  
///   (spec: “on read, both addresses return the contents of the control register”)
///
/// Access semantics (from table column “rscu”):
/// - r  = readable via the read view of LinkControl
/// - s  = set via LinkControlSet
/// - c  = clear via LinkControlClear
/// - u  = undefined on (soft) reset unless noted; some fields have hard-reset behavior
///
/// \note Before setting \ref kRcvSelfID you MUST program a valid DMA address
///       into \ref Register32::kSelfIDBuffer (spec warning).
/// \note `cycleMaster` and `cycleSource` interact with cycle start packet generation;
///       software should leave `cycleMaster` = 0 while not root or when
///       \ref IntEventBits::kCycleTooLong is set (spec).
struct LinkControlBits {
  /// \brief Accept Self-ID packets into AR contexts.
  ///
  /// **Access:** rsc (readable, settable via Set, clearable via Clear)  
  /// **Reset:** undefined  
  /// **Spec text (summary):** “When one, the receiver will accept incoming
  /// self-identification packets. Before setting this bit to one, software shall
  /// ensure that the Self-ID buffer pointer register contains a valid address.”
  static constexpr uint32_t kRcvSelfID = 1u << 9;

  /// \brief Accept PHY packets into the AR Request context.
  ///
  /// **Access:** rsc; **Reset:** undefined  
  /// Controls receipt of self-identification packets that occur **outside** the
  /// Self-ID phase, and of PHY packets generally, provided the AR Request
  /// context is enabled. (Spec clarifies it does not control receipt of
  /// Self-ID packets during the Self-ID phase.)
  static constexpr uint32_t kRcvPhyPkt = 1u << 10;

  /// \brief Enable the link’s cycle timer offset accumulation.
  ///
  /// **Access:** rsc; **Reset:** undefined  
  /// When 1, the cycle timer offset counts at 49.152 MHz / 2; when 0, it does not.
  static constexpr uint32_t kCycleTimerEnable = 1u << 20;

  /// \brief Request cycle master behavior when the node is root.
  ///
  /// **Access:** rscu; **Reset:** undefined  
  /// When 1 **and** the PHY has notified the OpenHCI that we are root, the
  /// controller generates a cycle-start packet on each wrap; otherwise it accepts
  /// received cycle starts for synchronization. This bit shall be 0 while
  /// \ref IntEventBits::kCycleTooLong is set.
  static constexpr uint32_t kCycleMaster = 1u << 21;

  // Optional fields
  // static constexpr uint32_t kCycleSource = 1u << <bit>;      ///< rsc(u=*): external cycle source; soft reset no effect.
  // static constexpr uint32_t kTag1SyncFilterLock = 1u << <b>; ///< rs: HW clears on hard reset; soft reset has no effect.
};

struct IntEventBits {
    static constexpr uint32_t kReqTxComplete = 1u << 0;
    static constexpr uint32_t kRespTxComplete = 1u << 1;
    static constexpr uint32_t kARRQ = 1u << 2; // Asynchronous Receive Request DMA interrupt. This bit is conditionally set upon
    // completion of an AR DMA Request context command descriptor.
    static constexpr uint32_t kARRS = 1u << 3;
    static constexpr uint32_t kRQPkt = 1u << 4;
    static constexpr uint32_t kRSPkt = 1u << 5;
    static constexpr uint32_t kIsochTx = 1u << 6;
    static constexpr uint32_t kIsochRx = 1u << 7;
    static constexpr uint32_t kPostedWriteErr = 1u << 8;
    static constexpr uint32_t kLockRespErr = 1u << 9;
    static constexpr uint32_t kSelfIDComplete2 = 1u << 15;
    static constexpr uint32_t kSelfIDComplete = 1u << 16;
    static constexpr uint32_t kBusReset = 1u << 17;
    static constexpr uint32_t kRegAccessFail = 1u << 18;
    static constexpr uint32_t kPhy = 1u << 19;
    static constexpr uint32_t kCycleSynch = 1u << 20;
    static constexpr uint32_t kCycle64Seconds = 1u << 21;
    static constexpr uint32_t kCycleLost = 1u << 22;
    static constexpr uint32_t kCycleInconsistent = 1u << 23;
    static constexpr uint32_t kUnrecoverableError = 1u << 24;
    static constexpr uint32_t kCycleTooLong = 1u << 25;
    static constexpr uint32_t kPhyRegRcvd = 1u << 26;  // PHY packet received
    static constexpr uint32_t kAckTardy = 1u << 27;     // Ack tardy
    // Bits 10-14, 28: reserved
    static constexpr uint32_t kSoftInterrupt = 1u << 29; // Software interrupt (via IntEventSet)
    static constexpr uint32_t kVendorSpecific = 1u << 30; // Vendor-specific event
    // Bit 31 is NOT an IntEvent bit; it belongs to IntMask (masterIntEnable)
};

// IntMask register bits (OHCI §5.7)
// IntMask has same layout as IntEvent (bits 0-30) plus bit 31 for master enable.
// Use IntMaskSet/Clear (write-only strobes) to modify; maintain software shadow for reads.
struct IntMaskBits {
    static constexpr uint32_t kMasterIntEnable = 1u << 31;  // Master interrupt enable (OHCI §5.7)
};

// Policy: Baseline interrupt mask for normal operation.
// Includes all critical events we want delivered during steady-state operation.
// Per OHCI §5.7: IntMask enables delivery of IntEvent sources to the system interrupt line.
// masterIntEnable (bit 31) must ALSO be set for any delivery to occur.
static constexpr uint32_t kBaseIntMask =
    IntEventBits::kReqTxComplete       |  // AT request complete
    IntEventBits::kRespTxComplete      |  // AT response complete
    IntEventBits::kARRQ                |  // AR request DMA complete
    IntEventBits::kARRS                |  // AR response DMA complete
    IntEventBits::kRQPkt               |  // AR request packet available
    IntEventBits::kRSPkt               |  // AR response packet available
    IntEventBits::kIsochTx             |  // Isochronous transmit
    IntEventBits::kIsochRx             |  // Isochronous receive
    IntEventBits::kPostedWriteErr      |  // Posted write error
    IntEventBits::kLockRespErr         |  // Lock response error
    IntEventBits::kSelfIDComplete      |  // Self-ID phase 1 complete (bit 16)
    IntEventBits::kSelfIDComplete2     |  // Self-ID phase 2 complete (bit 15, sticky)
    IntEventBits::kBusReset            |  // Bus reset detected (CRITICAL: must remain enabled)
    IntEventBits::kRegAccessFail       |  // Register access failure
    IntEventBits::kCycleInconsistent   |  // Cycle timer inconsistent
    IntEventBits::kUnrecoverableError  |  // Unrecoverable error
    IntEventBits::kCycleTooLong        |  // Cycle too long
    IntEventBits::kPhyRegRcvd;            // PHY register receive complete

struct SelfIDCountBits {
    static constexpr uint32_t kError = 0x80000000u;
    static constexpr uint32_t kGenerationMask = 0x00FF0000u;
    static constexpr uint32_t kGenerationShift = 16;
    static constexpr uint32_t kSizeMask = 0x000007FCu;
    static constexpr uint32_t kSizeShift = 2;
};

} // namespace ASFW::Driver

// Helper functions for variable DMA context registers
struct DMAContextHelpers {
    // Asynchronous Transmit Context (base 0x180)
    static constexpr uint32_t AsReqTrContextBase = 0x180;
    static constexpr uint32_t AsReqTrContextControlSet = 0x180;
    static constexpr uint32_t AsReqTrContextControlClear = 0x184;
    static constexpr uint32_t AsReqTrCommandPtr = 0x18C;
    
    // Asynchronous Response Transmit Context (base 0x1A0)
    static constexpr uint32_t AsRspTrContextBase = 0x1A0;
    static constexpr uint32_t AsRspTrContextControlSet = 0x1A0;
    static constexpr uint32_t AsRspTrContextControlClear = 0x1A4;
    static constexpr uint32_t AsRspTrCommandPtr = 0x1AC;
    
    // Asynchronous Request Receive Context (base 0x1C0)
    static constexpr uint32_t AsReqRcvContextBase = 0x1C0;
    static constexpr uint32_t AsReqRcvContextControlSet = 0x1C0;
    static constexpr uint32_t AsReqRcvContextControlClear = 0x1C4;
    static constexpr uint32_t AsReqRcvCommandPtr = 0x1CC;
    
    // Asynchronous Response Receive Context (base 0x1E0)
    static constexpr uint32_t AsRspRcvContextBase = 0x1E0;
    static constexpr uint32_t AsRspRcvContextControlSet = 0x1E0;
    static constexpr uint32_t AsRspRcvContextControlClear = 0x1E4;
    static constexpr uint32_t AsRspRcvCommandPtr = 0x1EC;
    
    // Isochronous Transmit Contexts (base 0x200 + 16*n)
    // OHCI layout: offset 0x00 = ContextControl (read) / ContextControlSet (write sets bits)
    //              offset 0x04 = ContextControlClear (write clears bits)
    //              offset 0x0C = CommandPtr
    static constexpr uint32_t IsoXmitContextBase(uint32_t n) { return 0x200u + 16u * n; }
    static constexpr uint32_t IsoXmitContextControl(uint32_t n) { return 0x200u + 16u * n; }  // For READS
    static constexpr uint32_t IsoXmitContextControlSet(uint32_t n) { return 0x200u + 16u * n; }  // For WRITES (set bits)
    static constexpr uint32_t IsoXmitContextControlClear(uint32_t n) { return 0x204u + 16u * n; }  // For WRITES (clear bits)
    static constexpr uint32_t IsoXmitCommandPtr(uint32_t n) { return 0x20Cu + 16u * n; }

    // Isochronous Receive Contexts (base 0x400 + 32*n)
    static constexpr uint32_t IsoRcvContextBase(uint32_t n) { return 0x400u + 32u * n; }
    static constexpr uint32_t IsoRcvContextControlSet(uint32_t n) { return 0x400u + 32u * n; }
    static constexpr uint32_t IsoRcvContextControlClear(uint32_t n) { return 0x404u + 32u * n; }
    static constexpr uint32_t IsoRcvCommandPtr(uint32_t n) { return 0x40Cu + 32u * n; }
    static constexpr uint32_t IsoRcvContextMatch(uint32_t n) { return 0x410u + 32u * n; }
    
    // IR Context Control bits
    static constexpr uint32_t kIRContextMultiChannelMode = 0x10000000u; // Bit 28
};
