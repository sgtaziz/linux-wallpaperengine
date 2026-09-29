#pragma once

#include <glm/vec4.hpp>

namespace WallpaperEngine::Render::Objects {

// Native v2.8.42 image draw paths 140208670 and 140207740 multiply the
// device-color RGB by image brightness only when the HDR flag is active.
// Keep alpha separate so brightness never changes transparency.
inline glm::vec4 imageDeviceColor (glm::vec4 color, float alpha, float brightness, bool hdr) {
    const float gain = hdr ? brightness : 1.0f;
    color.r *= gain;
    color.g *= gain;
    color.b *= gain;
    color.a *= alpha;
    return color;
}

} // namespace WallpaperEngine::Render::Objects
