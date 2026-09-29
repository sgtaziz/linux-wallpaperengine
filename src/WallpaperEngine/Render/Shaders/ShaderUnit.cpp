#include "ShaderUnit.h"

#include "WallpaperEngine/Logging/Log.h"
#include <exception>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <functional>
#include <regex>
#include <stack>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "GLSLContext.h"
#include "ExactSourceCache.h"
#include "ParticleRopeShader.h"
#include "WallpaperEngine/Assets/AssetLoadException.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariable.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableFloat.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableInteger.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableVector2.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableVector3.h"
#include "WallpaperEngine/Render/Shaders/Variables/ShaderVariableVector4.h"

#include "WallpaperEngine/Data/Builders/VectorBuilder.h"
#include "WallpaperEngine/FileSystem/Container.h"

#define SHADER_HEADER(filename)                                                                                        \
    "#version 330\n"                                                                                                   \
    "// ======================================================\n"                                                      \
    "// Processed shader "                                                                                             \
	+ filename                                                                                                     \
	+ "\n"                                                                                                         \
	  "// ======================================================\n"                                                \
	  "precision highp float;\n"                                                                                   \
	  "#define mul(x, y) ((y) * (x))\n"                                                                            \
	  "#define max(x, y) max (y, x)\n"                                                                             \
	  "#define lerp mix\n"                                                                                         \
	  "#define frac fract\n"                                                                                       \
	  "#define CAST2(x) (vec2(x))\n"                                                                               \
	  "#define CAST3(x) (vec3(x))\n"                                                                               \
	  "#define CAST4(x) (vec4(x))\n"                                                                               \
	  "#define CAST3X3(x) (mat3(x))\n"                                                                             \
	  "#define float2 vec2\n"                                                                                      \
	  "#define float3 vec3\n"                                                                                      \
	  "#define float4 vec4\n"                                                                                      \
	  "#define int2 ivec2\n"                                                                                       \
	  "#define int3 ivec3\n"                                                                                       \
	  "#define int4 ivec4\n"                                                                                       \
	  "#define saturate(x) (clamp(x, 0.0, 1.0))\n"                                                                 \
	  "#define texSample2D texture\n"                                                                              \
	  "#define texSample2DLod textureLod\n"                                                                        \
	  "#define log10(x) (log2(x) * 0.301029995663981)\n"                                                           \
	  "#define atan2 atan\n"                                                                                       \
	  "#define fmod(x, y) ((x)-(y)*trunc((x)/(y)))\n"                                                              \
	  "#define ddx dFdx\n"                                                                                         \
	  "#define ddy(x) dFdy(-(x))\n"                                                                                \
	  "#define GLSL 1\n\n";
#define FRAGMENT_SHADER_DEFINES                                                                                        \
    "out vec4 out_FragColor;\n"                                                                                        \
    "#define varying in\n"
#define VERTEX_SHADER_DEFINES                                                                                          \
    "#define attribute in\n"                                                                                           \
    "#define varying out\n"
#define DEFINE_COMBO(name, value) "#define " + name + " " + std::to_string (value) + "\n";

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Data::Builders;
using namespace WallpaperEngine::Render::Shaders;

namespace {
ExactSourceCache linkedVaryingCache {128, 32 * 1024 * 1024};

// This installed Workshop vertex source has one extra directive after all
// DEFORMITY branches have closed. The native renderer displays the containing
// layer, while GLSL rejects the authored source and drops the entire layer.
// Restrict the repair to the exact package bytes; another revision may use
// that closing directive for a real outer conditional.
std::string repairKnownAudioBarsVertex (const std::string& file, std::string source) {
    if (file != "workshop/3082978660/effects/Simple_Audio_Bars" || source.size () != 2565)
        return source;
    uint64_t fingerprint = UINT64_C (14695981039346656037);
    for (const unsigned char byte : source)
        fingerprint = (fingerprint ^ byte) * UINT64_C (1099511628211);
    if (fingerprint != UINT64_C (0xe79d7affab1a6582)) return source;

    constexpr std::string_view marker = "\r\n#endif\r\n\r\n#if TRANSFORM";
    const size_t position = source.find (marker);
    if (position == std::string::npos || source.find (marker, position + 1) != std::string::npos)
        return source;
    source.erase (position + 2, std::string_view ("#endif").size ());
    sLog.out ("Repaired known malformed Audio Bars vertex directive in ", file);
    return source;
}

struct ShaderToken {
    std::string text;
    size_t start;
    size_t end;
    bool identifier;
};

std::vector<ShaderToken> shaderTokens (const std::string& source) {
    std::vector<ShaderToken> tokens;
    bool lineStart = true;
    for (size_t i = 0; i < source.size ();) {
        if (source[i] == '\n') { lineStart = true; ++i; }
        else if (std::isspace (static_cast<unsigned char> (source[i]))) ++i;
        else if (source.compare (i, 2, "//") == 0) {
            const size_t end = source.find ('\n', i);
            i = end == std::string::npos ? source.size () : end;
        } else if (source.compare (i, 2, "/*") == 0) {
            const size_t end = source.find ("*/", i + 2);
            if (end == std::string::npos) break;
            if (source.find ('\n', i) < end) lineStart = true;
            i = end + 2;
        } else if (source[i] == '#' && lineStart) {
            const size_t end = source.find ('\n', i);
            i = end == std::string::npos ? source.size () : end;
        } else if (std::isalpha (static_cast<unsigned char> (source[i])) || source[i] == '_') {
            const size_t start = i++;
            while (i < source.size () &&
                   (std::isalnum (static_cast<unsigned char> (source[i])) || source[i] == '_')) ++i;
            tokens.push_back ({source.substr (start, i - start), start, i, true});
            lineStart = false;
        } else {
            tokens.push_back ({source.substr (i, 1), i, i + 1, false});
            lineStart = false;
            ++i;
        }
    }
    return tokens;
}
} // namespace

ShaderUnit::ShaderUnit (
    const GLSLContext::UnitType type, std::string file, std::string content, const AssetLocator& assetLocator,
    const ShaderConstantMap& constants, const TextureMap& passTextures, const TextureMap& overrideTextures,
    const ComboMap& combos, const ComboMap& overrideCombos, const ShaderConstantMap* baseConstants
) :
    m_type (type), m_file (std::move (file)), m_content (std::move (content)), m_combos (combos),
    m_overrideCombos (overrideCombos), m_constants (constants), m_baseConstants (baseConstants),
    m_passTextures (passTextures),
    m_overrideTextures (overrideTextures), m_link (nullptr), m_assetLocator (assetLocator) {
    if (m_type == GLSLContext::UnitType_Vertex && m_file == "genericropeparticle"
        && m_overrideCombos.contains ("LINUX_CPU_ROPE_UV")
        && m_overrideCombos.at ("LINUX_CPU_ROPE_UV") != 0)
        m_content = particleRopeCpuUVSource (std::move (m_content));
    // pre-process the shader so the units are clear
    this->preprocess ();
}

void ShaderUnit::preprocess () {
    this->m_preprocessed = this->m_type == GLSLContext::UnitType_Vertex
        ? repairKnownAudioBarsVertex (this->m_file, this->m_content) : this->m_content;
    this->m_includes = "";

    this->preprocessIncludes ();
    if (m_type == GLSLContext::UnitType_Vertex && m_file == "genericparticle"
        && m_overrideCombos.contains ("TRAILRENDERER")
        && m_overrideCombos.at ("TRAILRENDERER") != 0) {
        // common_particles.h computes the lateral sprite-trail basis with a
        // cross product. Particle streams and the local eye are Y-reflected
        // for GL, which reverses that cross product's handedness. Restore
        // native UV-associated corners without changing the authored UVs or
        // the velocity-aligned long axis. Ordinary sprites do not use it.
        constexpr std::string_view nativeRight =
            "right = cross(eyeDirection, localVelocity);";
        const size_t right = m_preprocessed.find (nativeRight);
        if (right != std::string::npos)
            m_preprocessed.replace (right, nativeRight.size (),
                "right = -cross(eyeDirection, localVelocity);");
    }
    this->collapseEquivalentConditionals ();
    this->preprocessRequires ();
    this->preprocessVariables ();

    // replace gl_FragColor with the equivalent
    const std::string from = "gl_FragColor";
    const std::string to = "out_FragColor";

    size_t start_pos = 0;
    while ((start_pos = this->m_preprocessed.find (from, start_pos)) != std::string::npos) {
	this->m_preprocessed.replace (start_pos, from.length (), to);
	start_pos += to.length (); // Handles case where 'to' is a substring of 'from'
    }
}

void ShaderUnit::collapseEquivalentConditionals () {
    struct Line { size_t start; size_t after; std::string directive; bool clean; };
    for (;;) {
        std::vector<Line> lines;
        bool blockComment = false;
        for (size_t start = 0; start < this->m_preprocessed.size ();) {
            const size_t newline = this->m_preprocessed.find ('\n', start);
            const size_t end = newline == std::string::npos ? this->m_preprocessed.size () : newline;
            const std::string line = this->m_preprocessed.substr (start, end - start);
            size_t firstCode = std::string::npos;
            for (size_t i = 0; i < line.size (); ++i) {
                if (blockComment) {
                    if (line.compare (i, 2, "*/") == 0) { blockComment = false; ++i; }
                } else if (line.compare (i, 2, "//") == 0) break;
                else if (line.compare (i, 2, "/*") == 0) { blockComment = true; ++i; }
                else if (firstCode == std::string::npos && !std::isspace (static_cast<unsigned char> (line[i])))
                    firstCode = i;
            }
            std::string directive;
            if (firstCode != std::string::npos && line[firstCode] == '#') {
                size_t wordEnd = firstCode + 1;
                while (wordEnd < line.size () && std::isalpha (static_cast<unsigned char> (line[wordEnd])))
                    ++wordEnd;
                directive = line.substr (firstCode, wordEnd - firstCode);
            }
            const bool clean = firstCode != std::string::npos &&
                               line.find ("/*") == std::string::npos &&
                               line.find ("*/") == std::string::npos &&
                               line.find ("//") == std::string::npos;
            lines.push_back ({start, newline == std::string::npos ? end : end + 1, directive, clean});
            start = lines.back ().after;
        }

        bool changed = false;
        for (size_t i = 0; i < lines.size () && !changed; ++i) {
            if (lines[i].directive != "#if" && lines[i].directive != "#ifdef" &&
                lines[i].directive != "#ifndef") continue;
            int depth = 1;
            size_t alternate = lines.size ();
            size_t closing = lines.size ();
            bool hasElif = false;
            for (size_t j = i + 1; j < lines.size (); ++j) {
                const auto& directive = lines[j].directive;
                if (directive == "#if" || directive == "#ifdef" || directive == "#ifndef") ++depth;
                else if (directive == "#endif" && --depth == 0) { closing = j; break; }
                else if (depth == 1 && directive == "#else") {
                    if (alternate != lines.size ()) hasElif = true;
                    alternate = j;
                }
                else if (depth == 1 && directive == "#elif") hasElif = true;
            }
            if (alternate == lines.size () || closing == lines.size () || hasElif ||
                !lines[i].clean || !lines[alternate].clean || !lines[closing].clean) continue;
            const std::string left = this->m_preprocessed.substr (
                lines[i].after, lines[alternate].start - lines[i].after);
            const std::string right = this->m_preprocessed.substr (
                lines[alternate].after, lines[closing].start - lines[alternate].after);
            if (left != right) continue;
            const std::string original = this->m_preprocessed.substr (
                lines[i].start, lines[closing].after - lines[i].start);
            std::string replacement = left;
            replacement.append (std::count (original.begin (), original.end (), '\n') -
                                std::count (left.begin (), left.end (), '\n'), '\n');
            this->m_preprocessed.replace (
                lines[i].start, lines[closing].after - lines[i].start, replacement);
            changed = true;
        }
        if (!changed) break;
    }
}

void ShaderUnit::preprocessVariables () {
    size_t start = 0, end = 0;
    while ((end = this->m_preprocessed.find ('\n', start)) != std::string::npos) {
	// Extract a line from the string
	std::string line = this->m_preprocessed.substr (start, end - start);
	const size_t combo = line.find ("// [COMBO] ");
	const size_t uniform = line.find ("uniform ");
	const size_t comment = line.find ("//");
	const size_t semicolon = line.find (';');
	const size_t metadata = comment == std::string::npos
	    ? std::string::npos : line.find_first_not_of (" \t", comment + 2);

	if (combo != std::string::npos) {
	    this->parseComboConfiguration (line.substr (combo + strlen ("// [COMBO] ")), 0);
	} else if (
	    uniform != std::string::npos && comment != std::string::npos && semicolon != std::string::npos &&
	    metadata != std::string::npos && line[metadata] == '{' &&
	    // this check ensures that the comment is after the semicolon (so it's not a commented-out line)
	    // this needs further refining as it's not taking into account block comments
	    semicolon < comment
	) {
	    // uniforms with comments should never have a value assigned, use this fact to detect the required parts
	    const size_t last_space = line.find_last_of (' ', semicolon);

	    if (last_space != std::string::npos) {
		const size_t previous_space = line.find_last_of (' ', last_space - 1);

		if (previous_space != std::string::npos) {
		    // extract type and name
		    std::string type = line.substr (previous_space + 1, last_space - previous_space - 1);
		    std::string name = line.substr (last_space + 1, semicolon - last_space - 1);
		    std::string json = line.substr (comment + 2);

		    this->parseParameterConfiguration (type, name, json);
		}
	    }
	}

	// Move to the next line
	start = end + 1;
    }
}

void ShaderUnit::preprocessIncludes () {
    // Keep conditional includes in scope. Unconditional helper files must follow
    // authored global declarations because GLSL functions cannot refer forward to
    // uniforms (genericimage4.frag places common_pbr_2.h before g_Texture0).
    std::vector<std::string> includeStack;
    std::string deferred;
    const auto firstFunctionLine = [](const std::string& source) {
        struct Token { std::string text; size_t start; bool identifier; };
        std::vector<Token> tokens;
        bool blockComment = false;
        bool lineComment = false;
        bool lineStart = true;
        for (size_t i = 0; i < source.size ();) {
            if (lineComment) {
                if (source[i++] == '\n') { lineComment = false; lineStart = true; }
            } else if (blockComment) {
                if (source.compare (i, 2, "*/") == 0) { blockComment = false; i += 2; }
                else if (source[i++] == '\n') lineStart = true;
            } else if (source.compare (i, 2, "//") == 0) { lineComment = true; i += 2; }
            else if (source.compare (i, 2, "/*") == 0) { blockComment = true; i += 2; }
            else if (source[i] == '#' && lineStart) {
                const size_t end = source.find ('\n', i);
                i = end == std::string::npos ? source.size () : end;
            }
            else if (std::isalpha (static_cast<unsigned char> (source[i])) || source[i] == '_') {
                const size_t start = i++;
                while (i < source.size () &&
                       (std::isalnum (static_cast<unsigned char> (source[i])) || source[i] == '_')) ++i;
                tokens.push_back ({source.substr (start, i - start), start, true});
                lineStart = false;
            } else {
                if (source[i] == '\n') lineStart = true;
                else if (!std::isspace (static_cast<unsigned char> (source[i]))) {
                    tokens.push_back ({source.substr (i, 1), i, false});
                    lineStart = false;
                }
                ++i;
            }
        }
        for (size_t i = 0; i + 3 < tokens.size (); ++i) {
            if (!tokens[i].identifier || !tokens[i + 1].identifier || tokens[i + 2].text != "(") continue;
            int depth = 0;
            size_t after = i + 2;
            for (; after < tokens.size (); ++after) {
                if (tokens[after].text == "(") ++depth;
                else if (tokens[after].text == ")" && --depth == 0) { ++after; break; }
            }
            if (after < tokens.size () && tokens[after].text == "{") {
                const size_t previous = source.rfind ('\n', tokens[i].start);
                return previous == std::string::npos ? size_t (0) : previous + 1;
            }
        }
        return std::string::npos;
    };
    std::function<std::string(const std::string&, bool)> expand = [&](const std::string& source, bool root) {
        std::string result;
        bool blockComment = false;
        int conditionalDepth = 0;
        const size_t functionStart = root ? firstFunctionLine (source) : std::string::npos;
        size_t lineStart = 0;

        while (lineStart < source.size ()) {
            const size_t lineEnd = source.find ('\n', lineStart);
            const bool hasNewline = lineEnd != std::string::npos;
            const size_t end = hasNewline ? lineEnd : source.size ();
            const std::string line = source.substr (lineStart, end - lineStart);
            size_t firstCode = std::string::npos;

            for (size_t i = 0; i < line.size (); ++i) {
                if (blockComment) {
                    if (line.compare (i, 2, "*/") == 0) {
                        blockComment = false;
                        ++i;
                    }
                } else if (line.compare (i, 2, "//") == 0) {
                    break;
                } else if (line.compare (i, 2, "/*") == 0) {
                    blockComment = true;
                    ++i;
                } else if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && firstCode == std::string::npos) {
                    firstCode = i;
                }
            }

            const bool directive = firstCode != std::string::npos && line.compare (firstCode, 8, "#include") == 0 &&
                                   (firstCode + 8 == line.size () || line[firstCode + 8] == ' ' ||
                                    line[firstCode + 8] == '\t');
            if (!directive) {
                result += line;
                if (hasNewline) result += '\n';
            } else {
                const size_t quoteStart = line.find ('"', firstCode + 8);
                const size_t quoteEnd = quoteStart == std::string::npos ? std::string::npos :
                                        line.find ('"', quoteStart + 1);
                if (quoteStart == std::string::npos || quoteEnd == std::string::npos) {
                    // Leave malformed input to the shader compiler, with its original line.
                    result += line;
                    if (hasNewline) result += '\n';
                } else {
                    const std::string filename = line.substr (quoteStart + 1, quoteEnd - quoteStart - 1);
                    if (std::find (includeStack.begin (), includeStack.end (), filename) != includeStack.end ()) {
                        // A guarded recursive include may be inactive when GLSL evaluates its #if.
                        // Keep the directive so active cycles fail at shader compilation.
                        result += line;
                        if (hasNewline) result += '\n';
                    } else try {
                        includeStack.push_back (filename);
                        const std::string content = this->m_assetLocator.includeShader (filename);
                        result += line.substr (0, firstCode);
                        std::string expanded = "// begin of include from file " + filename + "\n";
                        expanded += expand (content, false);
                        if (content.empty () || content.back () != '\n') expanded += '\n';
                        expanded += "// end of included from file " + filename + "\n";
                        if (root && conditionalDepth == 0 && lineStart < functionStart) deferred += expanded;
                        else result += expanded;
                        result += line.substr (quoteEnd + 1);
                        if (hasNewline) result += '\n';
                        includeStack.pop_back ();
                    } catch (AssetLoadException&) {
                        includeStack.pop_back ();
                        // Preserve an unresolved active directive so compilation reports it.
                        result += line;
                        if (hasNewline) result += '\n';
                    }
                }
            }
            if (root && firstCode != std::string::npos) {
                if (line.compare (firstCode, 3, "#if") == 0) ++conditionalDepth;
                else if (line.compare (firstCode, 6, "#endif") == 0 && conditionalDepth > 0) --conditionalDepth;
            }
            lineStart = hasNewline ? end + 1 : source.size ();
        }
        return result;
    };
    this->m_preprocessed = expand (this->m_preprocessed, true);
    if (!deferred.empty ()) {
        // Insert before the first function, or before its enclosing conditional.
        // This mirrors the engine's global helper placement without moving
        // includes from conditional branches out of those branches.
        const size_t functionStart = firstFunctionLine (this->m_preprocessed);
        size_t insertion = functionStart == std::string::npos ? this->m_preprocessed.size () : functionStart;
        std::vector<size_t> conditionStarts;
        bool blockComment = false;
        size_t lineStart = 0;
        while (lineStart < insertion) {
            const size_t end = this->m_preprocessed.find ('\n', lineStart);
            const std::string line = this->m_preprocessed.substr (lineStart, end - lineStart);
            size_t first = std::string::npos;
            for (size_t i = 0; i < line.size (); ++i) {
                if (blockComment) {
                    if (line.compare (i, 2, "*/") == 0) { blockComment = false; ++i; }
                } else if (line.compare (i, 2, "//") == 0) break;
                else if (line.compare (i, 2, "/*") == 0) { blockComment = true; ++i; }
                else if (first == std::string::npos && !std::isspace (static_cast<unsigned char> (line[i])))
                    first = i;
            }
            if (first != std::string::npos && line.compare (first, 3, "#if") == 0)
                conditionStarts.push_back (lineStart);
            else if (first != std::string::npos && line.compare (first, 6, "#endif") == 0 && !conditionStarts.empty ())
                conditionStarts.pop_back ();
            lineStart = end == std::string::npos ? this->m_preprocessed.size () : end + 1;
        }
        if (!conditionStarts.empty ()) insertion = conditionStarts.front ();
        this->m_preprocessed.insert (insertion, deferred);
    }
}

void ShaderUnit::preprocessRequires () {
    size_t start = 0, end = 0;

    while ((start = this->m_preprocessed.find ("#require", end)) != std::string::npos) {
	const size_t lineEnd = this->m_preprocessed.find_first_of ('\n', start);

	const size_t nameStart = start + std::string ("#require ").length ();

	if (nameStart >= lineEnd) {
	    sLog.error ("Malformed #require directive (no module name) in shader ", this->m_file);
	    end = lineEnd;
	    continue;
	}

	std::string moduleName = this->m_preprocessed.substr (nameStart, lineEnd - nameStart);

	while (!moduleName.empty () && (moduleName.back () == ' ' || moduleName.back () == '\r')) {
	    moduleName.pop_back ();
	}

	if (moduleName.empty ()) {
	    sLog.error ("Malformed #require directive (empty module name) in shader ", this->m_file);
	    end = lineEnd;
	    continue;
	}

	std::string moduleCode = this->resolveRequireModule (moduleName);

	// comment out the #require directive
	this->m_preprocessed = this->m_preprocessed.replace (start, 2, "//");

	if (!moduleCode.empty ()) {
	    // insert the generated code directly into m_preprocessed at the #require location
	    // (m_includes was already consumed by preprocessIncludes, so appending there would be lost)
	    this->m_preprocessed.insert (start, moduleCode);
	    end = start + moduleCode.length ();
	} else {
	    end = lineEnd;
	}
    }
}

std::string ShaderUnit::resolveRequireModule (const std::string& moduleName) const {
    if (moduleName == "LightingV1") {
	return this->generateLightingV1 ();
    }

    sLog.error ("Unknown #require module: ", moduleName, " in shader ", this->m_file);
    return "";
}

std::string ShaderUnit::generateLightingV1 () const {
    const auto overridden = m_overrideCombos.find ("LIGHTS_POINT");
    const auto authored = m_combos.find ("LIGHTS_POINT");
    const int pointCount = overridden != m_overrideCombos.end () ? overridden->second
        : authored != m_combos.end () ? authored->second : 0;
    if (pointCount < 0 || pointCount > 15)
	throw std::invalid_argument ("LightingV1 point-light count exceeds native four-bit lightconfig range");
    // Native 140169140 generates this module from the light-count combo. Its
    // unshadowed point branch calls the shipped common_pbr_2.h helper with
    // the scene's premultiplied color/radius and world origin/exponent arrays.
    std::string result = "// begin of generated module LightingV1\n#if LIGHTING\n";
    if (pointCount > 0)
        result += "uniform vec4 g_LPoint_Color[" + std::to_string (pointCount) + "];\n"
                  "uniform vec4 g_LPoint_Origin[" + std::to_string (pointCount) + "];\n";
    result += "vec3 PerformLighting_V1(vec3 worldPos, vec3 color, vec3 normal, vec3 viewVector,\n"
              "    vec3 specularTint, vec3 ambient, float roughness, float metallic)\n"
              "{\n    vec3 light = CAST3(0.0);\n";
    for (int index = 0; index < pointCount; ++index) {
        result += "    { const int i = " + std::to_string (index) + ";\n"
                  "      vec3 lightDelta = g_LPoint_Origin[i].xyz - worldPos;\n"
                  "      light += ComputePBRLightShadow(normal, lightDelta, viewVector, color, "
                  "g_LPoint_Color[i].rgb, g_LPoint_Color[i].w, g_LPoint_Origin[i].w, "
                  "specularTint, ambient, roughness, metallic, 1.0); }\n";
    }
    result += "    return light;\n}\n#else\n"
              "vec3 PerformLighting_V1(vec3 worldPos, vec3 color, vec3 normal, vec3 viewVector,\n"
              "    vec3 specularTint, vec3 ambient, float roughness, float metallic)\n"
              "{ return vec3(0.0); }\n#endif\n// end of generated module LightingV1\n";
    return result;
}

std::string ShaderUnit::applyLinkedVaryingCompatibility (std::string source) const {
    if (this->m_type != GLSLContext::UnitType_Vertex || this->m_link == nullptr) {
	return source;
    }

    // The transform depends only on these two preprocessed units. The same
    // generic image pair is often compiled hundreds of times in one scene.
    ExactSourceCache::SourcePair cacheKey {source, this->m_link->m_preprocessed};
    if (const auto* cached = linkedVaryingCache.find (cacheKey)) return cached->first;

    // Some shipped shaders declare vec2 and vec4 forms of the same varying
    // under complementary combo branches (for example generic LIGHTMAP).
    // The GLSL preprocessor selects a matching pair; a text-level rewrite
    // would corrupt the other branch, including vec4(vec4, 0, 1).
    const auto hasBothWidths = [] (const std::string& unit, const std::string& name) {
        return std::regex_search (unit, std::regex ("\\bvarying\\s+vec2\\s+" + name + "\\s*;"))
            && std::regex_search (unit, std::regex ("\\bvarying\\s+vec4\\s+" + name + "\\s*;"));
    };
    const auto conditionalWidth = [&] (const std::string& name) {
        return hasBothWidths (source, name) || hasBothWidths (m_link->m_preprocessed, name);
    };

    std::regex fragmentVec4Varying (R"(\bvarying\s+vec4\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)");
    std::smatch varyingMatch;
    std::string linked = this->m_link->m_preprocessed;
    size_t linkedOffset = 0;

    while (std::regex_search (linked.cbegin () + linkedOffset, linked.cend (), varyingMatch, fragmentVec4Varying)) {
	const std::string name = varyingMatch[1].str ();
	linkedOffset += varyingMatch.position () + varyingMatch.length ();
	if (conditionalWidth (name)) continue;

	const std::regex vertexVec2Decl ("\\bvarying\\s+vec2\\s+" + name + "\\s*;");
	if (!std::regex_search (source, vertexVec2Decl)) {
	    continue;
	}

	source = std::regex_replace (source, vertexVec2Decl, "varying vec4 " + name + ";");

	const std::regex assignment ("(^|\\n)([ \\t]*)" + name + "\\s*=\\s*([^;\\n]+);");
	std::smatch assignmentMatch;
	size_t offset = 0;
	while (std::regex_search (source.cbegin () + offset, source.cend (), assignmentMatch, assignment)) {
	    const std::string prefix = assignmentMatch[1].str ();
	    const std::string indent = assignmentMatch[2].str ();
	    const std::string expression = assignmentMatch[3].str ();
	    const std::string replacement = prefix + indent + name + " = vec4(" + expression + ", 0.0, 1.0);";
	    const size_t position = offset + assignmentMatch.position ();
	    source.replace (position, assignmentMatch.length (), replacement);
	    offset = position + replacement.length ();
	}
    }

    // Authored HLSL-style shader pairs can expose a vec4 output to a vec2
    // fragment input. Narrow only when the vertex unit itself touches the
    // first two components; then the fragment's authored vec2 contract and
    // every vertex value it can observe remain unchanged.
    const auto fragmentTokens = shaderTokens (this->m_link->m_preprocessed);
    for (size_t i = 0; i + 3 < fragmentTokens.size (); ++i) {
        if (fragmentTokens[i].text != "varying" || fragmentTokens[i + 1].text != "vec2"
            || !fragmentTokens[i + 2].identifier || fragmentTokens[i + 3].text != ";") continue;
        const std::string name = fragmentTokens[i + 2].text;
	if (conditionalWidth (name)) continue;
        const auto vertexTokens = shaderTokens (source);
        size_t declarationType = vertexTokens.size ();
        size_t declarationName = vertexTokens.size ();
        size_t declarations = 0;
        for (size_t j = 0; j + 3 < vertexTokens.size (); ++j) {
            if (vertexTokens[j].text == "varying" && vertexTokens[j + 1].text == "vec4"
                && vertexTokens[j + 2].text == name && vertexTokens[j + 3].text == ";") {
                declarationType = j + 1;
                declarationName = j + 2;
                ++declarations;
            }
        }
        if (declarations != 1) continue;
        bool onlyFirstTwoComponents = true;
        for (size_t j = 0; j < vertexTokens.size (); ++j) {
            if (vertexTokens[j].text != name || j == declarationName) continue;
            if (j > 0 && vertexTokens[j - 1].text == ".") { onlyFirstTwoComponents = false; break; }
            if (j + 2 >= vertexTokens.size () || vertexTokens[j + 1].text != "."
                || !vertexTokens[j + 2].identifier) { onlyFirstTwoComponents = false; break; }
            for (const char component : vertexTokens[j + 2].text) {
                if (std::string_view ("xyrgst").find (component) == std::string_view::npos) {
                    onlyFirstTwoComponents = false;
                    break;
                }
            }
            if (!onlyFirstTwoComponents) break;
        }
        if (!onlyFirstTwoComponents) continue;
        source.replace (vertexTokens[declarationType].start,
                        vertexTokens[declarationType].end - vertexTokens[declarationType].start, "vec2");
        sLog.out ("Narrowed vertex varying ", name, " to linked fragment vec2 in ", this->m_file);
    }

    linkedVaryingCache.insert (std::move (cacheKey), {source, ""});
    return source;
}

std::string ShaderUnit::applyFragmentLinkedBoundsCompatibility (std::string source) const {
    if (this->m_type != GLSLContext::UnitType_Fragment || this->m_link == nullptr ||
        this->m_file.find ("foliagesway") == std::string::npos ||
        source.find ("v_Bounds") == std::string::npos ||
        source.find ("varying vec2 v_Bounds;") != std::string::npos ||
        source.find ("#if MODE == 0") == std::string::npos ||
        this->m_link->m_preprocessed.find ("varying vec2 v_Bounds;") == std::string::npos ||
        this->m_link->m_preprocessed.find ("#if MODE == 0") == std::string::npos)
        return source;
    sLog.out ("Restored linked conditional bounds varying in ", this->m_file);
    return "#if MODE == 0\nvarying vec2 v_Bounds;\n#endif\n" + source;
}

std::string ShaderUnit::applyFragmentShadowMaskCompatibility (std::string source) const {
    if (this->m_type != GLSLContext::UnitType_Fragment ||
        this->m_file.find ("workshop/2821337237/effects/shadow_map") == std::string::npos ||
        source.find ("#if SHADOWMASK != 0") == std::string::npos)
        return source;
    const auto tokens = shaderTokens (source);
    for (size_t i = 0; i + 4 < tokens.size (); ++i) {
        if (tokens[i].text != "vec3" || tokens[i + 1].text != "ShadowMask" || tokens[i + 2].text != "(")
            continue;
        size_t open = i + 2;
        int parentheses = 0;
        for (; open < tokens.size (); ++open) {
            if (tokens[open].text == "(") ++parentheses;
            else if (tokens[open].text == ")" && --parentheses == 0) { ++open; break; }
        }
        if (open == tokens.size () || tokens[open].text != "{") continue;
        int braces = 1;
        size_t close = open + 1;
        for (; close < tokens.size (); ++close) {
            if (tokens[close].text == "{") ++braces;
            else if (tokens[close].text == "}" && --braces == 0) break;
        }
        if (close == tokens.size ()) continue;
        source.insert (tokens[close].end, "\n#endif\n");
        source.insert (tokens[i].start, "#if SHADOWMASK != 0\n");
        sLog.out ("Guarded unused shadow mask helper in ", this->m_file);
        return source;
    }
    return source;
}

std::string ShaderUnit::applyFragmentWritableInputCompatibility (std::string source) const {
    if (this->m_type != GLSLContext::UnitType_Fragment || source.find ("_weMutableTexCoord") != std::string::npos)
        return source;

    const auto tokens = shaderTokens (source);
    size_t bodyOpen = tokens.size ();
    size_t bodyClose = tokens.size ();
    for (size_t i = 0; i + 4 < tokens.size (); ++i) {
        if (tokens[i].text != "void" || tokens[i + 1].text != "main" || tokens[i + 2].text != "(") continue;
        int parentheses = 1;
        size_t next = i + 3;
        for (; next < tokens.size (); ++next) {
            if (tokens[next].text == "(") ++parentheses;
            else if (tokens[next].text == ")" && --parentheses == 0) { ++next; break; }
        }
        if (next < tokens.size () && tokens[next].text == "{") { bodyOpen = next; break; }
    }
    if (bodyOpen == tokens.size ()) return source;
    int braces = 1;
    for (size_t i = bodyOpen + 1; i < tokens.size (); ++i) {
        if (tokens[i].text == "{") ++braces;
        else if (tokens[i].text == "}" && --braces == 0) { bodyClose = i; break; }
    }
    if (bodyClose == tokens.size ()) return source;

    size_t declarationCount = 0;
    bool writesInput = false;
    for (size_t i = 0; i < tokens.size (); ++i) {
        if (tokens[i].text != "v_TexCoord") continue;
        if (i <= bodyOpen || i >= bodyClose) {
            if (i >= 2 && i + 1 < tokens.size () && tokens[i - 2].text == "varying" &&
                tokens[i - 1].text == "vec2" && tokens[i + 1].text == ";") ++declarationCount;
            else return source; // A helper outside main may need the original global input.
            continue;
        }
        if (i > 0 && (tokens[i - 1].text == "vec2" || tokens[i - 1].text == "vec4"))
            return source; // An authored main-local shadow already makes writes legal.
        if (i > 0 && tokens[i - 1].text == ".")
            return source; // A struct member with this name is a different object.
        size_t next = i + 1;
        if (next + 1 < tokens.size () && tokens[next].text == "." && tokens[next + 1].identifier)
            next += 2;
        if (next < tokens.size () && tokens[next].text == "=" &&
            (next + 1 == tokens.size () || tokens[next + 1].text != "=")) writesInput = true;
        if (next + 1 < tokens.size () &&
            ((tokens[next].text == "+" || tokens[next].text == "-" ||
              tokens[next].text == "*" || tokens[next].text == "/") && tokens[next + 1].text == "="))
            writesInput = true;
        if (next + 1 < tokens.size () &&
            ((tokens[next].text == "+" && tokens[next + 1].text == "+") ||
             (tokens[next].text == "-" && tokens[next + 1].text == "-"))) writesInput = true;
    }
    if (declarationCount != 1 || !writesInput) return source;

    std::string lowered = source.substr (0, tokens[bodyOpen].end);
    lowered += "\nvec2 _weMutableTexCoord = v_TexCoord.xy;\n";
    size_t consumed = tokens[bodyOpen].end;
    for (size_t i = bodyOpen + 1; i < bodyClose; ++i) {
        if (tokens[i].text != "v_TexCoord") continue;
        lowered += source.substr (consumed, tokens[i].start - consumed);
        lowered += "_weMutableTexCoord";
        consumed = tokens[i].end;
    }
    lowered += source.substr (consumed);
    sLog.out ("Lowered writable fragment texture coordinates in ", this->m_file);
    return lowered;
}

std::string ShaderUnit::applySineWaveOpacityCompatibility (std::string source) const {
    if (this->m_type != GLSLContext::UnitType_Fragment ||
        this->m_file.find ("workshop/2402621362/effects/sine_wave") == std::string::npos ||
        source.find ("vec2 waveCoord = ") == std::string::npos ||
        source.find ("waveCoord = pow(") == std::string::npos)
        return source;
    const std::string authored = "u_WaveOpacity * waveCoord)";
    const size_t call = source.find (authored);
    if (call == std::string::npos) return source;
    source.replace (call, authored.size (), "u_WaveOpacity * waveCoord.x)");
    sLog.out ("Selected scalar opacity from wave coordinate in ", this->m_file);
    return source;
}

std::string ShaderUnit::applySampledLocalConstCompatibility (std::string source) const {
    if (this->m_type != GLSLContext::UnitType_Fragment) return source;
    const auto tokens = shaderTokens (source);
    std::vector<size_t> qualifiers;
    int depth = 0;
    for (size_t i = 0; i < tokens.size (); ++i) {
        if (tokens[i].text == "{") { ++depth; continue; }
        if (tokens[i].text == "}") { --depth; continue; }
        if (depth <= 0 || tokens[i].text != "const" || i + 4 >= tokens.size () ||
            tokens[i + 1].text != "float" || !tokens[i + 2].identifier ||
            tokens[i + 3].text != "=") continue;
        bool sampled = false;
        bool multipleDeclarators = false;
        int parentheses = 0;
        for (size_t j = i + 4; j < tokens.size () && tokens[j].text != ";"; ++j) {
            if (tokens[j].text == "(") ++parentheses;
            else if (tokens[j].text == ")") --parentheses;
            else if (tokens[j].text == "," && parentheses == 0) multipleDeclarators = true;
            if (tokens[j].text == "texSample2D" || tokens[j].text == "texture") sampled = true;
        }
        if (sampled && !multipleDeclarators) qualifiers.push_back (tokens[i].start);
    }
    for (auto it = qualifiers.rbegin (); it != qualifiers.rend (); ++it)
        source.erase (*it, std::string ("const").size ());
    if (!qualifiers.empty ()) sLog.out ("Lowered sampled local const in ", this->m_file);
    return source;
}

std::string ShaderUnit::applyVertexPointerUVCompatibility (std::string source) const {
    if (this->m_type != GLSLContext::UnitType_Vertex) return source;
    const auto tokens = shaderTokens (source);
    bool widePointer = false;
    bool vectorScale = false;
    bool vectorMultiplier = false;
    for (size_t i = 0; i + 3 < tokens.size (); ++i) {
        if (tokens[i].text == "varying" && tokens[i + 1].text == "vec4" &&
            tokens[i + 2].text == "v_PointerUV" && tokens[i + 3].text == ";") widePointer = true;
        if (tokens[i].text == "uniform" && tokens[i + 1].text == "vec2" &&
            tokens[i + 2].text == "g_Scale" && tokens[i + 3].text == ";") vectorScale = true;
        if (tokens[i].text == "uniform" && tokens[i + 1].text == "vec2" &&
            tokens[i + 2].text == "g_Scale_Multiplier" && tokens[i + 3].text == ";") vectorMultiplier = true;
    }
    if (!widePointer || !vectorScale || !vectorMultiplier) return source;
    for (size_t i = 0; i + 11 < tokens.size (); ++i) {
        if (tokens[i].text == "vec2" && tokens[i + 1].text == "da" &&
            tokens[i + 2].text == "=" && tokens[i + 3].text == "v_PointerUV" &&
            tokens[i + 4].text == "*" && tokens[i + 5].text == "(" &&
            tokens[i + 6].text == "g_Scale" && tokens[i + 7].text == "*" &&
            tokens[i + 8].text == "g_Scale_Multiplier" && tokens[i + 9].text == ")" &&
            tokens[i + 10].text == "*") {
            const size_t scalarStart = tokens[i + 11].start;
            const size_t semicolon = source.find (';', scalarStart);
            if (semicolon == std::string::npos ||
                source.substr (scalarStart, semicolon - scalarStart) != "0.001") continue;
            source.insert (tokens[i + 3].end, ".xy");
            sLog.out ("Selected pointer UV.xy for vec2 iris offset in ", this->m_file);
            break;
        }
    }
    return source;
}

std::string ShaderUnit::applyFragmentTexCoordCompatibility (std::string source) const {
    if (this->m_type != GLSLContext::UnitType_Fragment) return source;
    const auto tokens = shaderTokens (source);
    bool wideCoordinate = false;
    for (size_t i = 0; i + 3 < tokens.size (); ++i) {
        if (tokens[i].text == "varying" &&
            (tokens[i + 1].text == "vec3" || tokens[i + 1].text == "vec4") &&
            tokens[i + 2].text == "v_TexCoord" && tokens[i + 3].text == ";") {
            wideCoordinate = true;
            break;
        }
    }
    if (!wideCoordinate) return source;

    const auto arithmetic = [](const std::string& op) {
        return op == "+" || op == "-" || op == "*" || op == "/";
    };
    std::vector<size_t> closingParenthesis (tokens.size (), tokens.size ());
    std::vector<size_t> openParentheses;
    for (size_t i = 0; i < tokens.size (); ++i) {
        if (tokens[i].text == "(") openParentheses.push_back (i);
        else if (tokens[i].text == ")" && !openParentheses.empty ()) {
            closingParenthesis[openParentheses.back ()] = i;
            openParentheses.pop_back ();
        }
    }
    std::vector<size_t> insertions;
    // HLSL truncates a wider coordinate when an arithmetic expression is
    // assigned to a float2.  GLSL instead rejects the first mixed-width
    // operator.  Limit this conversion to a vec2 initializer whose bare
    // coordinate is used in arithmetic, outside calls and swizzles.  A call
    // may consume all four lanes, even when its result is assigned to vec2.
    for (size_t i = 0; i + 3 < tokens.size (); ++i) {
        if (tokens[i].text != "vec2" || !tokens[i + 1].identifier ||
            tokens[i + 2].text != "=") continue;
        size_t end = i + 3;
        int depth = 0;
        bool callable = false;
        bool wideConstructor = false;
        bool arithmeticExpression = false;
        std::vector<size_t> coordinates;
        for (; end < tokens.size (); ++end) {
            const auto& token = tokens[end];
            if (token.text == ";" && depth == 0) break;
            if (token.text == "(") {
                if (end > i + 3 && tokens[end - 1].identifier &&
                    tokens[end - 1].text != "CAST2") callable = true;
                ++depth;
            } else if (token.text == ")") {
                if (--depth < 0) break;
            }
            if (token.text == "vec3" || token.text == "vec4" ||
                token.text == "CAST3" || token.text == "CAST4") wideConstructor = true;
            if (arithmetic (token.text)) arithmeticExpression = true;
            if (token.text == "v_TexCoord" &&
                (end == i + 3 || tokens[end - 1].text != ".") &&
                (end + 1 == tokens.size () ||
                 (tokens[end + 1].text != "." && tokens[end + 1].text != "[")))
                coordinates.push_back (token.end);
        }
        if (end < tokens.size () && tokens[end].text == ";" && depth == 0 &&
            arithmeticExpression && !callable && !wideConstructor)
            insertions.insert (insertions.end (), coordinates.begin (), coordinates.end ());
        i = end;
    }
    for (size_t i = 0; i < tokens.size (); ++i) {
        // v_TexCoord * CAST2(...) is an authored vec2 operation. Do not
        // change an already-swizzled coordinate or a struct member.
        if (i + 3 < tokens.size () && tokens[i].text == "v_TexCoord" &&
            (i == 0 || tokens[i - 1].text != ".") && arithmetic (tokens[i + 1].text) &&
            tokens[i + 2].text == "CAST2" && tokens[i + 3].text == "(")
            insertions.push_back (tokens[i].end);

        // CAST2(expr) * v_TexCoord permits nested parentheses in expr.
        if (tokens[i].text != "CAST2" || i + 1 >= tokens.size () || tokens[i + 1].text != "(")
            continue;
        const size_t close = closingParenthesis[i + 1];
        if (close + 2 < tokens.size () && arithmetic (tokens[close + 1].text) &&
            tokens[close + 2].text == "v_TexCoord" &&
            (close + 3 == tokens.size () || (tokens[close + 3].text != "." &&
                                            tokens[close + 3].text != "[")))
            insertions.push_back (tokens[close + 2].end);
    }
    std::sort (insertions.begin (), insertions.end ());
    insertions.erase (std::unique (insertions.begin (), insertions.end ()), insertions.end ());
    if (insertions.empty ()) return source;
    std::string rewritten;
    rewritten.reserve (source.size () + 3 * insertions.size ());
    size_t consumed = 0;
    for (const size_t insertion : insertions) {
        rewritten.append (source, consumed, insertion - consumed);
        rewritten += ".xy";
        consumed = insertion;
    }
    rewritten.append (source, consumed, std::string::npos);
    sLog.out ("Applied fragment TexCoord vec2 compatibility in ", this->m_file);
    return rewritten;
}

void ShaderUnit::parseComboConfiguration (const std::string& content, const int defaultValue) {
    // Native 14016ce60 stores only combo/default for runtime compilation.
    // Authored `require` controls editor visibility; it must not gate an
    // explicitly selected runtime permutation.
    JSON data;
    try {
	data = WallpaperEngine::Data::JSON::parseAuthoringJson (content, this->m_file + ":combo");
    } catch (const std::exception& e) {
	sLog.error ("Cannot parse combo metadata in shader ", this->m_file, ": ", e.what ());
	return;
    }
    const auto combo = data.require<std::string> ("combo", "cannot parse combo information");
    // ignore type as it seems to be used only on the editor
    // const auto type = data.find ("type");
    const auto defvalue = data.find ("default");

    // check the combos
    const auto entry = this->m_combos.find (combo);
    const auto entryOverride = this->m_overrideCombos.find (combo);

    // add the combo to the found list
    this->m_usedCombos.emplace (combo, true);

    // if the combo was not found in the predefined values this means that the default value in the JSON data can be
    // used so only define the ones that are not already defined
    if (entry == this->m_combos.end () && entryOverride == this->m_overrideCombos.end ()) {
	// if no combo is defined just load the default settings
	if (defvalue == data.end ()) {
	    // TODO: PROPERLY SUPPORT EMPTY COMBOS
	    this->m_discoveredCombos.emplace (combo, defaultValue);
	} else if (defvalue->is_number_float ()) {
	    sLog.exception ("float combos are not supported in shader ", this->m_file, ". ", combo);
	} else if (defvalue->is_number_integer ()) {
	    this->m_discoveredCombos.emplace (combo, defvalue->get<int> ());
	} else if (defvalue->is_string ()) {
	    sLog.exception ("string combos are not supported in shader ", this->m_file, ". ", combo);
	} else {
	    sLog.exception ("cannot parse combo information ", combo, ". unknown type for ", defvalue->dump ());
	}
    }
}

void ShaderUnit::parseParameterConfiguration (
    const std::string& type, const std::string& name, const std::string& content
) {
    JSON data;
    try {
	data = WallpaperEngine::Data::JSON::parseAuthoringJson (content, this->m_file + ":parameter:" + name);
    } catch (const std::exception& e) {
	sLog.error ("Cannot parse parameter metadata for ", name, " in shader ", this->m_file, ": ", e.what ());
	return;
    }
    const auto material = data.optional ("material");
    const auto defvalue = data.optional ("default");
    // auto range = data.find ("range");
    const auto combo = data.find ("combo");

    // Override constants take precedence over the pass's authored constants.
    const UserSetting* constant = nullptr;
    if (material.has_value ()) {
	if (const auto override = this->m_constants.find (*material); override != this->m_constants.end ())
	    constant = override->second.get ();
	else if (this->m_baseConstants != nullptr) {
	    if (const auto base = this->m_baseConstants->find (*material); base != this->m_baseConstants->end ())
		constant = base->second.get ();
	}
    }

    const DynamicValue* constantValue = constant != nullptr && constant->value != nullptr
	? constant->value.get () : nullptr;

    if (constantValue == nullptr && !defvalue.has_value ()) {
	if (type != "sampler2D") {
	    sLog.exception ("Cannot parse parameter data for ", name, " in shader ", this->m_file);
	}
    }

    Variables::ShaderVariable* parameter = nullptr;

    if (type == "vec4") {
	if (!defvalue.has_value ()) {
	    if (constantValue->getType () != DynamicValue::UnderlyingType::Vec4)
		throw std::invalid_argument ("Expected vec4 material constant for " + name + " in shader " + this->m_file);
	    parameter = new Variables::ShaderVariableVector4 (constantValue->getVec4 ());
	} else {
	    parameter = new Variables::ShaderVariableVector4 (VectorBuilder::parse<glm::vec4> (defvalue->get<std::string> ()));
	}
    } else if (type == "vec3") {
	if (!defvalue.has_value ()) {
	    if (constantValue->getType () != DynamicValue::UnderlyingType::Vec3)
		throw std::invalid_argument ("Expected vec3 material constant for " + name + " in shader " + this->m_file);
	    parameter = new Variables::ShaderVariableVector3 (constantValue->getVec3 ());
	} else {
	    parameter = new Variables::ShaderVariableVector3 (VectorBuilder::parse<glm::vec3> (*defvalue));
	}
    } else if (type == "vec2") {
	if (!defvalue.has_value ()) {
	    if (constantValue->getType () != DynamicValue::UnderlyingType::Vec2)
		throw std::invalid_argument ("Expected vec2 material constant for " + name + " in shader " + this->m_file);
	    parameter = new Variables::ShaderVariableVector2 (constantValue->getVec2 ());
	} else {
	    parameter = new Variables::ShaderVariableVector2 (VectorBuilder::parse<glm::vec2> (*defvalue));
	}
    } else if (type == "float") {
	float value = 0.0f;
	if (!defvalue.has_value ()) {
	    if (constantValue->getType () != DynamicValue::UnderlyingType::Float)
		throw std::invalid_argument ("Expected float material constant for " + name + " in shader " + this->m_file);
	    value = constantValue->getFloat ();
	} else if (defvalue->is_string ()) {
	    std::string token = defvalue->get<std::string> ();
	    const auto first = token.find_first_not_of (" \t\r\n");
	    if (first == std::string::npos)
		throw std::invalid_argument ("Empty float default for " + name + " in shader " + this->m_file);
	    const auto last = token.find_last_not_of (" \t\r\n");
	    token = token.substr (first, last - first + 1);
	    try {
		size_t consumed = 0;
		value = std::stof (token, &consumed);
		if (consumed != token.size ())
		    throw std::invalid_argument ("Float default contains trailing data");
	    } catch (const std::invalid_argument&) {
		throw std::invalid_argument ("Invalid float default for " + name + " in shader " + this->m_file);
	    } catch (const std::out_of_range&) {
		throw std::invalid_argument ("Float default is out of range for " + name + " in shader " + this->m_file);
	    }
	} else if (defvalue->is_number ()) {
	    value = defvalue->get<float> ();
	} else {
	    throw std::invalid_argument ("Float default must be numeric for " + name + " in shader " + this->m_file);
	}
	if (!std::isfinite (value))
	    throw std::invalid_argument ("Float default must be finite for " + name + " in shader " + this->m_file);
	parameter = new Variables::ShaderVariableFloat (value);
    } else if (type == "int") {
	if (!defvalue.has_value ()) {
	    if (constantValue->getType () != DynamicValue::UnderlyingType::Int)
		throw std::invalid_argument ("Expected int material constant for " + name + " in shader " + this->m_file);
	    parameter = new Variables::ShaderVariableInteger (constantValue->getInt ());
	} else if (defvalue->is_string ()) {
	    parameter = new Variables::ShaderVariableInteger (std::stoi (defvalue->get<std::string> ()));
	} else {
	    parameter = new Variables::ShaderVariableInteger (defvalue->get<int> ());
	}
    } else if (type == "sampler2D" || type == "sampler2DComparison") {
	// samplers can have special requirements, check what sampler we're working with and create definitions
	// if needed
	const auto textureName = data.find ("default");
	// TODO: CREATE TEXTURE WITH THE GIVEN COLOR
	// extract the texture number from the name
	const char value = name.at (std::string ("g_Texture").length ());
	const auto requireany = data.find ("requireany");
	const auto require = data.find ("require");
	// now convert it to integer
	// TODO: BETTER CONVERSION HERE
	size_t index = value - '0';
	// TODO: SUPPORT USER TEXTURES!!

	if (combo != data.end ()) {
	    // TODO: CLEANUP HOW THIS IS DETERMINED FIRST
	    // if the texture exists (and is not null), add to the combo
	    const auto textureSlotUsed
		= this->m_passTextures.contains (index) || this->m_overrideTextures.contains (index);
	    const auto effectiveCombo = [this] (const std::string& macro) -> std::optional<int> {
		if (const auto override = this->m_overrideCombos.find (macro); override != this->m_overrideCombos.end ())
		    return override->second;
		if (const auto authored = this->m_combos.find (macro); authored != this->m_combos.end ())
		    return authored->second;
		return std::nullopt;
	    };
	    bool isRequired = false;
	    int comboValue = 1;

	    if (textureSlotUsed) {
		// nothing extra to do, the texture exists, the combo must be set
		// these tend to not have default value
		isRequired = true;
	    } else if (require != data.end ()) {
		// this is required based on certain conditions
		if (requireany != data.end () && requireany->get<bool> ()) {
		    // any of the values set are valid, check for them
		    for (const auto& item : require->items ()) {
			const std::string& macro = item.key ();
			const auto value = effectiveCombo (macro);

			// Preserve existing dependency gating while reading the effective combo safely.
			if (!value.has_value () || *value != item.value ()) {
			    isRequired = true;
			    break;
			}
		    }
		} else {
		    isRequired = true;

		    // all values must match for it to be required
		    for (const auto& item : require->items ()) {
			const std::string& macro = item.key ();
			const auto value = effectiveCombo (macro);

			// Preserve existing dependency gating while reading the effective combo safely.
			if (value.has_value () && *value == item.value ()) {
			    isRequired = false;
			    break;
			}
		    }
		}
	    }

	    if (isRequired && !textureSlotUsed) {
		if (!defvalue.has_value ()) {
		    isRequired = false;
		} else {
		    // is the combo registered already?
		    // if not, add it with the default value
		    // there's already a combo providing this value, so it doesn't need to be added
		    if (this->m_combos.contains (*combo) || this->m_overrideCombos.contains (*combo)) {
			isRequired = false;
			// otherwise a default value must be used
		    } else if (defvalue->is_string ()) {
			comboValue = std::stoi (defvalue->get<std::string> ().c_str ());
		    } else if (defvalue->is_number ()) {
			comboValue = *defvalue;
		    } else {
			sLog.exception (
			    "Cannot determine default value for combo ", combo->get<std::string> (),
			    " because it's not specified by the shader and is not given a default value: ", this->m_file
			);
		    }
		}
	    }

	    if (isRequired) {
		// add the new combo to the list
		this->m_discoveredCombos.emplace (*combo, comboValue);
		// textures linked to combos need to be tracked too
		this->m_usedCombos.emplace (*combo, true);
	    }
	}

	if (textureName != data.end ()) {
	    this->m_defaultTextures.emplace (index, *textureName);
	}

	// samplers are not saved, we can ignore them for now
	return;
    } else {
	sLog.error ("Unknown parameter type: ", type, " for ", name, " in shader ", this->m_file);
	return;
    }

    if (material.has_value () && parameter != nullptr) {
	parameter->setIdentifierName (*material);
	parameter->setName (name);
	parameter->setPosition (data.optional<bool> ("position", false));

	this->m_parameters.push_back (parameter);
    }
}

const ComboMap& ShaderUnit::getCombos () const { return this->m_combos; }

const ComboMap& ShaderUnit::getDiscoveredCombos () const { return this->m_discoveredCombos; }

void ShaderUnit::linkToUnit (const ShaderUnit* unit) { this->m_link = unit; }

const ShaderUnit* ShaderUnit::getLinkedUnit () const { return this->m_link; }

const std::string& ShaderUnit::compile () {
    if (!this->m_final.empty ()) {
	return this->m_final;
    }

    this->m_final = SHADER_HEADER (this->m_file);

    if (this->m_type == GLSLContext::UnitType_Fragment) {
	this->m_final += FRAGMENT_SHADER_DEFINES;
    } else {
	this->m_final += VERTEX_SHADER_DEFINES;
    }

    std::map<std::string, bool> addedCombos;

    for (const auto& [name, value] : this->m_overrideCombos) {
	std::string uppercase;
	std::ranges::transform (name, std::back_inserter (uppercase), ::toupper);

	if (!addedCombos.contains (uppercase)) {
	    this->m_final += DEFINE_COMBO (uppercase, value);
	    addedCombos.emplace (uppercase, true);
	}
    }

    // now add all the combos to the source
    for (const auto& [name, value] : this->m_combos) {
	std::string uppercase;
	std::ranges::transform (name, std::back_inserter (uppercase), ::toupper);

	if (!addedCombos.contains (uppercase)) {
	    this->m_final += DEFINE_COMBO (uppercase, value);
	    addedCombos.emplace (uppercase, true);
	}
    }

    for (const auto& [name, value] : this->m_discoveredCombos) {
	std::string uppercase;
	std::ranges::transform (name, std::back_inserter (uppercase), ::toupper);

	if (!addedCombos.contains (uppercase)) {
	    this->m_final += DEFINE_COMBO (uppercase, value);
	    addedCombos.emplace (uppercase, true);
	}
    }

    if (this->m_link != nullptr) {
	for (const auto& [name, value] : this->m_link->getCombos ()) {
	    std::string uppercase;
	    std::ranges::transform (name, std::back_inserter (uppercase), ::toupper);

	    if (!addedCombos.contains (uppercase)) {
		this->m_final += DEFINE_COMBO (uppercase, value);
		addedCombos.emplace (uppercase, true);
	    }
	}

	for (const auto& [name, value] : this->m_link->getDiscoveredCombos ()) {
	    std::string uppercase;
	    std::ranges::transform (name, std::back_inserter (uppercase), ::toupper);

	    if (!addedCombos.contains (uppercase)) {
		this->m_final += DEFINE_COMBO (uppercase, value);
		addedCombos.emplace (uppercase, true);
	    }
	}
    }

    // this should be the rest of the shader
    std::string shaderSource = this->applyLinkedVaryingCompatibility (this->m_preprocessed);
    shaderSource = this->applyFragmentLinkedBoundsCompatibility (std::move (shaderSource));
    shaderSource = this->applyFragmentShadowMaskCompatibility (std::move (shaderSource));
    shaderSource = this->applyFragmentWritableInputCompatibility (std::move (shaderSource));
    shaderSource = this->applySineWaveOpacityCompatibility (std::move (shaderSource));
    shaderSource = this->applyFragmentTexCoordCompatibility (std::move (shaderSource));
    shaderSource = this->applyVertexPointerUVCompatibility (std::move (shaderSource));
    shaderSource = this->applySampledLocalConstCompatibility (std::move (shaderSource));
    this->m_final += shaderSource;

    // the pass itself handles shader compilation, the unit doesn't have enough information for this step
    return this->m_final;
}

const std::vector<Variables::ShaderVariable*>& ShaderUnit::getParameters () const { return this->m_parameters; }
const TextureMap& ShaderUnit::getTextures () const { return this->m_defaultTextures; }
