#pragma once

#include <string>
#include <string_view>

namespace asb::platform {

// The shared code stores device strings as std::wstring (Windows UTF-16).
// On Linux wchar_t is a UTF-32 code point, and sysfs/filesystem strings are
// UTF-8, so convert explicitly instead of depending on the C locale.
inline std::wstring widen(std::string_view text) {
    std::wstring result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        char32_t codePoint = 0;
        std::size_t length = 1;
        if (lead < 0x80) {
            codePoint = lead;
        } else if ((lead >> 5) == 0x6) {
            codePoint = lead & 0x1F;
            length = 2;
        } else if ((lead >> 4) == 0xE) {
            codePoint = lead & 0x0F;
            length = 3;
        } else if ((lead >> 3) == 0x1E) {
            codePoint = lead & 0x07;
            length = 4;
        } else {
            result.push_back(L'�');
            ++index;
            continue;
        }
        if (index + length > text.size()) {
            result.push_back(L'�');
            break;
        }
        for (std::size_t extra = 1; extra < length; ++extra) {
            codePoint = (codePoint << 6) | (static_cast<unsigned char>(text[index + extra]) & 0x3F);
        }
        result.push_back(static_cast<wchar_t>(codePoint));
        index += length;
    }
    return result;
}

inline std::string narrow(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const auto character : text) {
        const auto codePoint = static_cast<char32_t>(character);
        if (codePoint < 0x80) {
            result.push_back(static_cast<char>(codePoint));
        } else if (codePoint < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            result.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }
    return result;
}

} // namespace asb::platform
