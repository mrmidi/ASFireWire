#pragma once
#include <DriverKit/OSObject.h>
#include <vector>
#include <cstring>

class OSData : public OSObject {
    std::vector<uint8_t> data;
public:
    static OSData* withCapacity(uint32_t capacity) {
        auto* obj = new OSData();
        obj->data.reserve(capacity);
        return obj;
    }
    
    static OSData* withBytes(const void* bytes, uint32_t length) {
        auto* obj = new OSData();
        obj->data.assign((const uint8_t*)bytes, (const uint8_t*)bytes + length);
        return obj;
    }
    
    bool appendBytes(const void* bytes, uint32_t length) {
        data.insert(data.end(), (const uint8_t*)bytes, (const uint8_t*)bytes + length);
        return true;
    }
    
    uint32_t getLength() const { return static_cast<uint32_t>(data.size()); }
    const void* getBytesNoCopy() const { return data.data(); }
};
