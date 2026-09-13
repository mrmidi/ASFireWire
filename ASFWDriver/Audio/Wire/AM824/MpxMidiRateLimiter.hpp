//
// MpxMidiRateLimiter.hpp
// ASFWDriver
//
// Models the receiving device's MIDI UART so we do not feed it faster than
// 31.25 kbaud can clock out.
//
// The slot budget is not the constraint: at 48 kHz blocking each port gets one
// byte opportunity per DATA packet, 6000/s, nearly twice MIDI 1.0's 3125 B/s.
// A device driven at the slot rate garbles or drops. Both reference stacks
// model this -- neither omits it.
//
// Behaviour follows Linux's fractional accumulator (amdtp-am824.c:262-292),
// written fresh. Values are held in bytes x sample-rate so the arithmetic stays
// integral:
//
//   limit = rate - kModelledBytesPerSecond * sytInterval + 1
//   age:   used -= kModelledBytesPerSecond * sytInterval, floored at 0
//   emit:  used += rate
//
// Linux deliberately models 3093 B/s rather than the nominal 3125, because
// "the MIDI port's clock might be a bit slow" (amdtp-am824.c:19-22). Keeping
// that margin is the conservative choice against unknown hardware.
//
// Pure logic: no DriverKit dependency, no allocation, no locking.
//

#pragma once

#include <cstdint>

namespace ASFW::Encoding {

/// Modelled MIDI byte rate. Nominally 3125; Linux uses 3093 for clock margin.
inline constexpr uint32_t kModelledMidiBytesPerSecond = 3093;

/// Per-port UART model for one direction.
///
/// "Time" here is wire opportunities, not wall clock: Age() is called once per
/// port per eligible packet. Driving it from dispatch time instead would let a
/// single wake that prepares several packets age the model several times, or a
/// stalled wake age it not at all.
class MpxMidiRateLimiter final {
public:
    static constexpr uint32_t kPorts = 8;

    /// `sampleRateHz` is the stream's rate and `sytIntervalFrames` its
    /// SYT interval -- the data blocks one DATA packet carries at that rate.
    void Configure(uint32_t sampleRateHz, uint32_t sytIntervalFrames) noexcept {
        rate_ = sampleRateHz;
        drainPerPacket_ = kModelledMidiBytesPerSecond * sytIntervalFrames;
        // One byte costs `rate`; the limit is what remains after a packet's
        // worth of drain, so a port that just spent a byte has to wait out
        // roughly rate/drain packets before it may spend another.
        limit_ = (rate_ > drainPerPacket_) ? (rate_ - drainPerPacket_ + 1) : 1;
        Reset();
    }

    void Reset() noexcept {
        for (auto& used : used_) used = 0;
    }

    /// Advance the model for one wire opportunity and report whether `port` may
    /// spend a byte on it.
    ///
    /// Ages unconditionally: a packet that carries no MIDI still took the wire
    /// time the device needed to clock out what it already has.
    [[nodiscard]] bool AgeAndMayEmit(uint32_t port) noexcept {
        if (port >= kPorts || rate_ == 0) return false;
        uint32_t used = used_[port];
        if (used == 0) return true;
        used = (used > drainPerPacket_) ? (used - drainPerPacket_) : 0;
        used_[port] = used;
        return used < limit_;
    }

    /// Charge one emitted byte.
    void Debit(uint32_t port) noexcept {
        if (port >= kPorts) return;
        used_[port] += rate_;
    }

    /// Return the charge for a byte that was selected but never published.
    ///
    /// Only the debit is undone. Elapsed wire time is not: the packet still
    /// went out, the device still clocked for that long, and pretending
    /// otherwise would let a run of cancellations burst past the UART.
    void RollbackDebit(uint32_t port) noexcept {
        if (port >= kPorts) return;
        used_[port] = (used_[port] > rate_) ? (used_[port] - rate_) : 0;
    }

    [[nodiscard]] uint32_t UsedFor(uint32_t port) const noexcept {
        return port < kPorts ? used_[port] : 0;
    }
    [[nodiscard]] uint32_t Limit() const noexcept { return limit_; }
    [[nodiscard]] bool Configured() const noexcept { return rate_ != 0; }

private:
    uint32_t rate_{0};
    uint32_t drainPerPacket_{0};
    uint32_t limit_{0};
    uint32_t used_[kPorts]{};
};

} // namespace ASFW::Encoding
