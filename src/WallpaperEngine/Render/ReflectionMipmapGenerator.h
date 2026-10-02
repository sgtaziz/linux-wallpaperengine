#pragma once

#include <GL/glew.h>
#include <cstdint>

namespace WallpaperEngine::Render {
/** Reduction used only by the retained scene-reflection snapshot. */
class ReflectionMipmapGenerator final {
public:
    ReflectionMipmapGenerator ();
    ~ReflectionMipmapGenerator ();
    ReflectionMipmapGenerator (const ReflectionMipmapGenerator&) = delete;
    ReflectionMipmapGenerator& operator= (const ReflectionMipmapGenerator&) = delete;

    void generate (GLuint texture, uint32_t width, uint32_t height,
                   uint32_t levels, bool normalized);

private:
    GLuint m_program = 0;
    GLuint m_vertexArray = 0;
    GLuint m_scratch[2] {};
    GLuint m_framebuffers[2] {};
    uint32_t m_width = 0, m_height = 0, m_levels = 0;
};
} // namespace WallpaperEngine::Render
