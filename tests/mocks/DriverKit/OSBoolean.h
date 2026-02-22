#pragma once
#include <DriverKit/OSObject.h>
class OSBoolean : public OSObject {
public:
    static OSBoolean* withBoolean(bool value) { return new OSBoolean(); }
    bool getValue() const { return false; }
};

inline OSBoolean kOSBooleanTrue_storage;
inline OSBoolean kOSBooleanFalse_storage;
inline OSBoolean* const kOSBooleanTrue  = &kOSBooleanTrue_storage;
inline OSBoolean* const kOSBooleanFalse = &kOSBooleanFalse_storage;
