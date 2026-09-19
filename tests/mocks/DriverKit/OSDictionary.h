#pragma once
#include <DriverKit/OSObject.h>
#include <cstdint>
#include <map>
#include <string>

class OSSymbol;

class OSDictionary : public OSObject {
public:
    static OSDictionary* withCapacity(uint32_t /*capacity*/) { return new OSDictionary(); }

    void setObject(const char* key, OSObject* value) {
        if (key == nullptr || value == nullptr) {
            return;
        }
        auto existing = objects_.find(key);
        if (existing != objects_.end()) {
            existing->second->release();
            objects_.erase(existing);
        }
        value->retain();
        objects_.emplace(key, value);
    }
    void setObject(const OSSymbol* /*key*/, OSObject* /*value*/) { /* unused on host */ }

    [[nodiscard]] OSObject* getObject(const char* key) const {
        if (key == nullptr) {
            return nullptr;
        }
        const auto found = objects_.find(key);
        return found != objects_.end() ? found->second : nullptr;
    }

    [[nodiscard]] uint32_t getCount() const { return static_cast<uint32_t>(objects_.size()); }

    void free() override {
        for (auto& [key, object] : objects_) {
            object->release();
        }
        objects_.clear();
        OSObject::free();
    }

private:
    std::map<std::string, OSObject*> objects_{};
};
