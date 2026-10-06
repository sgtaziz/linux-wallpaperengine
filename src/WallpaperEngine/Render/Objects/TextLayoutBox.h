#pragma once

namespace WallpaperEngine::Render::Objects {
/** Native layout bounds, y up with the first row's baseline at 0.
 * The background, effect target and layer size are built on this box, not
 * on the glyph raster. */
struct TextLayoutBox {
    float minX = 0.0f;
    float maxX = 0.0f;
    float minY = 0.0f;
    float maxY = 0.0f;
    [[nodiscard]] float width () const { return maxX - minX; }
    [[nodiscard]] float height () const { return maxY - minY; }
    [[nodiscard]] float centerX () const { return (minX + maxX) * 0.5f; }
    [[nodiscard]] float centerY () const { return (minY + maxY) * 0.5f; }
};
} // namespace WallpaperEngine::Render::Objects
