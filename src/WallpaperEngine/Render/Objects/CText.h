#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include <GL/glew.h>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>

#include "WallpaperEngine/Render/Objects/CRenderable.h"
#include "WallpaperEngine/Scripting/ScriptEngine.h"
#include "WallpaperEngine/Scripting/ScriptableObject.h"

// Forward-declare FreeType types to avoid leaking the header into users.
struct FT_LibraryRec_;
struct FT_FaceRec_;
typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;

namespace WallpaperEngine::Render::Wallpapers {
class CScene;
}

namespace WallpaperEngine::Render::Objects {
using namespace WallpaperEngine::Data::Model;

/**
 * FreeType-backed scene text. Embedded fonts and script-driven text updates
 * are supported; paragraph bidi, native line breaking and complete effect
 * semantics remain open.
 */
class CText final : public CRenderable, public Scripting::ScriptableObject {
public:
    CText (Wallpapers::CScene& scene, const Text& text);
    ~CText () override;

    void setup () override;
    void render () override;
    bool executeMaterialFunction (const std::string& name);
    [[nodiscard]] glm::vec2 getLayoutSize () const { return m_quadSize; }
    [[nodiscard]] glm::vec2 getLayoutOffset () const;
    [[nodiscard]] const std::string& getHorizontalAlign () const { return m_horizontalAlign; }
    [[nodiscard]] const std::string& getVerticalAlign () const { return m_verticalAlign; }
    void setHorizontalAlign (std::string alignment) { m_horizontalAlign = std::move (alignment); }
    void setVerticalAlign (std::string alignment) { m_verticalAlign = std::move (alignment); }
    [[nodiscard]] const float& getBrightness () const override;
    [[nodiscard]] const float& getUserAlpha () const override;
    [[nodiscard]] const float& getAlpha () const override;
    [[nodiscard]] const glm::vec3& getColor () const override;
    [[nodiscard]] const glm::vec4& getColor4 () const override;
    [[nodiscard]] const glm::vec3& getCompositeColor () const override;

private:
    // Rebuilds the glyph texture (and matching quad VBO) from the given string.
    // Reuses existing GL handles if already allocated, so this is safe to call
    // every time the rendered text changes.
    void rebuildTextureFrom (const std::string& text);
    void buildShader ();
    void uploadQuadVertices ();

    // setup() helpers (kept small to keep the setup flow linear).
    bool initFreeType ();
    bool loadEmbeddedFont ();
    bool loadSystemFont ();
    FT_Face faceForCodepoint (char32_t codepoint);
    long computeCharacterSize26_6 () const;
    void setupEffects ();
    void resizeEffectTargets ();
    bool hasVisibleEffects () const;
    std::shared_ptr<const TextureProvider> runEffects ();

    const Text& m_text;
    std::string m_horizontalAlign;
    std::string m_verticalAlign;
    std::string m_lastRenderedText;
    long m_lastCharacterSize26_6 = 0;
    glm::vec2 m_lastSpacing = { 0.0f, 0.0f };
    glm::vec3 m_lastRasterColor = { -1.0f, -1.0f, -1.0f };
    glm::vec3 m_lastRasterBackgroundColor = { -1.0f, -1.0f, -1.0f };
    float m_lastRasterOpacity = -1.0f;
    bool m_lastOpaqueBackground = false;
    bool m_lastOffscreenGlyphBlend = false;
    int m_lastMaxRows = 0;
    bool m_lastLimitRows = false;
    float m_lastMaxWidth = 0.0f;
    bool m_lastLimitWidth = false;
    bool m_lastLimitUseEllipsis = false;

    FT_Library m_ftLibrary = nullptr;
    FT_Face m_ftFace = nullptr;
    FT_Face m_bundledEmojiFace = nullptr;
    bool m_bundledEmojiAttempted = false;
    std::vector<uint8_t> m_bundledEmojiData;
    std::vector<FT_Face> m_fallbackFaces;
    std::unordered_map<char32_t, FT_Face> m_characterFaces;
    std::unordered_map<std::string, FT_Face> m_fontPathFaces;
    std::vector<uint8_t> m_fontData;

    GLuint m_glyphTexture = 0;
    GLuint m_glyphTransmittanceTexture = 0;
    GLuint m_program = 0;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;

    GLint m_uMVP = -1;
    GLint m_uColor = -1;
    GLint m_uNodeAlpha = -1;
    GLint m_uTexture = -1;
    GLint m_uFlipV = -1;
    GLint m_uOpaqueBackground = -1;
    GLint m_uBackgroundColor = -1;
    GLint m_uGlyphUvOffset = -1;
    GLint m_uGlyphUvScale = -1;
    GLint m_uEffectFinal = -1;
    GLint m_uOffscreenBackdrop = -1;
    GLint m_uBackdropTexture = -1;
    GLint m_uTransmittanceTexture = -1;
    GLint m_uBackdropMVP = -1;

    std::shared_ptr<CFBO> m_textTarget;
    std::shared_ptr<CFBO> m_effectMain;
    std::shared_ptr<CFBO> m_effectSub;
    std::vector<std::shared_ptr<FBOProvider>> m_effectProviders;
    struct EffectStep {
        const ImageEffect* effect = nullptr;
        std::shared_ptr<FBOProvider> provider;
        std::unique_ptr<Effects::CPass> pass;
        std::optional<PassCommandType> command;
        std::string source;
        std::string target;
    };
    std::vector<EffectStep> m_effectSteps;
    struct SizedTarget {
        const FBO* descriptor;
        std::shared_ptr<FBOProvider> provider;
    };
    std::vector<SizedTarget> m_sizedTargets;
    std::vector<MaterialPassUniquePtr> m_virtualEffectMaterials;
    GLuint m_passPosition = 0;
    GLuint m_passTexCoord = 0;
    glm::mat4 m_effectIdentity = glm::mat4 (1.0f);
    glm::vec4 m_effectiveColor4 = glm::vec4 (1.0f);
    float m_effectBrightness = 1.0f;
    float m_effectUnitAlpha = 1.0f;
    glm::ivec2 m_effectTargetSize = { 0, 0 };

    glm::ivec2 m_textureSize = { 0, 0 };
    glm::vec2 m_quadSize = { 0.0f, 0.0f };
    int m_layoutRows = 1;
    float m_layoutLineHeight = 1.0f;

    bool m_valid = false;
};
} // namespace WallpaperEngine::Render::Objects
