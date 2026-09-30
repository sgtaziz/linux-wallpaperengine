#pragma once

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <optional>

#include "WallpaperEngine/Audio/AudioContext.h"

#include "WallpaperEngine/Render/CFBO.h"
#include "WallpaperEngine/Render/Helpers/ContextAware.h"
#include "WallpaperEngine/Render/RenderContext.h"

#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/Media/MediaSource.h"

#include "FBOProvider.h"
#include "WallpaperState.h"

namespace WallpaperEngine::Application {
class WallpaperApplication;
}

namespace WallpaperEngine::WebBrowser {
class WebBrowserContext;
}

namespace WallpaperEngine::Render {
namespace Helpers {
    class ContextAware;
}

using namespace WallpaperEngine::Render;
using namespace WallpaperEngine::Audio;
using namespace WallpaperEngine::Data::Model;
using namespace WallpaperEngine::FileSystem;

class CWallpaper : public Helpers::ContextAware, public FBOProvider, public TypeCaster {
    friend class WallpaperEngine::Application::WallpaperApplication;

public:
    /** Information for span-mode rendering: one wallpaper across multiple viewports */
    struct SpanInfo {
	/** Bounding box of the entire span group (x, y, width, height) in global desktop coordinates */
	glm::ivec4 totalBounds;
    };

    virtual ~CWallpaper () override;

    /**
     * Performs a render pass of the wallpaper
     */
    void render (
	const glm::ivec4& viewport, const bool vflip, const glm::ivec2& globalPosition = { 0, 0 },
	const glm::ivec2& logicalSize = { 0, 0 }
    );

    /**
     * Pause the renderer
     */
    virtual void setPause (bool newState);

    /**
     * @return The container to resolve files for this wallpaper
     */
    [[nodiscard]] const AssetLocator& getAssetLocator () const;

    /**
     * @return The current audio context for this wallpaper
     */
    AudioContext& getAudioContext () const;

    /**
     * @return The wallpaper state
     */
    [[nodiscard]] const WallpaperState& getState () const;

    /**
     * @return The scene's framebuffer
     */
    [[nodiscard]] virtual GLuint getWallpaperFramebuffer () const;
    /**
     * @return The scene's texture
     */
    [[nodiscard]] virtual GLuint getWallpaperTexture () const;
    /**
     * Searches the FBO list for the given FBO
     *
     * @param name
     * @return
     */
    [[nodiscard]] std::shared_ptr<const CFBO> findFBO (const std::string& name) const;

    /**
     * @return The main FBO of this wallpaper
     */
    [[nodiscard]] std::shared_ptr<const CFBO> getFBO () const;

    /**
     * Updates the UVs coordinates if window/screen/vflip/projection has changed
     */
    void updateUVs (const glm::ivec4& viewport, const bool vflip);

    /**
     * Updates the destination framebuffer for this wallpaper
     *
     * @param framebuffer
     */
    void setDestinationFramebuffer (GLuint framebuffer);

    /**
     * Sets span info for this wallpaper, enabling span-mode rendering
     */
    void setSpanInfo (const SpanInfo& spanInfo);

    /**
     * @return The span info if set, or nullptr
     */
    [[nodiscard]] const SpanInfo* getSpanInfo () const;

    /**
     * @return The width of this wallpaper
     */
    [[nodiscard]] virtual int getWidth () const = 0;

    /**
     * @return The height of this wallpaper
     */
    [[nodiscard]] virtual int getHeight () const = 0;

    /**
     * Creates a new instance of CWallpaper based on the information provided by the read backgrounds
     *
     * @param wallpaper
     * @param context
     * @param audioContext
     * @param scalingMode
     *
     * @return
     */
    static std::unique_ptr<CWallpaper> fromWallpaper (
	const Wallpaper& wallpaper, RenderContext& context, AudioContext& audioContext,
	WebBrowser::WebBrowserContext* browserContext, const WallpaperState::TextureUVsScaling& scalingMode,
	const uint32_t& clampMode
    );

protected:
    CWallpaper (
	const Wallpaper& wallpaperData, RenderContext& context, AudioContext& audioContext,
	const WallpaperState::TextureUVsScaling& scalingMode, const uint32_t& clampMode
    );

    /**
     * Renders a frame of the wallpaper
     */
    virtual void renderFrame (const glm::ivec4& viewport) = 0;

    /** Scene roots already apply presentation scaling in their projection. */
    [[nodiscard]] virtual bool rendersAtOutputSize () const { return false; }

    /**
     * Setups OpenGL's framebuffers for ping-pong and scene rendering
     */
    void setupFramebuffers (TextureFormat format = TextureFormat_ARGB8888);

    struct HdrBloomSettings {
	float strength = 2.0f;
	float threshold = 1.0f;
	float feather = 0.1f;
	float scatter = 1.619f;
	int iterations = 8;
	glm::vec3 tint {1.0f};
    };
    /** Configure native-style float bloom pyramid and SDR output combine. */
    void setHdrPresentation (const HdrBloomSettings& settings);
    void resizeHdrPresentation (uint32_t width, uint32_t height);
    void updateHdrBloomSettings (const HdrBloomSettings& settings);
    void combineHdrFrame ();

    const Wallpaper& m_wallpaperData;

    [[nodiscard]] const Wallpaper& getWallpaperData () const;

    /** The FBO used for scene output */
    std::shared_ptr<CFBO> m_sceneFBO = nullptr;
    std::shared_ptr<CFBO> m_hdrOutput = nullptr;

    GLuint m_vaoBuffer = GL_NONE;

private:
    /** The texture used for the scene output */
    GLuint m_texCoordBuffer = GL_NONE;
    GLuint m_positionBuffer = GL_NONE;
    GLuint m_shader = GL_NONE;
    GLuint m_hdrShader = GL_NONE;
    GLuint m_hdrPyramidShader = GL_NONE;
    GLuint m_hdrPositionBuffer = GL_NONE;
    std::shared_ptr<const CFBO> m_hdrBloom = nullptr;
    std::vector<std::shared_ptr<CFBO>> m_hdrPyramid;
    HdrBloomSettings m_hdrBloomSettings;
    size_t m_hdrActiveLevels = 0;
    bool m_hdrPeekDone = false;
    GLint m_hdrTexture0 = GL_NONE;
    GLint m_hdrTexture1 = GL_NONE;
    GLint m_hdrTexelSize = GL_NONE;
    GLint m_hdrPosition = GL_NONE;
    GLint m_hdrTexCoord = GL_NONE;
    // shader variables
    GLint g_Texture0 = GL_NONE;
    GLint a_Position = GL_NONE;
    GLint a_TexCoord = GL_NONE;
    /** The framebuffer to draw the background to */
    GLuint m_destFramebuffer = GL_NONE;
    /** Setups OpenGL's shaders for this wallpaper backbuffer */
    void setupShaders ();
    /** List of FBOs registered for this wallpaper */
    std::map<std::string, std::shared_ptr<const CFBO>> m_fbos = {};
    /** Audio context that is using this wallpaper */
    AudioContext& m_audioContext;
    /** Current Wallpaper state */
    WallpaperState m_state;
    /** Span info for multi-monitor spanning (optional) */
    std::optional<SpanInfo> m_spanInfo = std::nullopt;
    /** Frame counter to avoid redundant renderFrame calls when shared across viewports */
    uint32_t m_lastRenderedFrame = UINT32_MAX;
};
} // namespace WallpaperEngine::Render
