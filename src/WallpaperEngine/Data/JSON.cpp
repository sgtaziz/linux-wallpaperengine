#include "JSON.h"

#include "WallpaperEngine/Data/Parsers/UserSettingParser.h"

#include <algorithm>
#include <cctype>

namespace {
std::string location (const std::string& source, const std::string& input, size_t byte) {
    size_t line = 1, column = 1;
    for (size_t i = 0; i < std::min (byte, input.size ()); ++i) {
        if (input[i] == '\n') { ++line; column = 1; }
        else { ++column; }
    }
    return source + ":" + std::to_string (line) + ":" + std::to_string (column) + ": ";
}
}

using namespace WallpaperEngine::Data::JSON;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::Data::Parsers;

JSON WallpaperEngine::Data::JSON::parseAuthoringJson (const std::string& content,
                                                     const std::string& source) {
    std::string normalized = content;
    std::vector<size_t> commas;
    bool inString = false;
    for (size_t i = 0; i < content.size (); ++i) {
        const char c = content[i];
        if (inString) {
            if (c == '\\' && i + 1 < content.size ()) { ++i; }
            else if (c == '"') { inString = false; }
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == ',') { commas.push_back (i); continue; }
        if (c != '/' || i + 1 >= content.size ()) continue;
        if (content[i + 1] == '/') {
            normalized[i++] = ' ';
            normalized[i] = ' ';
            while (i + 1 < content.size () && content[i + 1] != '\n' && content[i + 1] != '\r') {
                normalized[++i] = ' ';
            }
        } else if (content[i + 1] == '*') {
            const size_t start = i;
            normalized[i++] = ' ';
            normalized[i] = ' ';
            bool closed = false;
            while (i + 1 < content.size ()) {
                if (content[i + 1] == '*' && i + 2 < content.size () && content[i + 2] == '/') {
                    normalized[++i] = ' ';
                    normalized[++i] = ' ';
                    closed = true;
                    break;
                }
                ++i;
                if (content[i] != '\n' && content[i] != '\r') normalized[i] = ' ';
            }
            if (!closed) throw JsonSyntaxError (location (source, content, start) + "unterminated block comment");
        }
    }
    for (size_t comma : commas) {
        size_t next = comma + 1;
        while (next < normalized.size () && std::isspace (static_cast<unsigned char> (normalized[next]))) ++next;
        if (next < normalized.size () && (normalized[next] == ']' || normalized[next] == '}')) {
            normalized[comma] = ' ';
        }
    }
    try {
        return JSON::parse (normalized);
    } catch (const JSON::parse_error& e) {
        const size_t byte = e.byte > 0 ? e.byte - 1 : 0;
        throw JsonSyntaxError (location (source, content, byte) + e.what ());
    } catch (const JSON::out_of_range& e) {
        // nlohmann does not expose a byte offset for numeric overflow errors.
        throw JsonSyntaxError (source + ": " + e.what ());
    }
}

UserSettingUniquePtr JsonExtensions::user (const std::string& key, const Properties& properties) const {
    const auto value = this->require (key, "User setting without default value must be present");

    return UserSettingParser::parse (value, properties);
}

UserSettingUniquePtr JsonExtensions::color (const std::string& key, const Properties& properties) const {
    const auto value = this->require (key, "User setting without default value must be present");

    return UserSettingParser::parse (value, properties, true);
}
