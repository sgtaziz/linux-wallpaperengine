#pragma once

#include <glm/vec4.hpp>

namespace WallpaperEngine::Render::Objects {

// Native image draw paths multiply the
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
