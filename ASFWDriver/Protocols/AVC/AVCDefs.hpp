//
// AVCDefs.hpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C (Audio/Video Control) protocol definitions
// Based on AV/C Digital Interface Command Set General Specification
// IEC 61883-1 for PCR/CMP integration
//

#pragma once

#include <stdint.h>
#include <cstddef>

namespace ASFW::Protocols::AVC {

//==============================================================================
// FCP (Function Control Protocol) CSR Addresses
//==============================================================================

/// FCP Command address (target receives commands here)
constexpr uint64_t kFCPCommandAddress = 0xFFFFF0000B00ULL;

/// FCP Response address (initiator receives responses here)
constexpr uint64_t kFCPResponseAddress = 0xFFFFF0000D00ULL;

/// Legacy Apple FCP base (non-standard, some devices use this)
constexpr uint64_t kFCPLegacyBase = 0xFFFFF0001000ULL;

//==============================================================================
// PCR (Plug Control Register) CSR Addresses (IEC 61883-1)
//==============================================================================

/// PCR base address
constexpr uint64_t kPCRBaseAddress = 0xFFFFF0000900ULL;

/// Output Master Plug Register
constexpr uint64_t kPCR_oMPR = kPCRBaseAddress + 0x00;

/// Input Master Plug Register
constexpr uint64_t kPCR_iMPR = kPCRBaseAddress + 0x04;

/// Output Plug Control Register array (0-30)
constexpr uint64_t kPCR_oPCRBase = kPCRBaseAddress + 0x08;

/// Input Plug Control Register array (0-30)
constexpr uint64_t kPCR_iPCRBase = kPCRBaseAddress + 0x80;

/// Get oPCR address for plug number
inline constexpr uint64_t GetOPCRAddress(uint8_t plugNum) {
    return kPCR_oPCRBase + (plugNum * 4);
}

/// Get iPCR address for plug number
inline constexpr uint64_t GetIPCRAddress(uint8_t plugNum) {
    return kPCR_iPCRBase + (plugNum * 4);
}

//==============================================================================
// AV/C Command Types (ctype field in byte[0] of CDB)
//==============================================================================

/// AV/C command types (request direction)
enum class AVCCommandType : uint8_t {
    kControl = 0x00,    ///< Perform action
    kStatus = 0x01,     ///< Query state
    kInquiry = 0x02,    ///< Query capability
    kNotify = 0x03,     ///< Subscribe to events
};

/// AV/C response types (response direction)
enum class AVCResponseType : uint8_t {
    kNotImplemented = 0x08,         ///< Command not supported
    kAccepted = 0x09,               ///< CONTROL succeeded
    kRejected = 0x0A,               ///< Command rejected
    kInTransition = 0x0B,           ///< State is changing
    kImplementedStable = 0x0C,      ///< STATUS succeeded, state stable
    kChanged = 0x0D,                ///< NOTIFY response
    kReserved = 0x0E,               ///< Reserved
    kInterim = 0x0F,                ///< Acknowledged, final coming
};

//==============================================================================
// AV/C Result Codes
//==============================================================================

/// AV/C command result (includes transport errors)
enum class AVCResult : uint8_t {
    // Success responses
    kAccepted = 0,              ///< CONTROL succeeded (0x09)
    kImplementedStable,         ///< STATUS succeeded, state stable (0x0C)
    kChanged,                   ///< NOTIFY response (0x0D)

    // Partial/transitional
    kInTransition,              ///< State changing, retry later (0x0B)
    kInterim,                   ///< Acknowledged, waiting for final (0x0F)

    // Errors
    kNotImplemented,            ///< Command not supported (0x08)
    kRejected,                  ///< Command rejected (0x0A)
    kInvalidResponse,           ///< Invalid/malformed response

    // Transport errors
    kTimeout,                   ///< FCP timeout
    kBusReset,                  ///< Bus reset during command
    kTransportError,            ///< FCP transport error
    kBusy,                      ///< Command already pending
};

/// Check if result indicates success
inline bool IsSuccess(AVCResult result) {
    return result == AVCResult::kAccepted ||
           result == AVCResult::kImplementedStable ||
           result == AVCResult::kChanged;
}

/// Check if result indicates retry might succeed
inline bool ShouldRetry(AVCResult result) {
    return result == AVCResult::kInTransition ||
           result == AVCResult::kBusReset;
}

/// Convert AV/C ctype to AVCResult
inline AVCResult CTypeToResult(uint8_t ctype) {
    switch (ctype) {
        case 0x09: return AVCResult::kAccepted;
        case 0x0C: return AVCResult::kImplementedStable;
        case 0x0D: return AVCResult::kChanged;
        case 0x0B: return AVCResult::kInTransition;
        case 0x0F: return AVCResult::kInterim;
        case 0x08: return AVCResult::kNotImplemented;
        case 0x0A: return AVCResult::kRejected;
        default:   return AVCResult::kInvalidResponse;
    }
}

//==============================================================================
// AV/C Opcodes
//==============================================================================

/// Common AV/C command opcodes
enum class AVCOpcode : uint8_t {
    kPlugInfo = 0x02,               ///< Query plug count
    kUnitInfo = 0x30,               ///< Unit info
    kConnect = 0x24,                ///< Connect plugs
    kDisconnect = 0x25,             ///< Disconnect plugs
    kConnections = 0x26,            ///< Query connections
    kChannelUsage = 0x1F,           ///< Query channel allocation
    kSubunitInfo = 0x31,            ///< Enumerate subunits
    kOutputPlugSignalFormat = 0xBF, ///< Query/set output format
    kInputPlugSignalFormat = 0xFF,  ///< Query/set input format
};

//==============================================================================
// AV/C Subunit Types
//==============================================================================

/// AV/C subunit types (bits 7-3 of subunit address byte)
enum class AVCSubunitType : uint8_t {
    kVideoMonitor = 0x00,       ///< Display device
    kAudio = 0x01,              ///< Audio processing
    kTapeRecorder = 0x04,       ///< DV camcorder
    kTuner = 0x05,              ///< TV tuner
    kCA = 0x06,                 ///< Conditional access
    kCamera = 0x07,             ///< Digital camera
    kPanel = 0x0A,              ///< Control panel
    kBulletinBoard = 0x0B,      ///< Info display
    kMusic0C = 0x0C,            ///< Music subunit (devices sometimes report 0x0C)
    kMusic = 0x1C,              ///< Audio interface (pro audio)
    kUnit = 0x1F,               ///< Whole unit (not a subunit)
};

/// Special subunit address: whole unit
constexpr uint8_t kAVCSubunitUnit = 0xFF;

/// Build subunit address byte
inline constexpr uint8_t MakeSubunitAddress(AVCSubunitType type, uint8_t id) {
    return (static_cast<uint8_t>(type) << 3) | (id & 0x07);
}

//==============================================================================
// 1394 Trade Association Spec IDs
//==============================================================================

/// 1394 Trade Association spec ID (24-bit)
constexpr uint32_t kSpecID_1394TA = 0x00A02D;

/// AV/C minimum version
constexpr uint32_t kAVCVersionMin = 0x010001;

//==============================================================================
// FCP/AV/C Frame Constraints
//==============================================================================

/// Minimum AV/C frame size (ctype + subunit + opcode)
constexpr size_t kAVCFrameMinSize = 3;

/// Maximum AV/C frame size
constexpr size_t kAVCFrameMaxSize = 512;

/// Maximum operand length
constexpr size_t kAVCOperandMaxLength = kAVCFrameMaxSize - kAVCFrameMinSize;

//==============================================================================
// FCP Timeouts
//==============================================================================

/// Initial FCP timeout (milliseconds)
constexpr uint32_t kFCPTimeoutInitial = 1000;

/// FCP timeout after interim response (milliseconds)
constexpr uint32_t kFCPTimeoutAfterInterim = 10000;

/// Maximum FCP retry attempts
constexpr uint8_t kFCPMaxRetries = 4;

//==============================================================================
// Plug Types (for PCR/CMP)
//==============================================================================

/// Plug type (input or output)
enum class PlugType : uint8_t {
    kInput = 0,     ///< Input (destination) plug
    kOutput = 1,    ///< Output (source) plug
};

//==============================================================================
// PCR Bit Masks (IEC 61883-1)
//==============================================================================

/// oPCR/iPCR bit masks
namespace PCRMask {
    constexpr uint32_t kOnline            = 0x80000000;  ///< bit 31
    constexpr uint32_t kBroadcastCount    = 0x3F000000;  ///< bits 24-29
    constexpr uint32_t kP2PCount          = 0x00FF0000;  ///< bits 16-23
    constexpr uint32_t kChannel           = 0x0000FC00;  ///< bits 10-15
    constexpr uint32_t kDataRate          = 0x000000C0;  ///< bits 6-7
    constexpr uint32_t kOverhead          = 0x0000003F;  ///< bits 0-5
}

/// PCR field shifts
namespace PCRShift {
    constexpr int kOnline         = 31;
    constexpr int kBroadcastCount = 24;
    constexpr int kP2PCount       = 16;
    constexpr int kChannel        = 10;
    constexpr int kDataRate       = 6;
    constexpr int kOverhead       = 0;
}

//==============================================================================
// Speed Codes
//==============================================================================

/// IEEE 1394 speed codes
enum class SpeedCode : uint8_t {
    kS100 = 0,      ///< 100 Mbps
    kS200 = 1,      ///< 200 Mbps
    kS400 = 2,      ///< 400 Mbps
    kS800 = 3,      ///< 800 Mbps (1394b)
};

} // namespace ASFW::Protocols::AVC
