#pragma once

#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <string>

namespace WallpaperEngine::Render::Shaders {

// The native rope geometry shader interpolates one segment's UVs across its
// generated curve. Linux expands that curve on the CPU, so the particle vertex
// stream carries already interpolated UV endpoints in one additional vec2.
// Apply to CPU-expanded ordinary ropes and rope trails.
inline std::string particleRopeCpuUVSource (std::string source) {
    constexpr char nativeAssignment[] =
        "v_TexCoord.y = mix(uvMinimum, uvMinimum + uvDelta, uvs.y);";
    constexpr char cpuAssignment[] =
        "v_TexCoord.y = mix(a_CpuRopeUV.x, a_CpuRopeUV.y, uvs.y);";
    const size_t position = source.find (nativeAssignment);
    if (position == std::string::npos || source.find (nativeAssignment, position + 1) != std::string::npos)
        throw std::runtime_error ("Rope CPU UV source contract changed");
    source.replace (position, sizeof (nativeAssignment) - 1, cpuAssignment);
    const auto replaceOnce = [&source] (const char* native, const char* cpu) {
        const size_t at = source.find (native);
        if (at == std::string::npos || source.find (native, at + 1) != std::string::npos)
            throw std::runtime_error ("Rope CPU right source contract changed");
        source.replace (at, std::char_traits<char>::length (native), cpu);
    };
    replaceOnce (
        "vec3 eyeDirection = mul(g_OrientationForward, CAST3X3(g_ModelMatrixInverse));",
        "vec3 eyeDirection = a_CpuRopeEyeDirection;");
    replaceOnce (
        "vec3 trailRightStart = cross(eyeDirection, trailDelta + CPStart);",
        "vec3 trailRightStart = a_CpuRopeRightStart;");
    replaceOnce (
        "vec3 trailRightEnd = cross(eyeDirection, trailDelta - CPEnd);",
        "vec3 trailRightEnd = a_CpuRopeRightEnd;");
    replaceOnce ("trailRightStart = normalize(trailRightStart) * sizeStart;", "");
    replaceOnce ("trailRightEnd = normalize(trailRightEnd) * sizeEnd;", "");
    // The shipped no-geometry fallback has a different centering expression.
    // CPU expansion models the native geometry shader's start/right pair,
    // which emits position - right and position + right around the centerline.
    replaceOnce ("position += right * uvs.x * 2.0 - 1.0;",
                 "position += right * (uvs.x * 2.0 - 1.0);");
    // Native geometry fades segment endpoints once, before inserting interior
    // vertices. The CPU-expanded stream already carries those faded values;
    // repeated shader fading would restart the curve on every subsegment.
    for (const char* fade : { "colorStart.a *= trailFadeIn;",
                              "colorEnd.a *= trailFadeOut;",
                              "sizeStart.w *= trailFadeIn;",
                              "sizeEnd.w *= trailFadeOut;" }) {
        const size_t fadePosition = source.find (fade);
        if (fadePosition == std::string::npos || source.find (fade, fadePosition + 1) != std::string::npos)
            throw std::runtime_error ("Rope-trail CPU fade source contract changed");
        source.replace (fadePosition, std::char_traits<char>::length (fade), "");
    }
    return "attribute vec2 a_CpuRopeUV;\n"
           "attribute vec3 a_CpuRopeRightStart;\n"
           "attribute vec3 a_CpuRopeRightEnd;\n"
           "attribute vec3 a_CpuRopeEyeDirection;\n" + source;
}

} // namespace WallpaperEngine::Render::Shaders
