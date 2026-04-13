#pragma once

namespace ASFW::Driver {
class ControllerCore;
}

namespace ASFW::UserClient {

/**
 * @brief Recover the controller core pointer from the DriverKit-local driver bridge.
 *
 * The cast stays localized here so UserClient handlers do not duplicate opaque
 * bridge logic.
 */
template <typename DriverLike>
[[nodiscard]] inline ASFW::Driver::ControllerCore*
GetControllerCorePtr(DriverLike* driver) noexcept {
    if (driver == nullptr) {
        return nullptr;
    }
    return static_cast<ASFW::Driver::ControllerCore*>(driver->GetControllerCore());
}

} // namespace ASFW::UserClient
