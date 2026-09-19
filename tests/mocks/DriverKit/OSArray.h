#pragma once
#include <DriverKit/OSObject.h>
#include <cstdint>
#include <vector>

// Retains what it holds and releases on free, like the DriverKit original, so
// the OSSharedPtr(..., OSNoRetain) handoff in driver code behaves on the host
// the way it does in the dext.
class OSArray : public OSObject {
public:
    static OSArray* withCapacity(uint32_t capacity) {
        auto* array = new OSArray();
        array->objects_.reserve(capacity);
        return array;
    }

    void setObject(OSObject* value) {
        if (value == nullptr) {
            return;
        }
        value->retain();
        objects_.push_back(value);
    }

    [[nodiscard]] uint32_t getCount() const { return static_cast<uint32_t>(objects_.size()); }

    [[nodiscard]] OSObject* getObject(uint32_t index) const {
        return index < objects_.size() ? objects_[index] : nullptr;
    }

    void free() override {
        for (OSObject* object : objects_) {
            object->release();
        }
        objects_.clear();
        OSObject::free();
    }

private:
    std::vector<OSObject*> objects_{};
};
