#pragma once

#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace WallpaperEngine::Render::Shaders {

// genericparticle.frag adds the screen refraction offset to the framebuffer
// texcoord. Native's HLSL path adds it unchanged: D3D V and the offset's Y
// both grow toward the screen bottom. The shipped GLSL-only branch
//   #ifndef HLSL
//   	screenRefractionOffset.y = -screenRefractionOffset.y;
//   #endif
// converts that for bottom-up GL targets. Linux scene targets are top-down
// (the final blit inverts them), so their texcoord Y already matches native's
// V. With the pass's g_ViewUp reflected like the rest of Linux's Y-reflected
// particle basis, dropping the GL-only negation reproduces native's offsets
// for every sprite rotation. Returns nullopt when the shipped source differs.
inline std::optional<std::string> particleTopDownRefractionSource (const std::string& source) {
    constexpr std::string_view negation = "screenRefractionOffset.y = -screenRefractionOffset.y;";
    const size_t at = source.find (negation);
    if (at == std::string::npos || source.find (negation, at + 1) != std::string::npos)
        return std::nullopt;
    // The statement must be the whole body line directly inside `#ifndef HLSL`.
    const size_t lineStart = source.rfind ('\n', at);
    if (lineStart == std::string::npos) return std::nullopt;
    for (size_t i = lineStart + 1; i < at; ++i)
        if (!std::isspace (static_cast<unsigned char> (source[i]))) return std::nullopt;
    const size_t previousEnd = lineStart;
    const size_t previousStart = source.rfind ('\n', previousEnd == 0 ? 0 : previousEnd - 1);
    std::string directive = source.substr (
        previousStart == std::string::npos ? 0 : previousStart + 1,
        previousEnd - (previousStart == std::string::npos ? 0 : previousStart + 1));
    std::erase_if (directive, [] (unsigned char c) { return std::isspace (c); });
    if (directive != "#ifndefHLSL") return std::nullopt;
    std::string patched = source;
    patched.erase (at, negation.size ());
    return patched;
}

} // namespace WallpaperEngine::Render::Shaders
