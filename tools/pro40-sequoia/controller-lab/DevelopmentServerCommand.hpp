// SPDX-License-Identifier: Apache-2.0
#pragma once
namespace Pro40Lab {
// Packaged helper diagnostic: provisioned identity, anonymous listener, no caller.
int developmentServerCommand();
// Implemented and linked, but not dispatched until controller lifecycle is ready.
int developmentServeVerifierCommand();
}
