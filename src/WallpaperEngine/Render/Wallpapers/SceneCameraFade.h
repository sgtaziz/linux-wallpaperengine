#pragma once

#include <GL/glew.h>
#include <glm/vec3.hpp>

namespace WallpaperEngine::Render::Wallpapers {

/**
 * The root camera-path transition fade: a full-frame
 * translucent quad of materials/util/fade.json, whose fragment is
 * vec4(color * 0.7, g_Alpha) with color the project's scheme colour, drawn over
 * the finished frame. Native loads the material only at scene creation when
 * camerafade is on and the scene has camera paths.
 */
class SceneCameraFade {
public:
    /** The tint default of shaders/fade.frag when the project has no scheme colour. */
    static constexpr glm::vec3 kDefaultSchemeColor {0.315f, 0.135f, 0.1125f};

    SceneCameraFade () = default;
    SceneCameraFade (const SceneCameraFade&) = delete;
    SceneCameraFade& operator= (const SceneCameraFade&) = delete;
    ~SceneCameraFade ();

    /** Blend the fade over the bound target's full viewport; alpha <= 0 draws nothing. */
    void draw (GLuint framebuffer, int width, int height, const glm::vec3& schemeColor, float alpha);

private:
    void create ();

    GLuint m_program = GL_NONE;
    GLuint m_vertexArray = GL_NONE;
    GLuint m_positions = GL_NONE;
    GLint m_color = -1;
    GLint m_alpha = -1;
};

} // namespace WallpaperEngine::Render::Wallpapers
