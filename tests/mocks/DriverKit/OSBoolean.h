#pragma once
#include <DriverKit/OSObject.h>
class OSBoolean : public OSObject {
public:
    static OSBoolean* withBoolean(bool value) { return new OSBoolean(); }
    bool getValue() const { return false; }
};
