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
#include <utility>
#include <vector>

namespace ASFW::UserClient::Wire {

/// Minimal JSON writer: commas are tracked per open container. The output is
/// ASCII JSON: a well-formed UTF-8 sequence (a driver-built name such as
/// "Mixer 1 → 2") becomes its \u escape; any other byte (a device string in
/// an unknown encoding) is taken as a Latin-1 code point.
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
        for (size_t i = 0; i < text.size();) {
            const auto c = static_cast<uint8_t>(text[i]);
            if (c == '"' || c == '\\') { out_ += '\\'; out_ += text[i++]; continue; }
            if (c >= 0x20 && c < 0x7F) { out_ += text[i++]; continue; }
            if (const auto [codePoint, length] = DecodeUtf8(text.substr(i)); length > 0) {
                if (codePoint > 0xFFFF) { // A surrogate pair (RFC 8259 §7).
                    const uint32_t v = codePoint - 0x10000;
                    Escape(0xD800 + (v >> 10));
                    Escape(0xDC00 + (v & 0x3FF));
                } else {
                    Escape(codePoint);
                }
                i += length;
                continue;
            }
            Escape(c); // Control byte, or a byte that starts no UTF-8 sequence: Latin-1.
            ++i;
        }
        out_ += '"';
    }
    void Escape(uint32_t unit) {
        static constexpr char kHex[] = "0123456789abcdef";
        out_ += "\\u";
        for (int shift = 12; shift >= 0; shift -= 4) out_ += kHex[(unit >> shift) & 0x0F];
    }
    /// The code point a well-formed multi-byte UTF-8 sequence at the start of
    /// `text` encodes, and its length; length 0 when there is none. Overlong
    /// forms, surrogates and values past U+10FFFF are not well-formed.
    static std::pair<uint32_t, size_t> DecodeUtf8(std::string_view text) {
        const auto lead = static_cast<uint8_t>(text[0]);
        const size_t length = lead >= 0xF0 && lead <= 0xF4 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC2 && lead <= 0xDF ? 2 : 0;
        if (length == 0 || lead > 0xF4 || text.size() < length) return {0, 0};
        uint32_t codePoint = lead & (0xFF >> (length + 1));
        for (size_t k = 1; k < length; ++k) {
            const auto next = static_cast<uint8_t>(text[k]);
            if ((next & 0xC0) != 0x80) return {0, 0};
            codePoint = (codePoint << 6) | (next & 0x3F);
        }
        constexpr uint32_t kMinimum[] = {0, 0, 0x80, 0x800, 0x10000};
        if (codePoint < kMinimum[length] || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
            return {0, 0};
        return {codePoint, length};
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
