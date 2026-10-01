#pragma once

#include "Helpers/ContextAware.h"
#include "TextureProvider.h"
#include "TextureAnimation.h"
#include "WallpaperEngine/Data/Assets/Texture.h"
#include "WallpaperEngine/VideoPlayback/MPV/GLPlayer.h"

#include <GL/glew.h>
#include <glm/vec4.hpp>
#include <memory>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#include <string>
#include <vector>

namespace WallpaperEngine::Render {
class RenderContext;
using namespace WallpaperEngine::Data::Assets;
using namespace WallpaperEngine::VideoPlayback::MPV;
/**
 * A normal texture file in WallpaperEngine's format
 */
class CTexture final : public TextureProvider, public Helpers::ContextAware {
public:
    explicit CTexture (RenderContext& context, TextureUniquePtr header, std::string assetName = {});
    ~CTexture () override;

    [[nodiscard]] GLuint getTextureID (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getTextureWidth (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getTextureHeight (uint32_t imageIndex) const override;
    [[nodiscard]] uint32_t getRealWidth () const override;
    [[nodiscard]] uint32_t getRealHeight () const override;
    [[nodiscard]] TextureFormat getFormat () const override;
    [[nodiscard]] uint32_t getFlags () const override;
    [[nodiscard]] const glm::vec4* getResolution () const override;
    [[nodiscard]] const std::vector<FrameSharedPtr>& getFrames () const override;
    [[nodiscard]] bool isAnimated () const override;
    [[nodiscard]] uint32_t getSpritesheetCols () const override;
    [[nodiscard]] uint32_t getSpritesheetRows () const override;
    [[nodiscard]] uint32_t getSpritesheetFrames () const override;
    [[nodiscard]] float getSpritesheetDuration () const override;
    [[nodiscard]] const std::shared_ptr<SharedTextureAnimation>& getAnimationPlayback () const {
        return m_animationPlayback;
    }

    /**
     * Increments the usage count of the texture
     *
     * Directly controls playback for video CTextures, only started when at least one thing is using it
     * Initializes mpv if needed and starts playback
     */
    void incrementUsageCount () const override;
    /**
     * Decrements the usage count of the texture
     *
     * Directly controls playback for video CTextures, only stopped when nothing is using it
     * De-initializes mpv if needed
     */
    void decrementUsageCount () const override;
    /**
     * Some textures need to be updated
     */
    void update () const override;
    bool isReady () const override;

    /** Uploads one parsed mipmap. Exposed for GL readback tests of the production upload path. */
    static void uploadLevel (const Texture& header, const Mipmap& mipmap, GLint internalFormat,
                             GLuint textureID, GLint level, const std::string& assetName);
    /** Selects the storage format used by the production texture constructor. */
    static GLint storageFormat (const Texture& header);
    static void configureSampling (const Texture& header, uint32_t imageIndex, GLuint textureID);

private:
    /**
     * @return The texture header
     */
    [[nodiscard]] const Texture& getHeader () const;

    /**
     * Calculate's texture's resolution vec4
     */
    void setupResolution ();
    /**
     * Prepares openGL parameters for loading texture data
     */
    void setupOpenGLParameters (uint32_t textureID) const;

    /** The texture header */
    TextureUniquePtr m_header;
    std::shared_ptr<SharedTextureAnimation> m_animationPlayback;
    /** OpenGL's texture ID */
    GLuint* m_textureID = nullptr;
    /** Resolution vector of the texture */
    glm::vec4 m_resolution {};
    /** The video player in use */
    GLPlayerUniquePtr m_player;
    std::string m_assetName;
};
} // namespace WallpaperEngine::Assets
