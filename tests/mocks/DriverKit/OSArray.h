#pragma once
#include <DriverKit/OSObject.h>
class OSArray : public OSObject {
public:
    static OSArray* withCapacity(uint32_t capacity) { return new OSArray(); }
    void setObject(OSObject* value) { /* no-op stub */ }
    void release() const { delete this; }
};
