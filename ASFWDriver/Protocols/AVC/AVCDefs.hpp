//
// AVCDefs.hpp
// ASFWDriver - AV/C Protocol Layer
//
// FCP register addresses, frame limits and transaction timing. Everything
// about AV/C frames themselves (ctype, response code, subunit address,
// opcodes) lives in Core/AvcTypes.hpp; PCR registers live in CMP/CMPClient.hpp.
//

#pragma once

#include <stdint.h>
#include <cstddef>

namespace ASFW::Protocols::AVC {

//==============================================================================
// FCP (Function Control Protocol) CSR Addresses (IEC 61883-1 §8)
//==============================================================================

/// FCP Command address (target receives commands here)
constexpr uint64_t kFCPCommandAddress = 0xFFFFF0000B00ULL;

/// FCP Response address (initiator receives responses here)
constexpr uint64_t kFCPResponseAddress = 0xFFFFF0000D00ULL;

// FCP response register space is 512 bytes. Cross-validated with Linux
// sound/firewire/fcp.c:379-386 and Apple's IOFireWireAVCUnit.cpp:907-912.
constexpr uint64_t kFCPResponseAddressSpaceSize = 0x200ULL;
constexpr uint64_t kFCPResponseAddressEnd =
    kFCPResponseAddress + kFCPResponseAddressSpaceSize;

//==============================================================================
// FCP/AV/C Frame Constraints
//==============================================================================

/// Minimum AV/C frame size (ctype + subunit + opcode)
constexpr size_t kAVCFrameMinSize = 3;

/// Maximum AV/C frame size
constexpr size_t kAVCFrameMaxSize = 512;

//==============================================================================
// Response matching
//==============================================================================

/// A tape recorder/player subunit answers TRANSPORT STATE (opcode D0) with the current transport
/// mode as the response opcode: Apple says "Play, Wind, Record, or LoadMedium"
/// (IOFireWireAVCCommand.cpp:122-125, 136-140). Those are the four values C1..C4; which opcode is
/// which mode is not in any local spec (the Tape Recorder/Player subunit specification is not in
/// 1papers), so they are named as a range.
constexpr uint8_t kTapeTransportStateOpcode = 0xD0;
constexpr uint8_t kTapeTransportModeOpcodeFirst = 0xC1;
constexpr uint8_t kTapeTransportModeOpcodeLast = 0xC4;

/// Response and command opcodes are compared without bit 7. This is legacy ASFW behaviour
/// (carried over from the pre-phase-3 transport); Apple compares the whole byte
/// (IOFireWireAVCCommand.cpp:154-157) and no spec supports ignoring the bit. Source unknown.
constexpr uint8_t kResponseOpcodeCompareMask = 0x7F;

//==============================================================================
// FCP Timeouts
//==============================================================================

/// Response deadline after the command write completes (milliseconds). A target
/// answers within 100 ms (TA 2004006 AV/C General 4.2 §6.2); Apple waits 250 ms
/// (IOFireWireAVCCommand.cpp:89), Linux 125 ms (sound/firewire/fcp.c:26).
constexpr uint32_t kFCPTimeoutInitial = 250;

/// Response deadline after an INTERIM response (milliseconds). Apple's
/// kInterimTimeout (IOFireWireAVCCommand.cpp:168).
constexpr uint32_t kFCPTimeoutAfterInterim = 10000;

/// Replays of a STATUS/INQUIRY after a lost response. Apple's fMaxRetries
/// (IOFireWireAVCCommand.cpp:325).
constexpr uint8_t kFCPMaxRetries = 4;

} // namespace ASFW::Protocols::AVC
