// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace ASFW::Audio::Families::BeBoB::MAudio {

/// RX anchors remain available for other profiles; M-Audio HAL publication
/// has one authority while its internal TX clock is armed.
[[nodiscard]] constexpr bool ShouldMirrorRxClockAnchor(
    const bool mAudioTxClockProfile) noexcept {
    return !mAudioTxClockProfile;
}

} // namespace ASFW::Audio::Families::BeBoB::MAudio
