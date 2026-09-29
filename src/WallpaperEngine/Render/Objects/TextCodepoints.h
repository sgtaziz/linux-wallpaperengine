#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>
#include <vector>

namespace WallpaperEngine::Render::Objects {

// Scene and script strings are UTF-8. Invalid sequences become replacement
// glyphs instead of splitting one character into several FreeType requests.
inline std::vector<char32_t> decodeTextCodepoints (std::string_view text) {
    constexpr char32_t replacement = 0xfffd;
    std::vector<char32_t> result;
    result.reserve (text.size ());
    for (size_t index = 0; index < text.size ();) {
        const auto first = static_cast<uint8_t> (text[index]);
        if (first < 0x80) {
            result.push_back (first);
            ++index;
            continue;
        }
        const unsigned int length = first >= 0xc2 && first <= 0xdf ? 2
            : first >= 0xe0 && first <= 0xef ? 3
            : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        if (!length || index + length > text.size ()) {
            result.push_back (replacement);
            ++index;
            continue;
        }
        char32_t codepoint = first & ((1u << (7 - length)) - 1);
        bool valid = true;
        for (unsigned int part = 1; part < length; ++part) {
            const auto byte = static_cast<uint8_t> (text[index + part]);
            if ((byte & 0xc0) != 0x80) {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6) | (byte & 0x3f);
        }
        if (!valid) {
            result.push_back (replacement);
            ++index;
            continue;
        }
        const char32_t minimum = length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000;
        if (codepoint < minimum || (codepoint >= 0xd800 && codepoint <= 0xdfff)
            || codepoint > 0x10ffff)
            codepoint = replacement;
        result.push_back (codepoint);
        index += length;
    }
    return result;
}

} // namespace WallpaperEngine::Render::Objects
