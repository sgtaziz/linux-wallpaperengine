#pragma once

#include "WallpaperEngine/Data/JSON.h"

#include <cctype>
#include <string>

namespace WallpaperEngine::Render::Shaders {

inline Data::JSON::JSON parseShaderMetadata (const std::string& content, const std::string& source) {
    // Native shader comments use JsonCpp (14016ce60 -> 1401668f0), which
    // accepts decimal leading zeroes. The original-runtime metadata probe
    // preserves default 0.75 with range [0,01]. Keep scene JSON and strings
    // untouched, and retain byte positions for diagnostics.
    std::string normalized = content;
    for (size_t i = 0; i < content.size (); ++i) {
        if (content[i] == '"') {
            while (++i < content.size ()) {
                if (content[i] == '\\' && i + 1 < content.size ()) ++i;
                else if (content[i] == '"') break;
            }
            continue;
        }
        if (content[i] == '/' && i + 1 < content.size ()) {
            if (content[i + 1] == '/') {
                while (i < content.size () && content[i] != '\n') ++i;
                continue;
            }
            if (content[i + 1] == '*') {
                const auto end = content.find ("*/", i + 2);
                i = end == std::string::npos ? content.size () : end + 1;
                continue;
            }
        }
        const size_t digits = i + (content[i] == '-' ? 1 : 0);
        if (digits >= content.size () || !std::isdigit (static_cast<unsigned char> (content[digits]))) continue;
        if (i > 0 && (std::isalnum (static_cast<unsigned char> (content[i - 1]))
                      || content[i - 1] == '_' || content[i - 1] == '.')) continue;
        size_t integerEnd = digits;
        while (integerEnd < content.size () && std::isdigit (static_cast<unsigned char> (content[integerEnd])))
            ++integerEnd;
        size_t first = digits;
        while (first + 1 < integerEnd && content[first] == '0') ++first;
        size_t end = integerEnd;
        while (end < content.size () && (std::isdigit (static_cast<unsigned char> (content[end]))
            || content[end] == '.' || content[end] == 'e' || content[end] == 'E'
            || content[end] == '+' || content[end] == '-')) ++end;
        if (first != digits) {
            std::string token = content.substr (i, digits - i) + content.substr (first, end - first);
            token.resize (end - i, ' ');
            normalized.replace (i, end - i, token);
        }
        i = end - 1;
    }
    return Data::JSON::parseAuthoringJson (normalized, source);
}

} // namespace WallpaperEngine::Render::Shaders
