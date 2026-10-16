// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
namespace Pro40::WorkerDiagnosticTiming {
// Native signed fake-runner diagnostic only. Isolated helper tests keep 8s.
inline constexpr auto capture=std::chrono::seconds(2);
inline constexpr auto admission=std::chrono::seconds(30);
inline constexpr auto collection=std::chrono::seconds(35);
inline constexpr auto command=std::chrono::seconds(40);
static_assert(admission<collection && collection<command);
}
