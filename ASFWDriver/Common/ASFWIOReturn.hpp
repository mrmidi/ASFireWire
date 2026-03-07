#pragma once

#include <DriverKit/IOReturn.h>

#include <cstdint>

namespace ASFW::FW {

enum class Ack : int8_t;
enum class Response : uint8_t;

/**
 * @brief FireWire-family status codes encoded via `sub_iokit_firewire`.
 *
 * Low values preserve the historical IOFireWireFamily layout where semantics match.
 * ASFW-specific extensions start at `0x200` to avoid collisions with Apple-defined
 * family codes.
 */
enum class FireWireStatusCode : uint16_t {
    NoEntry = 0x001,
    PendingQueue = 0x002,
    ConfigROMInvalid = 0x004,

    AckBusy = 0x200,
    AckTypeError = 0x201,
    AckDataError = 0x202,
};

/**
 * @brief FireWire response codes carried in response packets or synthesized locally.
 *
 * These values match the IEEE 1394 / IOFireWireFamily response-code layout and are
 * composed on top of the FireWire response-family base (`0x10`).
 */
enum class FireWireResponseCode : uint8_t {
    Complete = 0,
    ConflictError = 4,
    DataError = 5,
    TypeError = 6,
    AddressError = 7,
    BusReset = 16,
    Pending = 17,
};

[[nodiscard]] constexpr IOReturn MakeFireWireIOReturn(uint16_t code) noexcept {
    return static_cast<IOReturn>(iokit_family_err(sub_iokit_firewire, code));
}

[[nodiscard]] constexpr IOReturn MakeFireWireIOReturn(FireWireStatusCode code) noexcept {
    return MakeFireWireIOReturn(static_cast<uint16_t>(code));
}

inline constexpr uint16_t kFireWireResponseBaseCode = 0x10;

[[nodiscard]] constexpr IOReturn FireWireResponseBase() noexcept {
    return MakeFireWireIOReturn(kFireWireResponseBaseCode);
}

[[nodiscard]] constexpr IOReturn MakeFireWireResponseIOReturn(FireWireResponseCode code) noexcept {
    return MakeFireWireIOReturn(static_cast<uint16_t>(kFireWireResponseBaseCode) +
                                static_cast<uint8_t>(code));
}

inline constexpr IOReturn kASFWIOReturnNoEntry = MakeFireWireIOReturn(FireWireStatusCode::NoEntry);
inline constexpr IOReturn kASFWIOReturnPendingQueue =
    MakeFireWireIOReturn(FireWireStatusCode::PendingQueue);
inline constexpr IOReturn kASFWIOReturnConfigROMInvalid =
    MakeFireWireIOReturn(FireWireStatusCode::ConfigROMInvalid);

inline constexpr IOReturn kASFWIOReturnResponseBase = FireWireResponseBase();
inline constexpr IOReturn kASFWIOReturnResponseConflict =
    MakeFireWireResponseIOReturn(FireWireResponseCode::ConflictError);
inline constexpr IOReturn kASFWIOReturnResponseDataError =
    MakeFireWireResponseIOReturn(FireWireResponseCode::DataError);
inline constexpr IOReturn kASFWIOReturnResponseTypeError =
    MakeFireWireResponseIOReturn(FireWireResponseCode::TypeError);
inline constexpr IOReturn kASFWIOReturnResponseAddressError =
    MakeFireWireResponseIOReturn(FireWireResponseCode::AddressError);
inline constexpr IOReturn kASFWIOReturnBusReset =
    MakeFireWireResponseIOReturn(FireWireResponseCode::BusReset);
inline constexpr IOReturn kASFWIOReturnResponsePending =
    MakeFireWireResponseIOReturn(FireWireResponseCode::Pending);

inline constexpr IOReturn kASFWIOReturnAckBusy = MakeFireWireIOReturn(FireWireStatusCode::AckBusy);
inline constexpr IOReturn kASFWIOReturnAckTypeError =
    MakeFireWireIOReturn(FireWireStatusCode::AckTypeError);
inline constexpr IOReturn kASFWIOReturnAckDataError =
    MakeFireWireIOReturn(FireWireStatusCode::AckDataError);

[[nodiscard]] constexpr bool IsFireWireIOReturn(IOReturn status) noexcept {
    return err_get_system(status) == err_get_system(sys_iokit) &&
           err_get_sub(status) == err_get_sub(sub_iokit_firewire);
}

[[nodiscard]] constexpr bool IsFireWireResponseIOReturn(IOReturn status) noexcept {
    return status == kASFWIOReturnResponseConflict || status == kASFWIOReturnResponseDataError ||
           status == kASFWIOReturnResponseTypeError ||
           status == kASFWIOReturnResponseAddressError || status == kASFWIOReturnBusReset ||
           status == kASFWIOReturnResponsePending;
}

/**
 * @brief Map a wire-level ACK code to a boundary-facing `IOReturn`.
 *
 * Queue/internal pending is distinct from wire response pending:
 * `Ack::Pending` maps to response-pending semantics, while queued work uses
 * `kASFWIOReturnPendingQueue`.
 */
[[nodiscard]] constexpr IOReturn MapAckToIOReturn(Ack ack) noexcept {
    switch (static_cast<int8_t>(ack)) {
    case 1:
        return kIOReturnSuccess;
    case 2:
        return kASFWIOReturnResponsePending;
    case 4:
    case 5:
    case 6:
        return kASFWIOReturnAckBusy;
    case 13:
        return kASFWIOReturnAckDataError;
    case 14:
        return kASFWIOReturnAckTypeError;
    case -1:
        return kIOReturnTimeout;
    default:
        return kIOReturnError;
    }
}

/**
 * @brief Map a wire-level response code to a boundary-facing `IOReturn`.
 */
[[nodiscard]] constexpr IOReturn MapRespToIOReturn(Response response) noexcept {
    switch (static_cast<uint8_t>(response)) {
    case 0:
        return kIOReturnSuccess;
    case 4:
        return kASFWIOReturnResponseConflict;
    case 5:
        return kASFWIOReturnResponseDataError;
    case 6:
        return kASFWIOReturnResponseTypeError;
    case 7:
        return kASFWIOReturnResponseAddressError;
    case 16:
        return kASFWIOReturnBusReset;
    case 17:
        return kASFWIOReturnResponsePending;
    default:
        return kIOReturnError;
    }
}

static_assert(kASFWIOReturnNoEntry ==
                  static_cast<IOReturn>(iokit_family_err(sub_iokit_firewire, 0x1)),
              "NoEntry must preserve FireWire-family encoding");
static_assert(kASFWIOReturnPendingQueue ==
                  static_cast<IOReturn>(iokit_family_err(sub_iokit_firewire, 0x2)),
              "PendingQueue must preserve FireWire-family encoding");
static_assert(kASFWIOReturnConfigROMInvalid ==
                  static_cast<IOReturn>(iokit_family_err(sub_iokit_firewire, 0x4)),
              "ConfigROMInvalid must preserve FireWire-family encoding");
static_assert(kASFWIOReturnResponseBase ==
                  static_cast<IOReturn>(iokit_family_err(sub_iokit_firewire, 0x10)),
              "Response base must preserve FireWire-family encoding");
static_assert(kASFWIOReturnBusReset ==
                  static_cast<IOReturn>(iokit_family_err(sub_iokit_firewire, 0x20)),
              "BusReset must preserve FireWire-family encoding");
static_assert(kASFWIOReturnResponsePending ==
                  static_cast<IOReturn>(iokit_family_err(sub_iokit_firewire, 0x21)),
              "ResponsePending must preserve FireWire-family encoding");
static_assert(kASFWIOReturnAckBusy ==
                  static_cast<IOReturn>(iokit_family_err(sub_iokit_firewire, 0x200)),
              "ASFW-specific FireWire codes must start in the reserved 0x200 block");

} // namespace ASFW::FW
