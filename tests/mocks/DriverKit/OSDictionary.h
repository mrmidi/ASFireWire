#pragma once
#include <DriverKit/OSObject.h>
class OSDictionary : public OSObject {
public:
    static OSDictionary* withCapacity(uint32_t capacity) { return new OSDictionary(); }
    void setObject(const class OSSymbol* key, OSObject* value) { /* no-op stub */ }
    void setObject(const char* key, OSObject* value) { /* no-op stub */ }
};
