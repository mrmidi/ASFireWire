// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Host stand-in for the IIG-generated ASFWProtocolBooleanControl.h. The host
// tests only hold these in BoolControlSlot, so an owned, refcounted object is
// all the audio driver's private header needs.

#pragma once

#include <DriverKit/OSObject.h>

class ASFWProtocolBooleanControl : public OSObject {};
