#pragma once
#include <DriverKit/OSObject.h>
#include <string>

class OSString : public OSObject {
public:
    static OSString* withCString(const char* cString) {
        auto* string = new OSString();
        string->value_ = cString != nullptr ? cString : "";
        return string;
    }
    [[nodiscard]] const char* getCStringNoCopy() const { return value_.c_str(); }
    [[nodiscard]] size_t getLength() const { return value_.size(); }

private:
    std::string value_{};
};
