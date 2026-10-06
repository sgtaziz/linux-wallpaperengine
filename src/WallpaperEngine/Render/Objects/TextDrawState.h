#pragma once

#include <GL/glew.h>

namespace WallpaperEngine::Render::Objects {

// Native text draws through the shipped materials/fonts/*.json, which are all
// nocull. Text draws outside CPass, so it must clear face culling itself
// instead of inheriting the cull state a preceding model pass left behind.
// Depth state is deliberately left as the last pass set it.
inline void applyTextDrawState () {
    glDisable (GL_CULL_FACE);
}

} // namespace WallpaperEngine::Render::Objects
