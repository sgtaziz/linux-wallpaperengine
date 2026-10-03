#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace WallpaperEngine::Render::Shaders {

// Native particle atlases sample one window per frame. The shipped vertex
// shader derives windows from a uniform grid (ComputeSpriteFrame), which
// cannot express per-frame TEXS rectangles, so Linux computes each vertex's
// current/next frame UVs and blend factor on the CPU and the sprite vertex
// stream carries them. Returns nullopt when the shipped source differs.
inline std::optional<std::string> particleSpriteCpuUVSource (const std::string& source) {
    constexpr std::string_view nativeFrame =
        "ComputeSpriteFrame(frac(in_ParticleLifeTime), v_TexCoord, uvOffsets, v_TexCoordBlend);";
    constexpr std::string_view nativeMask = "uvOffsets *= step(0.001, a_TexCoordVec4.xy);";
    constexpr std::string_view nativeOffset = "v_TexCoord += uvOffsets.xyxy;";
    std::string patched = source;
    const auto replaceOnce = [&patched] (std::string_view native, std::string_view cpu) {
        const size_t at = patched.find (native);
        if (at == std::string::npos || patched.find (native, at + 1) != std::string::npos) return false;
        patched.replace (at, native.size (), cpu);
        return true;
    };
    if (!replaceOnce (nativeFrame, "v_TexCoord = a_CpuSpriteUV; v_TexCoordBlend = a_CpuSpriteBlend;")
        || !replaceOnce (nativeMask, "") || !replaceOnce (nativeOffset, ""))
        return std::nullopt;
    return "attribute vec4 a_CpuSpriteUV;\nattribute float a_CpuSpriteBlend;\n" + patched;
}

} // namespace WallpaperEngine::Render::Shaders
