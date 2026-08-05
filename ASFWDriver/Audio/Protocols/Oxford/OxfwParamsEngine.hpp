// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// OxfwParamsEngine.hpp - OXFW-common parameter cache and diff engine (FW-128).
//
// Two operations, neither of which is device logic:
//
//   Cache   issue every STATUS command for a parameter group and deserialize
//           the responses into one struct
//   Update  serialize the desired and the cached parameters, send ONLY the
//           commands whose encoded value differs, and advance the cache just
//           once the whole sequence succeeded
//
// The reference factors these out as two traits and instantiates them over
// unrelated parameter types - which is what establishes them as OXFW-common
// mechanism rather than Apogee code:
//
//   protocols/oxfw/src/lib.rs:75-90    OxfwFcpParamsOperation<P,T>::cache and
//                                      OxfwFcpMutableParamsOperation<P,T>::update
//   protocols/oxfw/src/apogee.rs:47-89 the Duet's five parameter groups
//   protocols/oxfw/src/tascam.rs:212-237 the FireOne's unrelated SpecificParams
//
// (snd-firewire-ctl-services, read as a behavioural source; no code copied.)
//
// The diff is taken on *serialized commands*, never on parameter fields. That is
// what lets one engine carry heterogeneous parameter types without knowing any
// of them, and it is what both reference instantiations do.
//
// Nothing here names a device, a transport, or FCP: commands reach the wire
// through an injected dispatch, so the engine instantiates against a fake with
// no device and no DriverKit.

#pragma once

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include <DriverKit/IOReturn.h>

namespace ASFW::Audio::Oxford {

/// Result of running one command sequence: the responses are meaningful for a
/// STATUS sequence and ignored for a CONTROL one.
template <typename Command>
using OxfwSequenceCallback = std::function<void(IOReturn, const std::vector<Command>&)>;

/// The engine's only contact with the outside world. `isStatus` selects the AV/C
/// ctype, matching the reference's split between `avc.status` and `avc.control`.
/// A sequence must run in order and stop at the first failure.
template <typename Command>
using OxfwCommandDispatch =
    std::function<void(const std::vector<Command>&, bool isStatus, OxfwSequenceCallback<Command>)>;

/// `Serdes` supplies the device half and must provide:
///
///   using Params  = <the parameter struct>
///   using Command = <the vendor command type, equality-comparable>
///   static std::vector<Command> BuildQuery();
///   static std::vector<Command> BuildControl(const Params&);
///   static Params Parse(const std::vector<Command>&);
///
/// `BuildControl` must emit a fixed command shape for a given `Params` type -
/// same length, same order, same codes - because the diff is positional.
template <typename Serdes>
class OxfwParamsCache {
public:
    using Params = typename Serdes::Params;
    using Command = typename Serdes::Command;
    using Dispatch = OxfwCommandDispatch<Command>;
    using CacheCallback = std::function<void(IOReturn, const Params&)>;
    using UpdateCallback = std::function<void(IOReturn)>;

    OxfwParamsCache() = default;
    explicit OxfwParamsCache(Dispatch dispatch) : dispatch_(std::move(dispatch)) {}

    /// Rebinding the dispatch means a different transport epoch, so whatever the
    /// device was known to hold no longer applies.
    void SetDispatch(Dispatch dispatch) {
        dispatch_ = std::move(dispatch);
        Invalidate();
    }

    [[nodiscard]] bool IsEstablished() const noexcept { return established_; }
    [[nodiscard]] const Params& Cached() const noexcept { return cached_; }

    /// Drop what the device is believed to hold. The next Update then writes the
    /// whole group instead of a diff against a stale picture.
    void Invalidate() noexcept {
        cached_ = Params{};
        established_ = false;
    }

    /// Read the whole group from the device and replace the cache with it.
    void Cache(CacheCallback callback) {
        if (!dispatch_) {
            if (callback) {
                callback(kIOReturnNotReady, cached_);
            }
            return;
        }

        dispatch_(Serdes::BuildQuery(), true,
                  [this, callback = std::move(callback)](
                      IOReturn status, const std::vector<Command>& responses) {
                      if (status != kIOReturnSuccess) {
                          if (callback) {
                              callback(status, Params{});
                          }
                          return;
                      }
                      cached_ = Serdes::Parse(responses);
                      established_ = true;
                      if (callback) {
                          callback(kIOReturnSuccess, cached_);
                      }
                  });
    }

    /// Send only what changed. This is the property that stops one volume tick
    /// re-sending a whole parameter group.
    void Update(const Params& params, UpdateCallback callback) {
        if (!dispatch_) {
            if (callback) {
                callback(kIOReturnNotReady);
            }
            return;
        }

        const std::vector<Command> desired = Serdes::BuildControl(params);
        const std::vector<Command> pending = SelectChanged(desired);

        if (pending.empty()) {
            // Already matches, so nothing goes on the wire - but the cache still
            // advances, exactly as the reference's empty-filter case does.
            cached_ = params;
            established_ = true;
            if (callback) {
                callback(kIOReturnSuccess);
            }
            return;
        }

        dispatch_(pending, false,
                  [this, params, callback = std::move(callback)](IOReturn status,
                                                                 const std::vector<Command>&) {
                      // The cache advances only on full success. A group that
                      // failed part-way must re-send the failed command on the
                      // next attempt rather than diff it away as already applied.
                      if (status == kIOReturnSuccess) {
                          cached_ = params;
                          established_ = true;
                      }
                      if (callback) {
                          callback(status);
                      }
                  });
    }

private:
    [[nodiscard]] std::vector<Command> SelectChanged(const std::vector<Command>& desired) const {
        if (!established_) {
            // Diffing now would compare against a default-constructed struct the
            // hardware never reported, which would drop commands that happen to
            // match the default. The reference only ever reaches update() after a
            // successful cache(); when we have no cache, write everything.
            return desired;
        }

        const std::vector<Command> current = Serdes::BuildControl(cached_);
        if (current.size() != desired.size()) {
            // A serdes whose command count varies with its values would make a
            // positional diff silently skip commands. Fall back to a full write
            // rather than send a wrong subset.
            return desired;
        }

        std::vector<Command> changed;
        for (size_t i = 0; i < desired.size(); ++i) {
            if (!(desired[i] == current[i])) {
                changed.push_back(desired[i]);
            }
        }
        return changed;
    }

    Dispatch dispatch_{};
    Params cached_{};
    bool established_{false};
};

} // namespace ASFW::Audio::Oxford
