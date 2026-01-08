#pragma once
#include <DriverKit/OSObject.h>
class OSString : public OSObject {
public:
    static OSString* withCString(const char* cString) { return new OSString(); }
    const char* getCStringNoCopy() const { return ""; }
};
