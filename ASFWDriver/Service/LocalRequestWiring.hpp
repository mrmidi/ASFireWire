// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// LocalRequestWiring.hpp — single entry point that builds the LocalRequestDispatch,
// constructs the per-protocol address handlers (CSR, FCP, DICE, SBP-2), and
// installs the dispatch as the sole owner of inbound request tCodes. This is the
// one place inbound local-request routing is assembled (FW-19).

#pragma once

struct ServiceContext;

namespace ASFW::Service {

// Creates the CSR responder (if needed), assembles all local-address handlers in
// priority order, and installs the dispatch on the PacketRouter. Idempotent-safe
// to call once after async + protocol deps exist.
void WireLocalRequestDispatch(::ServiceContext& ctx);

} // namespace ASFW::Service
