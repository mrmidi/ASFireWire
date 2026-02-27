#pragma once

#include "../Discovery/DiscoveryTypes.hpp"

namespace ASFW::Discovery {

// Holds per-node ROM scan state and validates legal FSM transitions.
class ROMScanNodeStateMachine {
public:
    enum class State : uint8_t {
        Idle,
        ReadingBIB,
        VerifyingIRM_Read,
        VerifyingIRM_Lock,
        ReadingRootDir,
        ReadingDetails,
        Complete,
        Failed
    };

    ROMScanNodeStateMachine() = default;

    ROMScanNodeStateMachine(uint8_t nodeIdIn,
                            Generation generation,
                            FwSpeed speed,
                            uint8_t retries)
        : nodeId_(nodeIdIn)
        , currentSpeed_(speed)
        , retriesLeft_(retries) {
        partialROM_.gen = generation;
        partialROM_.nodeId = nodeIdIn;
    }

    [[nodiscard]] bool IsTerminal() const {
        return state_ == State::Complete || state_ == State::Failed;
    }

    [[nodiscard]] uint8_t NodeId() const { return nodeId_; }
    [[nodiscard]] State CurrentState() const { return state_; }
    [[nodiscard]] FwSpeed CurrentSpeed() const { return currentSpeed_; }
    [[nodiscard]] uint8_t RetriesLeft() const { return retriesLeft_; }

    void SetCurrentSpeed(FwSpeed speed) { currentSpeed_ = speed; }
    void SetRetriesLeft(uint8_t retries) { retriesLeft_ = retries; }
    void DecrementRetries() {
        if (retriesLeft_ > 0) {
            --retriesLeft_;
        }
    }

    [[nodiscard]] ConfigROM& MutableROM() { return partialROM_; }
    [[nodiscard]] const ConfigROM& ROM() const { return partialROM_; }

    [[nodiscard]] bool NeedsIRMCheck() const { return needsIRMCheck_; }
    void SetNeedsIRMCheck(bool value) { needsIRMCheck_ = value; }

    [[nodiscard]] bool IRMCheckReadDone() const { return irmCheckReadDone_; }
    void SetIRMCheckReadDone(bool value) { irmCheckReadDone_ = value; }

    [[nodiscard]] bool IRMCheckLockDone() const { return irmCheckLockDone_; }
    void SetIRMCheckLockDone(bool value) { irmCheckLockDone_ = value; }

    [[nodiscard]] bool IRMIsBad() const { return irmIsBad_; }
    void SetIRMIsBad(bool value) { irmIsBad_ = value; }

    [[nodiscard]] uint32_t IRMBitBucket() const { return irmBitBucket_; }
    void SetIRMBitBucket(uint32_t value) { irmBitBucket_ = value; }

    [[nodiscard]] bool BIBInProgress() const { return bibInProgress_; }
    void SetBIBInProgress(bool value) { bibInProgress_ = value; }

    [[nodiscard]] bool CanTransitionTo(State next) const {
        using enum State;

        switch (state_) {
            case Idle:
                return next == ReadingBIB || next == Failed;
            case ReadingBIB:
                return next == VerifyingIRM_Read ||
                       next == ReadingRootDir ||
                       next == Complete ||
                       next == Idle ||
                       next == Failed;
            case VerifyingIRM_Read:
                return next == VerifyingIRM_Lock ||
                       next == ReadingRootDir ||
                       next == Failed;
            case VerifyingIRM_Lock:
                return next == ReadingRootDir || next == Failed;
            case ReadingRootDir:
                return next == ReadingDetails ||
                       next == Complete ||
                       next == Failed ||
                       next == Idle;
            case ReadingDetails:
                return next == Complete || next == Failed;
            case Complete:
                return next == Idle;  // manual reread
            case Failed:
                return next == Idle;  // manual retry
        }
        return false;
    }

    [[nodiscard]] bool TransitionTo(State next) {
        if (!CanTransitionTo(next)) {
            return false;
        }
        state_ = next;
        return true;
    }

    void ForceState(State next) {
        state_ = next;
    }

    void ResetForGeneration(Generation generation,
                            uint8_t nodeIdIn,
                            FwSpeed speed,
                            uint8_t retries) {
        nodeId_ = nodeIdIn;
        state_ = State::Idle;
        currentSpeed_ = speed;
        retriesLeft_ = retries;
        partialROM_ = ConfigROM{};
        partialROM_.gen = generation;
        partialROM_.nodeId = nodeIdIn;
        needsIRMCheck_ = false;
        irmCheckReadDone_ = false;
        irmCheckLockDone_ = false;
        irmIsBad_ = false;
        irmBitBucket_ = 0xFFFFFFFF;
        bibInProgress_ = false;
    }

private:
    uint8_t nodeId_{0xFF};
    State state_{State::Idle};
    FwSpeed currentSpeed_{FwSpeed::S100};
    uint8_t retriesLeft_{0};
    ConfigROM partialROM_{};

    bool needsIRMCheck_{false};
    bool irmCheckReadDone_{false};
    bool irmCheckLockDone_{false};
    bool irmIsBad_{false};
    uint32_t irmBitBucket_{0xFFFFFFFF};

    bool bibInProgress_{false};
};

} // namespace ASFW::Discovery
