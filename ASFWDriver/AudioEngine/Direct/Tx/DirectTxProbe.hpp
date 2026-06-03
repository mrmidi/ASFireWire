#pragma once

#include "../DirectOutputReader.hpp"
#include "DirectTxTypes.hpp"

namespace ASFW::AudioEngine::Direct::Tx {

class DirectTxProbe final {
public:
    explicit DirectTxProbe(DirectOutputReader& reader) noexcept
        : reader_(reader) {}

    [[nodiscard]] DirectTxReadResult Probe(const DirectTxReadRequest& request) noexcept;

private:
    DirectOutputReader& reader_;
};

} // namespace ASFW::AudioEngine::Direct::Tx
