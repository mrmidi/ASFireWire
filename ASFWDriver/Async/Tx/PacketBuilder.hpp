#pragma once

#include <cstddef>
#include <cstdint>

#include "../AsyncTypes.hpp"

namespace ASFW::Async {

// PacketContext is defined in AsyncTypes.hpp

class PacketBuilder {
public:
    PacketBuilder() = default;
    ~PacketBuilder() = default;

    [[nodiscard]] std::size_t BuildReadQuadlet(const ReadParams& params,
                                               uint8_t label,
                                               const PacketContext& context,
                                               void* headerBuffer,
                                               std::size_t bufferSize) const;

    [[nodiscard]] std::size_t BuildReadBlock(const ReadParams& params,
                                             uint8_t label,
                                             const PacketContext& context,
                                             void* headerBuffer,
                                             std::size_t bufferSize) const;

    [[nodiscard]] std::size_t BuildWriteQuadlet(const WriteParams& params,
                                                uint8_t label,
                                                const PacketContext& context,
                                                void* headerBuffer,
                                                std::size_t bufferSize) const;

    [[nodiscard]] std::size_t BuildWriteBlock(const WriteParams& params,
                                              uint8_t label,
                                              const PacketContext& context,
                                              void* headerBuffer,
                                              std::size_t bufferSize) const;

    [[nodiscard]] std::size_t BuildLock(const LockParams& params,
                                        uint8_t label,
                                        uint16_t extendedTCode,
                                        const PacketContext& context,
                                        void* headerBuffer,
                                        std::size_t bufferSize) const;

    [[nodiscard]] std::size_t BuildPhyPacket(const PhyParams& params,
                                             void* headerBuffer,
                                             std::size_t bufferSize) const;
};

} // namespace ASFW::Async
