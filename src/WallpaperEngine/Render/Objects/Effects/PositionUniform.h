#pragma once

#include <glm/vec2.hpp>

namespace WallpaperEngine::Render::Objects::Effects {
// Authored position coordinates use top-left UVs. Map their vertical component
// through the actual pass quad endpoints, including a cropped first-copy UV.
inline glm::vec2 positionUniformForPass (glm::vec2 authored, float topV, float bottomV) {
    authored.y = topV + (bottomV - topV) * authored.y;
    return authored;
}
} // namespace WallpaperEngine::Render::Objects::Effects
