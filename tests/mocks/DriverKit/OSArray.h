#pragma once
#include <DriverKit/OSObject.h>
class OSArray : public OSObject {
public:
    static OSArray* withCapacity(uint32_t capacity) { return new OSArray(); }
    void setObject(OSObject* value) {}
    void release() const { delete this; }
};
