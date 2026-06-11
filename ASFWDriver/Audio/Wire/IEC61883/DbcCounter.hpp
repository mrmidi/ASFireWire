#pragma once

#include <cstdint>

namespace ASFW::Protocols::Audio::IEC61883 {

class DbcCounter final {
public:
    DbcCounter() noexcept = default;

    void Reset(uint8_t initialDbc = 0) noexcept;

    [[nodiscard]] uint8_t Current() const noexcept;
    [[nodiscard]] uint8_t ValueForNextPacket() const noexcept;

    void AdvanceDataBlocks(uint8_t dataBlocks) noexcept;

private:
    uint8_t dbc_{0};
};

} // namespace ASFW::Protocols::Audio::IEC61883
