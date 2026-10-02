#pragma once

#include "TextureProvider.h"

namespace WallpaperEngine::Render {
/** Sampleable native D32 shadow atlas, independent of color render targets. */
class DepthTarget final : public TextureProvider {
public:
    DepthTarget (uint32_t width, uint32_t height);
    ~DepthTarget () override;
    DepthTarget (const DepthTarget&) = delete;
    DepthTarget& operator= (const DepthTarget&) = delete;
    void resize (uint32_t width, uint32_t height);
    void clear () const;
    [[nodiscard]] GLuint getFramebuffer () const { return m_framebuffer; }
    GLuint getTextureID (uint32_t) const override { return m_texture; }
    uint32_t getTextureWidth (uint32_t) const override { return uint32_t (m_resolution.x); }
    uint32_t getTextureHeight (uint32_t) const override { return uint32_t (m_resolution.y); }
    uint32_t getRealWidth () const override { return getTextureWidth (0); }
    uint32_t getRealHeight () const override { return getTextureHeight (0); }
    TextureFormat getFormat () const override { return TextureFormat_UNKNOWN; }
    uint32_t getFlags () const override { return TextureFlags_ClampUVsBorder; }
    const std::vector<FrameSharedPtr>& getFrames () const override { return m_frames; }
    const glm::vec4* getResolution () const override { return &m_resolution; }
    bool isAnimated () const override { return false; }
    uint32_t getSpritesheetCols () const override { return 0; }
    uint32_t getSpritesheetRows () const override { return 0; }
    uint32_t getSpritesheetFrames () const override { return 0; }
    float getSpritesheetDuration () const override { return 0; }
    bool isReady () const override { return m_texture != GL_NONE; }
    void incrementUsageCount () const override { }
    void decrementUsageCount () const override { }
    void update () const override { }
private:
    GLuint m_texture = GL_NONE, m_framebuffer = GL_NONE;
    glm::vec4 m_resolution {};
    std::vector<FrameSharedPtr> m_frames;
};
} // namespace WallpaperEngine::Render
