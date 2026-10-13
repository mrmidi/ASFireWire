// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeChildProcess.hpp"
#include <optional>
#include <string>
namespace Pro40Audio {
struct IdentityLabOutcome {
    std::optional<int32_t> status;
    std::string reason;
    bool reaped=false;
    pid_t pendingPID=0; // On unresolved cleanup caller must retain responsibility.
    int resourceError=0; // Nonzero forbids treating the session as clean/reusable.
    int preparationError=0; // Original setup/spawn error, separate from cleanup.
    std::optional<NativeChildProcess> process=std::nullopt; // Retained observation/cleanup responsibility.
};
}
