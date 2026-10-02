#pragma once

#include <string>
#include <optional>
#include <memory>
#include <glm/vec4.hpp>

#include "TextureProvider.h"

using namespace WallpaperEngine::Render;

namespace WallpaperEngine::Render {
class ReflectionMipmapGenerator;
class CFBO final : public TextureProvider {
public:
    CFBO (
	std::string name, const TextureFormat format, const uint32_t flags, const float scale, uint32_t realWidth,
	uint32_t realHeight, uint32_t textureWidth, uint32_t textureHeight,
        std::optional<glm::vec4> clearColor = std::nullopt
    );
    ~CFBO () override;

    [[nodiscard]] const std::string& getName () const;
    [[nodiscard]] const float& getScale () const;
    [[nodiscard]] TextureFormat getFormat () const override;
    [[nodiscard]] uint32_t getFlags () const override;
    [[nodiscard]] GLuint getFramebuffer () const;
    [[nodiscard]] GLuint getDepthbuffer () const;
    /** Attach D16 storage only where the scene target contract requires it. */
    void attachDepth16 ();
    /** Native secondary target may share the primary target depth attachment. */
    void attachSharedDepth (std::shared_ptr<const CFBO> source);
    void clear (const glm::vec4& color) const;
    void resize (uint32_t realWidth, uint32_t realHeight, uint32_t textureWidth, uint32_t textureHeight);
    /** Keep the render-target sampler choice when resize replaces its GL texture. */
    void setMaxAnisotropy (float value);
    /** Enable the native, limited scene-reflection mip chain. */
    void enableSceneReflectionMipmaps ();
    /** Copy a completed scene before presentation; consumers use this next frame. */
    void snapshotFrom (const CFBO& source);
    [[nodiscard]] uint32_t getMipLevelCount (uint32_t imageIndex) const override;
    [[nodiscard]] GLuint getTextureID (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getTextureWidth (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getTextureHeight (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getRealWidth () const override;
    [[nodiscard]] uint32_t getRealHeight () const override;
    [[nodiscard]] const std::vector<FrameSharedPtr>& getFrames () const override;
    [[nodiscard]] const glm::vec4* getResolution () const override;
    [[nodiscard]] bool isAnimated () const override;
    [[nodiscard]] uint32_t getSpritesheetCols () const override;
    [[nodiscard]] uint32_t getSpritesheetRows () const override;
    [[nodiscard]] uint32_t getSpritesheetFrames () const override;
    [[nodiscard]] float getSpritesheetDuration () const override;

    void incrementUsageCount () const override;
    void decrementUsageCount () const override;
    void update () const override;
    bool isReady () const override;

private:
    GLuint m_framebuffer = GL_NONE;
    GLuint m_depthbuffer = GL_NONE;
    std::shared_ptr<const CFBO> m_depthSource;
    GLuint m_texture = GL_NONE;
    glm::vec4 m_resolution = {};
    float m_scale = 0;
    std::string m_name = "";
    TextureFormat m_format = TextureFormat_UNKNOWN;
    uint32_t m_flags = TextureFlags_NoFlags;
    float m_maxAnisotropy = 8.0f;
    bool m_sceneReflectionMipmaps = false;
    uint32_t m_mipLevelCount = 1;
    std::unique_ptr<ReflectionMipmapGenerator> m_reflectionGenerator;
    /** Placeholder for frames, FBOs only have ONE */
    std::vector<FrameSharedPtr> m_frames = {};
};
} // namespace WallpaperEngine::Render
