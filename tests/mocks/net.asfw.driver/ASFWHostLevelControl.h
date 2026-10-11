// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Host stand-in for the IIG-generated ASFWHostLevelControl.h. The host tests
// only hold it in the audio driver's device state, so an owned, refcounted
// object is all the private header needs.

#pragma once

#include <DriverKit/OSObject.h>

class ASFWHostLevelControl : public OSObject {};
