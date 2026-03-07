#pragma once

#include <DriverKit/IOReturn.h>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>
#include <vector>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/OSSharedPtr.h>
#endif

namespace ASFW::Driver {

class HardwareInterface;

#ifdef ASFW_HOST_TEST
class SelfIDCaptureTestPeer;
#endif

/**
 * @class SelfIDCapture
 * @brief Owns the OHCI Self-ID DMA buffer and validates captured Self-ID data.
 *
 * The capture path intentionally treats malformed inverse pairs, generation
 * races, and broken sequence structure as typed decode failures instead of
 * silently reusing stale topology. This keeps the bus-reset FSM honest and
 * lets recovery policy make an explicit follow-up reset decision.
 */
class SelfIDCapture {
  public:
    struct Result {
        std::vector<uint32_t> quads;
        // sequences: pairs of (start_index, quadlet_count) into `quads`
        std::vector<std::pair<size_t, unsigned int>> sequences;
        uint32_t generation{0};
        bool valid{false};
        bool timedOut{false};
        bool crcError{false};
    };

    enum class DecodeErrorCode : uint8_t {
        BufferUnavailable,
        ControllerErrorBit,
        EmptyCapture,
        CountOverflow,
        NullMapAddress,
        GenerationMismatch,
        InvalidInversePair,
        MalformedSequence,
    };

    struct DecodeError {
        DecodeErrorCode code{DecodeErrorCode::BufferUnavailable};
        uint32_t countRegister{0};
        uint32_t generation{0};
        size_t quadletIndex{0};
    };

    SelfIDCapture();
    ~SelfIDCapture();

    kern_return_t PrepareBuffers(size_t quadCapacity, HardwareInterface& hw);
    void ReleaseBuffers();

    kern_return_t Arm(HardwareInterface& hw);
    void Disarm(HardwareInterface& hw);

    /**
     * Decode the captured Self-ID buffer using the current `SelfIDCount` value.
     *
     * OHCI 1.1 §11.5 sets the completion bits only after the controller updates the
     * first quadlet of the Self-ID receive buffer. The implementation still
     * double-checks the captured generation against the buffer header and a second
     * `SelfIDCount` read so back-to-back resets never publish stale topology.
     */
    [[nodiscard]] std::expected<Result, DecodeError>
    Decode(uint32_t selfIDCountReg, HardwareInterface& hw) const;

    [[nodiscard]] static const char* DecodeErrorCodeString(DecodeErrorCode code) noexcept;

  private:
#ifdef ASFW_HOST_TEST
    friend class SelfIDCaptureTestPeer;
#endif

    OSSharedPtr<IOBufferMemoryDescriptor> buffer_;
    OSSharedPtr<IODMACommand> dmaCommand_;
    OSSharedPtr<IOMemoryMap> map_;
    IOAddressSegment segment_{};
    bool segmentValid_{false};
    size_t bufferBytes_{0};
    size_t quadCapacity_{0};
    bool armed_{false};
};

} // namespace ASFW::Driver
