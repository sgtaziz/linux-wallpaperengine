#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace WallpaperEngine::Render::Wallpapers {

// Native 14018b2c0 scans the mutable main list for the first image, including
// hidden images. It uses loaded logical dimensions, then resets that image's
// origin before SceneScript runs. Visibility and authored bounds do not enter.
template<typename Range, typename ImageAccessor>
auto firstAutomaticProjectionImage (const Range& order, ImageAccessor image) {
    using Result = decltype (image (*order.begin ()));
    for (auto* object : order)
        if (auto* candidate = image (object)) return candidate;
    return Result (nullptr);
}

inline glm::vec3 automaticProjectionImageOrigin (glm::vec2 logicalSize) {
    return {logicalSize.x * 0.5f, logicalSize.y * 0.5f, 0.0f};
}

} // namespace WallpaperEngine::Render::Wallpapers
