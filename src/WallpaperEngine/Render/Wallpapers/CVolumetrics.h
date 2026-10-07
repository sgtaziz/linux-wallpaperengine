#pragma once

#include <memory>
#include <vector>

#include <GL/glew.h>
#include <glm/glm.hpp>

#include "VolumetricLight.h"
#include "WallpaperEngine/Assets/AssetLocator.h"

namespace WallpaperEngine::Render::Wallpapers {
/**
 * Native point and spot light volumetrics.
 *
 * Each volumetric light draws its volume (sphere or spot cone) twice: the back surface into
 * _rt_volumetricsBack, then the front surface through the shipped
 * volumetricsfront ray march, additively into a reduced _rt_volumetricsLightBuffer.
 * The buffer is cleared by the first light of a batch and added onto the
 * current scene target before the next non-light object.
 *
 * Linux stores the back and limit depths as normalized device depth in R32F
 * colour targets, so the shipped comparison against v_ScreenPos.z holds under
 * the OpenGL depth convention. The back pass mirrors device Y because the
 * shipped shader samples screen textures with the D3D (0.5, -0.5) mapping.
 */
class CVolumetrics {
public:
    explicit CVolumetrics (const Assets::AssetLocator& assets);
    ~CVolumetrics ();
    CVolumetrics (const CVolumetrics&) = delete;
    CVolumetrics& operator= (const CVolumetrics&) = delete;

    /**
     * Ray-march one point volume into the light buffer.
     *
     * @param shape the volume mesh (sphere for point lights, cone for spots)
     * @param fullscreen the eye is inside the volume: native draws volumetrics_fullscreen
     *        (a full-screen triangle starting the march at the near plane) instead of the front faces
     * @param viewProjection the scene projection for authored-basis geometry
     * @param targetSize the scene target size (the back depth resolution)
     * @param hdr native allocates the light buffer as RGBA16F for HDR scene targets
     */
    void accumulate (
        const VolumeUniforms& volume, VolumeShape shape, bool fullscreen, const glm::mat4& viewProjection,
        glm::ivec2 targetSize, bool hdr);
    /** Add the pending light buffer onto the bound target (native combine pass) and close the batch. */
    void flush (GLuint framebuffer, glm::ivec2 targetSize);
    [[nodiscard]] bool pending () const { return m_pending; }

private:
    void ensureTargets (glm::ivec2 targetSize, bool hdr);
    void ensurePrograms ();
    GLuint frontProgram (bool point, bool fullscreen);

    const Assets::AssetLocator& m_assets;
    bool m_pending = false;
    glm::ivec2 m_targetSize {0};
    bool m_hdr = false;
    GLuint m_backFramebuffer = 0, m_backTexture = 0, m_backDepth = 0;
    GLuint m_limitTexture = 0, m_limitFramebuffer = 0;
    GLuint m_lightFramebuffer = 0, m_lightTexture = 0;
    GLuint m_vao = 0, m_sphereVertices = 0, m_sphereIndices = 0, m_quadVertices = 0;
    GLuint m_coneVertices = 0, m_coneIndices = 0, m_triangleVertices = 0;
    GLsizei m_sphereIndexCount = 0, m_coneIndexCount = 0;
    GLuint m_backProgram = 0, m_combineProgram = 0;
    /** volumetricsfront variants by [POINTLIGHT][FULLSCREEN], compiled on first use. */
    GLuint m_frontPrograms[2][2] = {};
};
} // namespace WallpaperEngine::Render::Wallpapers
