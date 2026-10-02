#pragma once

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace WallpaperEngine::Render::Shaders {

// GL already adapts native clip positions. Preserve the explicit native
// conversion of exported reflection vectors without changing lighting's
// shared normal matrix or the previously computed tangent-space directions.
inline std::string nativeReflectionVectorSource (const std::string& source) {
    struct Line { std::string raw, code; };
    std::vector<Line> lines;
    bool comment = false;
    std::istringstream input (source);
    for (std::string raw; std::getline (input, raw);) {
        std::string code;
        for (size_t i = 0; i < raw.size ();) {
            if (comment) {
                if (raw.compare (i, 2, "*/") == 0) { comment = false; i += 2; }
                else ++i;
            } else if (raw.compare (i, 2, "/*") == 0) { comment = true; code += ' '; i += 2; }
            else if (raw.compare (i, 2, "//") == 0) break;
            else code += raw[i++];
        }
        // Native shipped sources use CRLF. ECMAScript '.' does not match CR
        // in the directive capture; normalize only the parsed view so that
        // exact authored text and instruction/comment placement are retained.
        if (!code.empty () && code.back () == '\r') code.pop_back ();
        lines.push_back ({std::move (raw), std::move (code)});
    }
    std::string clean;
    for (const auto& line : lines) clean += line.code + '\n';
    static const std::regex exported (R"(\b(?:varying|out)\s+vec3\s+(v_Tangent|v_Bitangent)\s*;)");
    bool tangent = false, bitangent = false;
    for (auto i = std::sregex_iterator (clean.begin (), clean.end (), exported);
         i != std::sregex_iterator (); ++i) {
        tangent |= (*i)[1] == "v_Tangent";
        bitangent |= (*i)[1] == "v_Bitangent";
    }
    if (!tangent && !bitangent) return source;

    struct Scope {
        bool reflection = false, normalMap = false;
        std::string complement;
        std::vector<std::string> conversions;
    };
    static const std::regex directive (R"(^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$)");
    static const std::regex negate (R"(^\s*(v_Tangent|v_Bitangent)\s*\.\s*y\s*=\s*-\s*\1\s*\.\s*y\s*;\s*$)");
    constexpr auto marker = "// Native reflection vector coordinate bridge";
    std::vector<Scope> stack;
    std::string result;
    bool changed = false;
    for (size_t i = 0; i < lines.size (); ++i) {
        const auto& line = lines[i];
        result += line.raw + '\n';
        std::smatch match;
        if (std::regex_match (line.code, match, directive)) {
            const std::string kind = match[1];
            std::string condition = match[2];
            std::erase_if (condition, [] (unsigned char c) { return std::isspace (c); });
            if (kind == "if" || kind == "ifdef" || kind == "ifndef") {
                Scope scope;
                if (kind == "if") {
                    std::string booleanScope = condition;
                    std::erase (booleanScope, '(');
                    std::erase (booleanScope, ')');
                    scope.reflection = booleanScope == "REFLECTION" || booleanScope == "REFLECTION&&NORMALMAP";
                    scope.normalMap = booleanScope == "NORMALMAP" || booleanScope == "REFLECTION&&NORMALMAP";
                    if (condition == "HLSL") scope.complement = "!(HLSL)";
                    else if (condition == "defined(HLSL)" || condition == "definedHLSL")
                        scope.complement = "!defined(HLSL)";
                } else if (kind == "ifdef" && condition == "HLSL")
                    scope.complement = "!defined(HLSL)";
                stack.push_back (std::move (scope));
            } else if (!stack.empty () && (kind == "else" || kind == "elif")) {
                // An alternate/fallback branch is not this native contract.
                stack.back () = {};
            } else if (!stack.empty () && kind == "endif") {
                auto scope = std::move (stack.back ());
                stack.pop_back ();
                const bool reflection = std::ranges::any_of (stack, [] (const Scope& s) { return s.reflection; });
                const bool normalMap = std::ranges::any_of (stack, [] (const Scope& s) { return s.normalMap; });
                const bool alreadyBridged = i + 1 < lines.size () && lines[i + 1].raw == marker;
                if (reflection && normalMap && !scope.complement.empty ()
                    && !scope.conversions.empty () && !alreadyBridged) {
                    result += std::string (marker) + "\n#if " + scope.complement + '\n';
                    for (const auto& conversion : scope.conversions) result += conversion + '\n';
                    result += "#endif\n";
                    changed = true;
                }
            }
        } else if (!stack.empty () && !stack.back ().complement.empty ()
                   && std::regex_match (line.code, match, negate)) {
            const std::string name = match[1];
            if ((name == "v_Tangent" && tangent) || (name == "v_Bitangent" && bitangent))
                stack.back ().conversions.push_back (line.raw);
        }
    }
    return changed ? result : source;
}
} // namespace WallpaperEngine::Render::Shaders
