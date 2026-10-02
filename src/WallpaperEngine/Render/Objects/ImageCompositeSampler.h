#pragma once

#include "WallpaperEngine/Data/Assets/Texture.h"

namespace WallpaperEngine::Render::Objects {
// Native public image-composite targets use edge addressing independently
// of their source TEX sampler. Preserve other source properties, notably the
// nearest filter, without carrying repeat/border addressing onto A/B.
inline uint32_t imageCompositeTextureFlags (uint32_t sourceFlags) {
    using namespace WallpaperEngine::Data::Assets;
    return (sourceFlags & ~TextureFlags_ClampUVsBorder) | TextureFlags_ClampUVs;
}
} // namespace WallpaperEngine::Render::Objects
