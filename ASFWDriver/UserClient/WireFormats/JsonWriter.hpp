// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// JsonWriter.hpp - the minimal JSON writer the user-client documents share
// (AV/C discovery, device controls). Moved from AVCDiscoveryDocument.cpp.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ASFW::UserClient::Wire {

/// Minimal JSON writer: commas are tracked per open container. Strings from
/// the device are escaped byte by byte, so the output is valid ASCII JSON.
class Json {
public:
    Json& Open(char bracket) { Comma(); out_ += bracket; first_.push_back(true); return *this; }
    Json& Close(char bracket) { out_ += bracket; first_.pop_back(); return *this; }
    Json& Key(std::string_view key) { Comma(); Quote(key); out_ += ':'; pendingValue_ = true; return *this; }
    Json& String(std::string_view text) { Comma(); Quote(text); return *this; }
    Json& Number(uint64_t value) { Comma(); out_ += std::to_string(value); return *this; }
    Json& Bool(bool value) { Comma(); out_ += value ? "true" : "false"; return *this; }
    Json& Null() { Comma(); out_ += "null"; return *this; }
    /// A number token written as given ("-1.0000"): for decimals the writer's integer Number cannot carry.
    Json& RawNumber(std::string_view token) { Comma(); out_ += token; return *this; }
    Json& Hex(std::span<const uint8_t> bytes) {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string text;
        text.reserve(bytes.size() * 2);
        for (const auto b : bytes) { text += kHex[b >> 4]; text += kHex[b & 0x0F]; }
        return String(text);
    }
    [[nodiscard]] std::string Take() { return std::move(out_); }

private:
    void Quote(std::string_view text) {
        out_ += '"';
        for (const char raw : text) {
            const auto c = static_cast<uint8_t>(raw);
            if (c == '"' || c == '\\') { out_ += '\\'; out_ += raw; }
            else if (c >= 0x20 && c < 0x7F) out_ += raw;
            else { // Control and non-ASCII bytes: Latin-1 code points.
                static constexpr char kHex[] = "0123456789abcdef";
                out_ += "\\u00"; out_ += kHex[c >> 4]; out_ += kHex[c & 0x0F];
            }
        }
        out_ += '"';
    }
    void Comma() {
        if (pendingValue_) { pendingValue_ = false; return; } // Value right after its key.
        if (first_.empty()) return;
        if (!first_.back()) out_ += ',';
        first_.back() = false;
    }
    std::string out_;
    std::vector<bool> first_;
    bool pendingValue_{false};
};

} // namespace ASFW::UserClient::Wire
