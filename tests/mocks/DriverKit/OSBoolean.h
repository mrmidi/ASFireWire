#pragma once
#include <DriverKit/OSObject.h>
class OSBoolean : public OSObject {
public:
    static OSBoolean* withBoolean(bool value) { return new OSBoolean(); }
    bool getValue() const { return false; }
};

inline OSBoolean kOSBooleanTrue_storage;  // NOSONAR(cpp:S5421): test mock object — must be non-const to match OSObject lifecycle
inline OSBoolean kOSBooleanFalse_storage; // NOSONAR(cpp:S5421): test mock object — must be non-const to match OSObject lifecycle
inline OSBoolean* const kOSBooleanTrue  = &kOSBooleanTrue_storage;  // NOSONAR(cpp:S5421): pointee is intentionally non-const
inline OSBoolean* const kOSBooleanFalse = &kOSBooleanFalse_storage; // NOSONAR(cpp:S5421): pointee is intentionally non-const
