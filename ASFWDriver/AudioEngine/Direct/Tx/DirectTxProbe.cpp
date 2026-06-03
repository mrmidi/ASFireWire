#include "DirectTxProbe.hpp"

#include <limits>

namespace ASFW::AudioEngine::Direct::Tx {

DirectTxReadResult DirectTxProbe::Probe(const DirectTxReadRequest& request) noexcept {
    DirectTxReadResult result{};

    if (!reader_.IsBound()) {
        result.status = DirectTxReadStatus::kInvalidBinding;
        return result;
    }

    result.writtenEndFrame = reader_.OutputWrittenEndFrame();

    if (request.frameCount == 0 ||
        request.channels == 0 ||
        request.channels != reader_.OutputChannels()) {
        result.status = DirectTxReadStatus::kInvalidRange;
        return result;
    }

    constexpr uint64_t kMaxFrame = std::numeric_limits<uint64_t>::max();
    if (request.firstFrame > (kMaxFrame - request.frameCount)) {
        result.status = DirectTxReadStatus::kInvalidRange;
        return result;
    }

    result.requestedEndFrame = request.firstFrame + request.frameCount;
    if (result.writtenEndFrame < result.requestedEndFrame) {
        result.status = DirectTxReadStatus::kUnderrun;
        return result;
    }

    result.firstFramePtr = reader_.Frame(request.firstFrame);
    if (!result.firstFramePtr) {
        result.status = DirectTxReadStatus::kInvalidBinding;
        return result;
    }

    result.status = DirectTxReadStatus::kAvailable;
    return result;
}

} // namespace ASFW::AudioEngine::Direct::Tx
